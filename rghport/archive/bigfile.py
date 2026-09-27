"""Reading the version-4 bigfiles (.bf) Rabbids Go Home ships on the Wii and on the PC.

On-disk layout (all little-endian, on the Wii too; header layout from the RGH Wii executable's DWARF):

    header              0x14D8 bytes
    file table chain    { u32 count; i64 next; count x 200-byte file record }
    directory chain     { u32 count; i64 next; count x 100-byte directory record }
    per file:           32-byte file header + payload + reference table

File record (200 bytes):
    +0x00 char name[64]      +0x50 u32 directory      +0x64 u32 key
    +0x40 char user[16]      +0x54 u32 next file      +0x68 i64 position {hi, lo}
                             +0x58 u32 length on disk +0x70 u16 permission flags, u16 volume flags
                             +0x5C FILETIME           +0x74 u32 user length
                             +0x78 u16 ?  +0x7A u16 revision  +0x7C u32 byte-sum checksum
Directory record (100 bytes):
    +0x00 name[64] +0x40 parent +0x44 first sub-directory +0x48 next directory +0x4C first file
    +0x50 u16 permission flags +0x52 u16 volume flags +0x54 revision +0x58 dummy +0x5C revision +0x60 dummy
File header (32 bytes):
    u32 length (payload bytes on disk); u32 user length (decompressed); u32 reference bytes (12-byte references after
    the payload); u32 flags; 12 unused bytes; u32 0xFFFFFFFF
Flags: 0x01 shadow shortcut (the bytes are in a sibling bigfile "<base>.<branch>.<ext>.bf"), 0x04 LZO1X block stream.
length == user length: stored as is; otherwise one LZO1X stream (see lzo.py).
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field

from . import lzo

MAGIC = 0x00454241
HDR_SIZE = 0x14D8
FILE_REC = 200
DIR_REC = 100
FILE_HDR = 32
INVALID = 0xFFFFFFFF

F_SHADOW = 0x01
F_BLOCKS = 0x04


def _u32(d, o): return struct.unpack_from("<I", d, o)[0]
def _u16(d, o): return struct.unpack_from("<H", d, o)[0]
def _i64(d, o): return (_u32(d, o) << 32) | _u32(d, o + 4)
def _cstr(b): return b.split(b"\0")[0].decode("latin-1")


@dataclass
class Ref:
    key: int
    priv: int = 0
    user: int = 0

    def pack(self) -> bytes:
        return struct.pack("<III", self.key, self.priv, self.user)


@dataclass
class Entry:
    name: str
    key: int
    dir: int = 0
    pos: int = 0
    length_disk: int = 0
    length_user: int = 0
    perm_flags: int = 0x0A00
    vol_flags: int = 0
    filetime: int = 0
    revision: int = 0
    checksum: int = 0
    next_file: int = INVALID
    index: int = -1

    @property
    def ext(self) -> str:
        """The extension, lower case.  A bin named by its key alone is a bin: the US disc's archive names its packages,
        texture banks, Magma blobs and language bins "fff09e7b", "163ca8d." where the Japanese one has
        "fff09e7b.bin"."""
        ext = self.name.rsplit(".", 1)[-1].lower() if "." in self.name else ""
        if not ext and self.name.rstrip(".").lower() == "%x" % self.key:
            return "bin"
        return ext


@dataclass
class Dir:
    name: str
    parent: int = INVALID
    first_sub: int = INVALID
    next_dir: int = INVALID
    first_file: int = INVALID
    perm_flags: int = 0x100
    vol_flags: int = 0
    index: int = -1
    children: list = field(default_factory=list)   # directory indices, in stored order
    files: list = field(default_factory=list)      # file indices, in stored order


class Big:
    """Read-only view of a bigfile on disk."""

    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()
        hdr = self._read(0, HDR_SIZE)
        if len(hdr) < 0x54 or _u32(hdr, 0) != MAGIC:
            raise ValueError(f"{path}: not a bigfile")
        self.version = _u32(hdr, 4)
        self.tick = _u32(hdr, 8)
        self.max_files = _u32(hdr, 0x24)
        self.max_dirs = _u32(hdr, 0x28)
        self.num_files = _u32(hdr, 0x2C)
        self.num_dirs = _u32(hdr, 0x30)
        self.params = [_u32(hdr, 0x44 + 4 * i) for i in range(4)]
        self.header = hdr
        self.files: list[Entry] = []
        self.dirs: list[Dir] = []
        self.by_key: dict[int, Entry] = {}
        self._load_fat(_i64(hdr, 0x14), _u32(hdr, 0x0C))
        self._load_dirs(_i64(hdr, 0x1C), _u32(hdr, 0x10))
        self.shadows: dict[str, "Big"] = {}
        self.shadow_dir = os.path.dirname(os.path.abspath(path))

    # ------------------------------------------------------------ parsing
    def _read(self, pos: int, n: int) -> bytes:
        self.f.seek(pos)
        return self.f.read(n)

    def _chain(self, pos: int, nblocks: int, entsize: int):
        out = []
        seen = 0
        visited = set()
        while pos and pos < self.size and seen < max(nblocks, 1) and pos not in visited:
            visited.add(pos)
            head = self._read(pos, 12)
            cnt = _u32(head, 0)
            nxt = _i64(head, 4)
            if cnt > 4_000_000:
                break
            blk = self._read(pos + 12, cnt * entsize)
            for i in range(cnt):
                out.append(blk[i * entsize:(i + 1) * entsize])
            seen += 1
            if nxt == 0 or nxt >= self.size:
                break
            pos = nxt
        return out

    def _load_fat(self, pos, nblocks):
        for i, r in enumerate(self._chain(pos, nblocks, FILE_REC)):
            name = _cstr(r[:64])
            key = _u32(r, 0x64)
            e = Entry(name=name, key=key, dir=_u32(r, 0x50), pos=_i64(r, 0x68),
                      length_disk=_u32(r, 0x58), length_user=_u32(r, 0x74),
                      perm_flags=_u16(r, 0x70), vol_flags=_u16(r, 0x72),
                      filetime=struct.unpack_from("<Q", r, 0x5C)[0],
                      revision=_u16(r, 0x7A), checksum=_u32(r, 0x7C),
                      next_file=_u32(r, 0x54), index=i)
            self.files.append(e)
            if name and e.pos:
                self.by_key.setdefault(key, e)

    def _load_dirs(self, pos, nblocks):
        for i, r in enumerate(self._chain(pos, nblocks, DIR_REC)):
            self.dirs.append(Dir(name=_cstr(r[:64]), parent=_u32(r, 0x40),
                                 first_sub=_u32(r, 0x44), next_dir=_u32(r, 0x48),
                                 first_file=_u32(r, 0x4C), perm_flags=_u16(r, 0x50),
                                 vol_flags=_u16(r, 0x52), index=i))
        for d in self.dirs:
            s = d.first_sub
            n = 0
            while s != INVALID and s < len(self.dirs) and n < 100000:
                d.children.append(s)
                s = self.dirs[s].next_dir
                n += 1
            fidx = d.first_file
            n = 0
            while fidx != INVALID and fidx < len(self.files) and n < 10_000_000:
                d.files.append(fidx)
                fidx = self.files[fidx].next_file
                n += 1

    # ------------------------------------------------------------ queries
    def dir_path(self, idx: int) -> str:
        parts = []
        n = 0
        while idx != INVALID and idx < len(self.dirs) and n < 64:
            parts.append(self.dirs[idx].name)
            idx = self.dirs[idx].parent
            n += 1
        return "/".join(reversed(parts))

    def entries(self) -> list[Entry]:
        """Live entries (name set, position set), in file table order."""
        return [e for e in self.files if e.name and e.pos]

    def find(self, key: int) -> Entry | None:
        return self.by_key.get(key)

    def find_name(self, name: str) -> Entry | None:
        nl = name.lower()
        for e in self.files:
            if e.name.lower() == nl:
                return e
        return None

    def file_header(self, e: Entry):
        """(length, user length, reference bytes, flags) of an entry."""
        fh = self._read(e.pos, FILE_HDR)
        return struct.unpack_from("<IIII", fh, 0)

    def raw(self, e: Entry):
        """(header tuple, payload bytes, reference bytes) exactly as stored."""
        length, lu, lr, flags = self.file_header(e)
        payload = self._read(e.pos + FILE_HDR, length)
        refs = self._read(e.pos + FILE_HDR + length, lr)
        return (length, lu, lr, flags), payload, refs

    def refs(self, e: Entry) -> list[Ref]:
        length, lu, lr, flags = self.file_header(e)
        raw = self._read(e.pos + FILE_HDR + length, lr)
        return [Ref(*struct.unpack_from("<III", raw, i)) for i in range(0, lr - lr % 12, 12)]

    def read(self, e: Entry, follow_shadow: bool = True) -> bytes:
        (length, lu, lr, flags), payload, _ = self.raw(e)
        if flags & F_SHADOW:
            if not follow_shadow:
                return payload
            return self._read_shadow(payload)
        if length == lu:
            return payload
        if flags & F_BLOCKS:
            try:
                return lzo.decompress_blocks(payload, lu)
            except lzo.LzoError as ex:
                raise ValueError(f"{e.name}: {ex}") from None
        try:
            return lzo.decompress(payload, lu)
        except lzo.LzoError as ex:
            raise ValueError(f"{e.name}: {ex}") from None

    def read_key(self, key: int, **kw) -> bytes:
        e = self.find(key)
        if e is None:
            raise KeyError(f"{key:08X} not in {self.path}")
        return self.read(e, **kw)

    def _read_shadow(self, link: bytes) -> bytes:
        # '<$shadow$>/xx/yy/KEY.branch.ext'
        s = link.split(b"\0")[0].decode("latin-1")
        last = s.rsplit("/", 1)[-1]
        keyhex, tag, ext = last.split(".", 2)
        key = int(keyhex, 16)
        base = os.path.basename(self.path).split(".")[0]
        cands = []
        for t in (tag, "wii", "$hd$"):
            for e2 in (ext, ext.lower(), ext.upper()):
                for bf in ("BF", "bf"):
                    cands.append(os.path.join(self.shadow_dir, f"{base}.{t}.{e2}.{bf}"))
        for c in cands:
            if c in self.shadows or os.path.exists(c):
                sh = self.shadows.get(c) or Big(c)
                self.shadows[c] = sh
                if sh.find(key):
                    return sh.read_key(key)
        raise FileNotFoundError(f"shadow {s}: no sibling bigfile holds {key:08X}")

    def close(self):
        self.f.close()
        for s in self.shadows.values():
            s.close()


def is_bigfile(path: str) -> bool:
    """True when the file starts with the bigfile magic."""
    try:
        with open(path, "rb") as f:
            head = f.read(4)
    except OSError:
        return False
    return len(head) == 4 and _u32(head, 0) == MAGIC
