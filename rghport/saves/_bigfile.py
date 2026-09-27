"""Minimal read access to one entry of a Rabbids Go Home bigfile (version 4), used by the save import.

This is a fallback for `rghport.archive`: `read_entry` uses the archive module when it provides
`open_bigfile(path)` returning an object with `read_key(key) -> bytes`, and otherwise reads the entry itself.

Layout (little-endian on both platforms):
    header        0x14D8 bytes: u32 magic 0x00454241 (+0x00), u32 file FAT blocks (+0x0C),
                  i64 file FAT position (+0x14, high word first)
    file FAT      blocks { u32 count; i64 next; count x 200-byte file record }
    file record   +0x00 name[64], +0x64 u32 key, +0x68 i64 data position (high word first), +0x74 u32 user length
    entry data    32-byte header { u32 stored length; u32 user length; u32 reference bytes; u32 flags }, payload
                  flags 0x01: link to a sibling bigfile (not needed here); 0x04: LZO1X block stream
                  (u32 n, n x u32 block size, blocks); stored length == user length: stored as is;
                  otherwise one LZO1X stream.
"""
from __future__ import annotations

import struct

MAGIC = 0x00454241
FILE_RECORD = 200
ENTRY_HEADER = 32
FLAG_LINK = 0x01
FLAG_BLOCKS = 0x04


class BigfileError(ValueError):
    pass


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def _i64_hi_lo(d, o):
    return (_u32(d, o) << 32) | _u32(d, o + 4)


def lzo1x_decompress(src: bytes, expected: int | None = None) -> bytes:
    """LZO1X-1 decompression (the stock algorithm, written as the reference decoder's state machine)."""
    src = bytes(src)
    n = len(src)
    out = bytearray()
    ip = 0

    def literals(count):
        nonlocal ip
        if ip + count > n:
            raise BigfileError("LZO literal run past the end of the input")
        out.extend(src[ip:ip + count])
        ip += count

    def match(pos, count):
        if pos < 0:
            raise BigfileError("LZO back-reference before the start of the output")
        for _ in range(count):                       # byte by byte: overlapping copies encode runs
            out.append(out[pos])
            pos += 1

    t = 0
    if n and src[0] > 17:
        t = src[0] - 17
        ip = 1
        if t < 4:
            state = "match_next"
        else:
            literals(t)
            state = "first_literal_run"
    else:
        state = "loop"

    while True:
        if state == "loop":
            if ip >= n:
                break
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                while src[ip] == 0:
                    t += 255
                    ip += 1
                t += 15 + src[ip]
                ip += 1
            literals(t + 3)
            state = "first_literal_run"
        elif state == "first_literal_run":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            pos = len(out) - 0x801 - (t >> 2) - (src[ip] << 2)
            ip += 1
            match(pos, 3)
            state = "match_done"
        elif state == "match":
            if t >= 64:
                pos = len(out) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
                ip += 1
                count = (t >> 5) - 1
            elif t >= 32:
                t &= 31
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 31 + src[ip]
                    ip += 1
                pos = len(out) - 1 - ((src[ip] | (src[ip + 1] << 8)) >> 2)
                ip += 2
                count = t
            elif t >= 16:
                pos = len(out) - ((t & 8) << 11)
                t &= 7
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 7 + src[ip]
                    ip += 1
                pos -= (src[ip] | (src[ip + 1] << 8)) >> 2
                ip += 2
                if pos == len(out):
                    break                               # end of stream
                pos -= 0x4000
                count = t
            else:
                pos = len(out) - 1 - (t >> 2) - (src[ip] << 2)
                ip += 1
                match(pos, 2)
                state = "match_done"
                continue
            match(pos, count + 2)
            state = "match_done"
        elif state == "match_done":
            t = src[ip - 2] & 3
            state = "loop" if t == 0 else "match_next"
        else:                                           # match_next
            literals(t)
            t = src[ip]
            ip += 1
            state = "match"

    if expected is not None and len(out) != expected:
        raise BigfileError("LZO stream gave %d bytes, expected %d" % (len(out), expected))
    return bytes(out)


def _read_entry_local(path: str, key: int) -> bytes:
    with open(path, "rb") as f:
        head = f.read(0x40)
        if len(head) < 0x40 or _u32(head, 0) != MAGIC:
            raise BigfileError("%s is not a bigfile" % path)
        blocks = _u32(head, 0x0C)
        pos = _i64_hi_lo(head, 0x14)
        f.seek(0, 2)
        size = f.tell()
        found = None
        seen = set()
        while pos and pos < size and len(seen) < max(blocks, 1) and pos not in seen:
            seen.add(pos)
            f.seek(pos)
            count, = struct.unpack("<I", f.read(4))
            nxt = _i64_hi_lo(f.read(8), 0)
            table = f.read(count * FILE_RECORD)
            for i in range(count):
                r = table[i * FILE_RECORD:(i + 1) * FILE_RECORD]
                if len(r) < FILE_RECORD or not r[0] or _u32(r, 0x64) != key:
                    continue
                dpos = _i64_hi_lo(r, 0x68)
                if dpos:
                    found = dpos
                    break
            if found is not None or nxt == 0 or nxt >= size:
                break
            pos = nxt
        if found is None:
            raise BigfileError("%s has no entry %08X" % (path, key))
        f.seek(found)
        stored, user, _refs, flags = struct.unpack("<IIII", f.read(16))
        f.seek(found + ENTRY_HEADER)
        payload = f.read(stored)
    if flags & FLAG_LINK:
        raise BigfileError("entry %08X of %s is a link to a sibling bigfile" % (key, path))
    if stored == user:
        return payload
    if flags & FLAG_BLOCKS:
        nblocks = _u32(payload, 0)
        o = 4 + 4 * nblocks
        out = bytearray()
        for b in range(nblocks):
            bsize = _u32(payload, 4 + 4 * b)
            out += lzo1x_decompress(payload[o:o + bsize])
            o += bsize
        if len(out) != user:
            raise BigfileError("entry %08X: block stream gave %d of %d bytes" % (key, len(out), user))
        return bytes(out)
    return lzo1x_decompress(payload, user)


def read_entry(path: str, key: int) -> bytes:
    """Bytes of entry `key` in the bigfile at `path` (through rghport.archive when it is available)."""
    try:
        from rghport import archive                  # tool-core module, when present
        opener = getattr(archive, "open_bigfile", None)
    except ImportError:
        opener = None
    if opener is not None:
        big = opener(path)
        try:
            return big.read_key(key)
        finally:
            close = getattr(big, "close", None)
            if close:
                close()
    return _read_entry_local(path, key)
