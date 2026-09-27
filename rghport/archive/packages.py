"""Packages: the bins that hold the records of one world, and the world-list view of an archive.

A package (bigfile entry 0xFFF00000 | world key low 20 bits) is a flat sequence of records walked from byte 0:

    u32 key
    u32 len          0 for a record an earlier package of the same load already holds (an empty placeholder)
    u8  body[len]    u32 in-memory size, then the record's stream

For a modifier the stream opens with the MDF header MDF::pu32_BeginLoad reads: u32 flags; u16 version; u16 type
(0xFFFF: an extended header follows).
"""
from __future__ import annotations

import collections
import struct

from .bigfile import Big

MAXLEN = 1 << 23


def walk(d: bytes) -> tuple[list[tuple[int, int, int]], int]:
    """Every record in stored order as (offset, key, length), and the offset where the walk stopped."""
    n = len(d)
    o = 0
    out = []
    while o + 8 <= n:
        key, ln = struct.unpack_from("<2I", d, o)
        if ln > MAXLEN or o + 8 + ln > n or key == 0xFFFFFFFF:
            break
        out.append((o, key, ln))
        o += 8 + ln
    return out, o


def package_key(key: int) -> int:
    """The package of a world (or any key sharing its low 20 bits)."""
    return 0xFFF00000 | (key & 0xFFFFF)


class Package:
    """The records of one package."""

    def __init__(self, data: bytes, key: int):
        self.key = key
        self.d = data
        recs, end = walk(data)
        self.records = recs
        self.by_key: dict[int, list[tuple[int, int]]] = {}
        for o, k, ln in recs:
            self.by_key.setdefault(k, []).append((o + 8, ln))
        self.end = end

    def body(self, key: int, which: int = 0) -> bytes | None:
        """The bytes of copy `which` of a record (b"" for an empty placeholder), None when the package lacks it."""
        lst = self.by_key.get(key)
        if not lst:
            return None
        o, ln = lst[which]
        return self.d[o:o + ln]


class Archive:
    """The world lists of a bigfile: `worlds` maps every world key a .wol names to that list's file name (the last list
    naming it); `package(key)` loads the package of a world or record key and keeps it, in load order."""

    def __init__(self, big: Big):
        self.big = big
        self.worlds: dict[int, str] = {}
        for e in big.entries():
            if e.ext == "wol":
                for r in big.refs(e):
                    self.worlds[r.key] = e.name
        self._pk: dict[int, Package] = {}

    def package(self, key: int) -> Package | None:
        key = package_key(key)
        if key in self._pk:
            return self._pk[key]
        e = self.big.find(key)
        if e is None:
            return None
        p = Package(self.big.read(e), key)
        self._pk[key] = p
        return p

    def loaded(self):
        """The packages loaded so far, in load order."""
        return self._pk.values()


class Packages:
    """Walked packages of one archive for the converters: package key -> {record key: first body}, LRU-limited."""

    def __init__(self, big: Big, limit: int = 24):
        self.big = big
        self.limit = limit
        self.cache: collections.OrderedDict[int, dict] = collections.OrderedDict()

    def records(self, pk: int) -> dict | None:
        if pk in self.cache:
            self.cache.move_to_end(pk)
            return self.cache[pk]
        e = self.big.find(pk)
        if e is None:
            return None
        d = self.big.read(e)
        rl, _ = walk(d)
        m: dict[int, bytes] = {}
        for o, k, ln in rl:
            m.setdefault(k, d[o + 8:o + 8 + ln])
        self.cache[pk] = m
        if len(self.cache) > self.limit:
            self.cache.popitem(last=False)
        return m

    def ordered(self, pk: int) -> list[tuple[int, bytes]]:
        """Every record of a package in stored order, duplicates included (not cached)."""
        e = self.big.find(pk)
        if e is None:
            return []
        d = self.big.read(e)
        rl, _ = walk(d)
        return [(k, d[o + 8:o + 8 + ln]) for o, k, ln in rl]
