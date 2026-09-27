"""Patching a bigfile in place: replace or add files by appending them, without rewriting gigabytes.

How the RGH PC executable reads the file table (BIG::b_Open 006BCC90, BIG::ReadFatFiles 006BCF90): the 0x14D8-byte
header is loaded as is; `u32 +0x0C` file-table blocks are read, the first at `i64 +0x14` ({hi, lo}), each block
`{u32 count; i64 next; count x 200-byte records}`, the next block at the previous block's `next`; records are taken in
chain order until `u32 +0x2C` (numFiles) records have been read, so the slots of a block beyond that total are unused
capacity; the key map is sized by `u32 +0x24` (maxFiles); for each record only name, u32 length on disk (+0x58), key
(+0x64), i64 position (+0x68) and permission flags (+0x70) are kept, and records whose position low word is
0xFFFFFFFF are skipped.  Directory records are not used by the executable.  A file is the 32-byte file header
(u32 length, u32 user length, u32 reference bytes, u32 flags, 12 unused bytes, 0xFFFFFFFF) + payload + reference table;
BIG_S_P4::b_ReadFilePriv (006BDE40): flags & 1 shortcut, flags & 4 LZO block stream, otherwise plain LZO1X when
length < user length, raw when equal.  A bigfile whose name starts with "RGH" gets BIG flag 1.  Shadowed extensions
(sns, bik) open "<bigfile base>.<branch>.<ext>.bf" next to it (Shadow_u32_GetShadowBFIndex).

Replacing a file appends it and rewrites its record in place (the original bytes stay as dead space).  Adding files
appends them; their records fill the unused slots after numFiles first (a sibling bigfile can have far more slots
than files, and a new block there would never be read), then one new block per commit linked from the last block,
with the header counts raised.

Bin-load files (the per-world bins FFF/FEF/FDF/FCF/FBF: "BIG_ChunkDecompress" 006CF0E0 maps world key + loading mode
3 + type 1..5 to them) are streamed by the binary loader (FUN_006cf650 / FUN_006cf850, not b_ReadFilePriv): it takes a
file raw, or as an LZO chunk stream when header flags & 4.  A plain LZO1X payload (flags 0, length < user length) is
read as raw bytes and the load never ends, so `put` stores raw by default; plain LZO (compress=True) is only for files
read through b_ReadFilePriv.
"""
from __future__ import annotations

import os
import shutil
import struct

from . import lzo
from .bigfile import MAGIC
from .writer import byte_sum

HDR_SIZE = 0x14D8
REC = 200
FH = 32


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _i64(b, o):
    return (_u32(b, o) << 32) | _u32(b, o + 4)


def _p64(v):
    return struct.pack("<II", (v >> 32) & 0xFFFFFFFF, v & 0xFFFFFFFF)


