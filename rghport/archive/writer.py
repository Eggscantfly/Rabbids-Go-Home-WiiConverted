"""Streaming writer for a version-4 bigfile the RGH PC executable reads: gigabytes of files go straight to disk, only
the file table stays in memory.

    w = BigStreamWriter(path, capacity=5000, tick=pc_big.tick, params=pc_big.params)
    w.add(key, name, data, refs=b"", flags=None, length_user=None, perm=0x0A00)
    w.close()

Layout (what BIG::b_Open / ReadFatFiles read, see patch.py): the 0x14D8-byte header; one file-table block
{u32 count = capacity; i64 next; capacity x 200-byte records} at 0x14D8; one directory block (a single "Root"
directory) right after; then the files in the order they are added.  Unused table slots stay zero; numFiles (header
+0x2C) counts the used ones, so the executable reads only those.  Every file is stored raw (the binary loader streams
bins, see patch.py), except entries given explicit `flags` (shadow shortcuts, flags 3).  Header tick and params come
from the caller (the PC release's own bigfile, so the executable sees the values it shipped with).  Every file
record's FILETIME is `filetime` (0 unless given): the executable never reads it, and a fixed value keeps builds
reproducible.
"""
from __future__ import annotations

import struct

from .bigfile import MAGIC

HDR_SIZE = 0x14D8
REC = 200
DREC = 100
INVALID = 0xFFFFFFFF
DEFAULT_TICK = 0x66B8
DEFAULT_PARAMS = (0x0800B3F8, 0, 0, 0x36)


def _p64(v):
    return struct.pack("<II", (v >> 32) & 0xFFFFFFFF, v & 0xFFFFFFFF)


def byte_sum(data: bytes) -> int:
    """The checksum a file record keeps at +0x7C: the plain sum of the payload bytes, modulo 2^32."""
    return sum(data) & 0xFFFFFFFF


class BigStreamWriter:
    def __init__(self, path: str, capacity: int, tick: int = DEFAULT_TICK, params=DEFAULT_PARAMS, filetime: int = 0):
        self.path = path
        self.capacity = capacity
        self.f = open(path, "wb")
        self.tick, self.params = tick, list(params)
        self.filetime = filetime
        self.pos_files = HDR_SIZE
        self.pos_dirs = self.pos_files + 12 + capacity * REC
        self.data_pos = self.pos_dirs + 12 + DREC
        self.f.write(bytes(self.data_pos))              # header + tables, written for real by close()
        self.records: list[bytes] = []
        self.keys: set[int] = set()

    def add(self, key: int, name: str, data: bytes, refs: bytes | list = b"", flags: int | None = None,
            length_user: int | None = None, perm: int = 0x0A00, filetime: int | None = None) -> bool:
        """Append a file; False (nothing written) when the key is already in the bigfile."""
        if key in self.keys:
            return False
        if len(self.records) >= self.capacity:
            raise ValueError("bigfile table full (%d entries)" % self.capacity)
        if isinstance(refs, list):
            refs = b"".join(r.pack() if hasattr(r, "pack") else struct.pack("<III", *r) for r in refs)
        at = self.f.tell()
        lu = len(data) if length_user is None else length_user
        fl = 0 if flags is None else flags
        self.f.write(struct.pack("<IIII", len(data), lu, len(refs), fl) + bytes(12) + struct.pack("<I", INVALID))
        self.f.write(data)
        self.f.write(refs)
        blob_len = 32 + len(data) + len(refs)
        idx = len(self.records)
        r = bytearray(REC)
        nm = name.encode("latin-1")[:63]
        r[0:len(nm)] = nm
        nxt = idx + 1                                    # directory file chain: record order
        struct.pack_into("<III", r, 0x50, 0, nxt, blob_len)
        struct.pack_into("<Q", r, 0x5C, filetime if filetime is not None else self.filetime)
        struct.pack_into("<I", r, 0x64, key)
        r[0x68:0x70] = _p64(at)
        struct.pack_into("<HHI", r, 0x70, perm, 0, lu)
        struct.pack_into("<HHI", r, 0x78, 0, 1, 0 if flags is not None else byte_sum(data))
        self.records.append(bytes(r))
        self.keys.add(key)
        return True

    def close(self) -> int:
        """Write the header and the tables; returns the number of files."""
        n = len(self.records)
        if n:                                            # terminate the directory's file chain
            last = bytearray(self.records[-1])
            struct.pack_into("<I", last, 0x54, INVALID)
            self.records[-1] = bytes(last)
        end = self.f.tell()
        self.f.seek(self.pos_files)
        self.f.write(struct.pack("<I", self.capacity) + _p64(0))
        self.f.write(b"".join(self.records))
        self.f.seek(self.pos_dirs)
        d = bytearray(DREC)
        d[0:4] = b"Root"
        struct.pack_into("<IIIIHH", d, 0x40, INVALID, INVALID, INVALID, 0 if n else INVALID, 0x103, 0)
        self.f.write(struct.pack("<I", 1) + _p64(0) + bytes(d))
        h = bytearray(HDR_SIZE)
        struct.pack_into("<IIIII", h, 0, MAGIC, 4, self.tick, 1, 1)
        h[0x14:0x1C] = _p64(self.pos_files)
        h[0x1C:0x24] = _p64(self.pos_dirs)
        struct.pack_into("<IIII", h, 0x24, self.capacity, 1, n, 1)
        struct.pack_into("<II", h, 0x34, INVALID, INVALID)
        h[0x3C:0x44] = _p64(INVALID << 32)
        struct.pack_into("<IIII", h, 0x44, *self.params)
        self.f.seek(0)
        self.f.write(h)
        self.f.seek(end)
        self.f.close()
        return n
