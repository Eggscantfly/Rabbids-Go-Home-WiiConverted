"""Shared access to both archives for the Wii -> PC converters.

    ctx = Context(wii_big, pc_big, wii_rec, pc_rec, cache_dir, kinds=kinds, options=vars(args))
    b = ctx.wii_body(0x5000A0E7)                 # a Wii package record body (the package the index names)
    p = ctx.pc_body(0x5000A0E7)                  # the PC record of the same key
    raw = ctx.wii_entry(0xFEF00EC6)              # any bigfile entry by key (banks, language bins ...)

Packages are walked (u32 key, u32 len, body) and kept in a small LRU per archive.  `kinds` (record key -> kind) and
`options` (the parsed command line) are for the scripts phase's hooks (hooks.py).
"""
from __future__ import annotations

import os

from ..archive.bigfile import Big
from ..archive.packages import Packages


class Context:
    def __init__(self, wii_big: Big, pc_big: Big, wii_rec: dict[int, int], pc_rec: dict[int, int],
                 cache_dir: str | None = None, log=None, kinds: dict[int, str] | None = None,
                 options: dict | None = None):
        self.wii_big = wii_big
        self.pc_big = pc_big
        self.wii_rec = wii_rec
        self.pc_rec = pc_rec
        self.cache_dir = cache_dir
        self.log = log
        self.kinds = kinds
        self.options = dict(options or {})
        self.wii = Packages(wii_big)
        self.pc = Packages(pc_big)

    def cache_path(self, name: str) -> str:
        if not self.cache_dir:
            raise ValueError("no cache folder")
        return os.path.join(self.cache_dir, name)

    # ---------------------------------------------------------------- bodies
    def wii_body(self, key: int, package: int | None = None) -> bytes | None:
        pk = package if package is not None else self.wii_rec.get(key)
        if pk is None:
            return None
        m = self.wii.records(pk)
        return m.get(key) if m else None

    def pc_body(self, key: int, package: int | None = None) -> bytes | None:
        pk = package if package is not None else self.pc_rec.get(key)
        if pk is None:
            return None
        m = self.pc.records(pk)
        return m.get(key) if m else None

    def wii_entry(self, key: int) -> bytes | None:
        e = self.wii_big.find(key)
        return self.wii_big.read(e) if e is not None else None

    def pc_entry(self, key: int) -> bytes | None:
        e = self.pc_big.find(key)
        return self.pc_big.read(e) if e is not None else None