class BigPatch:
    def __init__(self, path: str, filetime: int = 0):
        self.path = path
        self.filetime = filetime
        self.f = open(path, "r+b")
        self.f.seek(0, 2)
        self.size = self.f.tell()
        self.f.seek(0)
        self.hdr = bytearray(self.f.read(HDR_SIZE))
        if _u32(self.hdr, 0) != MAGIC:
            raise ValueError(f"{path}: not a bigfile")
        self.blocks = []                     # (block position, count) in chain order
        self.recpos: dict[int, int] = {}     # key -> file offset of its 200-byte record
        num = _u32(self.hdr, 0x2C)
        pos = _i64(self.hdr, 0x14)
        left = num
        for _ in range(_u32(self.hdr, 0x0C)):
            self.f.seek(pos)
            head = self.f.read(12)
            cnt, nxt = _u32(head, 0), _i64(head, 4)
            self.blocks.append((pos, cnt))
            take = min(cnt, left)
            self.f.seek(pos + 12)
            recs = self.f.read(take * REC)
            for i in range(take):
                r = recs[i * REC:(i + 1) * REC]
                if _u32(r, 0x6C) != 0xFFFFFFFF and r[0]:
                    self.recpos.setdefault(_u32(r, 0x64), pos + 12 + i * REC)
            left -= take
            pos = nxt
        self.num = num                       # records the executable reads (header +0x2C)
        self.new: list[bytes] = []           # records of files added since the last commit

    def slot_offsets(self, start: int, count: int) -> list[int]:
        """File offsets of the record slots `start` .. `start + count - 1` in chain order (existing blocks only)."""
        out = []
        idx = 0
        for pos, cnt in self.blocks:
            for i in range(cnt):
                if idx >= start + count:
                    return out
                if idx >= start:
                    out.append(pos + 12 + i * REC)
                idx += 1
        return out

    # ---------------------------------------------------------------- writing
    def _append(self, blob: bytes) -> int:
        self.f.seek(0, 2)
        at = self.f.tell()
        self.f.write(blob)
        self.size = at + len(blob)
        return at

    @staticmethod
    def file_blob(data: bytes, refs: bytes = b"", compress: bool = True) -> tuple[bytes, int]:
        payload = data
        if compress and len(data) >= 64:
            c = lzo.compress(data)
            if len(c) < len(data):
                payload = c
        head = struct.pack("<IIII", len(payload), len(data), len(refs), 0) + bytes(12) + struct.pack("<I", 0xFFFFFFFF)
        return head + payload + refs, len(payload)

    def put(self, key: int, name: str, data: bytes, refs: bytes | list = b"", compress: bool = False,
            perm_flags: int = 0x0A00, flags: int | None = None, length_user: int | None = None):
        """Replace the file of `key` or add it.  `flags` / `length_user` store `data` verbatim with that file header
        (a shadow shortcut: flags 3, the 1024-byte link payload, user length = the shadowed file's size)."""
        if isinstance(refs, list):
            refs = b"".join(r.pack() if hasattr(r, "pack") else struct.pack("<III", *r) for r in refs)
        if flags is not None:
            lu = len(data) if length_user is None else length_user
            blob = struct.pack("<IIII", len(data), lu, len(refs), flags) + bytes(12) + struct.pack("<I", 0xFFFFFFFF) \
                + data + refs
            return self._record(key, name, blob, lu, data, perm_flags, checksum=0)
        blob, _ = self.file_blob(data, refs, compress)
        return self._record(key, name, blob, len(data), data, perm_flags)

    def _record(self, key: int, name: str, blob: bytes, length_user: int, data: bytes, perm_flags: int,
                checksum: int | None = None):
        csum = byte_sum(data) if checksum is None else checksum
        at = self._append(blob)
        nm = name.encode("latin-1")[:63]
        rp = self.recpos.get(key)
        if rp is not None and rp >= 0:
            self.f.seek(rp)
            r = bytearray(self.f.read(REC))
            r[0:64] = nm + bytes(64 - len(nm))
            struct.pack_into("<I", r, 0x58, len(blob))
            struct.pack_into("<Q", r, 0x5C, self.filetime)
            r[0x68:0x70] = _p64(at)
            struct.pack_into("<I", r, 0x74, length_user)
            struct.pack_into("<I", r, 0x7C, csum)
            self.f.seek(rp)
            self.f.write(r)
            return "replaced"
        r = bytearray(REC)
        r[0:len(nm)] = nm
        struct.pack_into("<III", r, 0x50, 0, 0xFFFFFFFF, len(blob))     # directory 0 (Root), no next file
        struct.pack_into("<Q", r, 0x5C, self.filetime)
        struct.pack_into("<I", r, 0x64, key)
        r[0x68:0x70] = _p64(at)
        struct.pack_into("<HHI", r, 0x70, perm_flags, 0, length_user)
        struct.pack_into("<HHI", r, 0x78, 0, 1, csum)
        if rp == -1:                          # added earlier in this commit: replace the pending record
            for i, old in enumerate(self.new):
                if _u32(old, 0x64) == key:
                    self.new[i] = bytes(r)
                    return "replaced (pending)"
        self.new.append(bytes(r))
        self.recpos[key] = -1                 # pending until commit writes it
        return "added"

    def commit(self):
        if self.new:
            capacity = sum(cnt for _, cnt in self.blocks)
            free = self.slot_offsets(self.num, min(len(self.new), capacity - self.num)) if capacity > self.num else []
            for off, r in zip(free, self.new):
                self.f.seek(off)
                self.f.write(r)
                self.recpos[_u32(r, 0x64)] = off
            rest = self.new[len(free):]
            self.num += len(free)
            if rest:
                block = struct.pack("<I", len(rest)) + _p64(0) + b"".join(rest)
                at = self._append(block)
                last_pos, last_cnt = self.blocks[-1]
                self.f.seek(last_pos + 4)
                self.f.write(_p64(at))
                for i, r in enumerate(rest):
                    self.recpos[_u32(r, 0x64)] = at + 12 + i * REC
                self.blocks.append((at, len(rest)))
                self.num += len(rest)
                capacity += len(rest)
            struct.pack_into("<I", self.hdr, 0x0C, len(self.blocks))
            struct.pack_into("<I", self.hdr, 0x24, max(_u32(self.hdr, 0x24), capacity))
            struct.pack_into("<I", self.hdr, 0x2C, self.num)
            self.f.seek(0)
            self.f.write(self.hdr)
            self.new = []
        self.f.flush()

    def close(self):
        self.commit()
        self.f.close()


def clone(src: str, dst: str):
    """Copy a bigfile and its shadow siblings (<base>.<branch>.<ext>.bf) under dst's base name."""
    shutil.copyfile(src, dst)
    sd, sb = os.path.split(src)
    sbase = sb.rsplit(".", 1)[0]
    dbase = os.path.basename(dst).rsplit(".", 1)[0]
    for fn in os.listdir(sd or "."):
        if fn.lower().startswith(sbase.lower() + ".") and fn.lower().endswith(".bf") and fn.lower() != sb.lower():
            shutil.copyfile(os.path.join(sd, fn), os.path.join(os.path.dirname(dst), dbase + fn[len(sbase):]))
