"""LZO1X decompression of bigfile payloads.

A compressed payload is one LZO1X-1 stream, or a block stream (u32 n, n x u32 block size, the blocks) whose blocks
inflate to BLOCK_SIZE bytes each but the last (every block-stream entry of the Wii and the PC archive follows that).
The bundled `_lzo_native` Pybind11 module does the work with miniLZO when it is built. It fills the whole output buffer
it is given, so each block is decompressed into exactly its expected size; a block stream that breaks the block-size
rule makes a later block overrun its buffer, and the entry is then decoded again with the reference decoder below.
Without the native module the reference decoder does decompression (correct, but slow for the gigabytes of a whole
archive).

The reference decoder is a transcription of the stock LZO1X decompressor (the RGH Wii executable calls
LZO1X_Decompress at 8004A3B0), written as a label machine because the original is built out of gotos.
"""
from __future__ import annotations

import struct

try:
    from . import _lzo_native
except ImportError:                     # pragma: no cover - optional speed-up
    _lzo_native = None

BLOCK_SIZE = 0x3FFFC


class LzoError(ValueError):
    pass


def decompress_reference(src) -> bytes:
    """One LZO1X stream, decoded up to its end marker."""
    src = bytes(src)
    n = len(src)
    out = bytearray()
    ip = 0
    t = 0

    def literals(count):
        nonlocal ip
        if ip + count > n:
            raise LzoError("literal run past the end of the input")
        out.extend(src[ip:ip + count])
        ip += count

    def match(pos, count):
        dist = len(out) - pos
        if pos < 0 or dist <= 0:
            raise LzoError("back-reference outside the output")
        if dist >= count:
            out.extend(out[pos:pos + count])
        else:                               # overlapping copy: the last `dist` bytes repeat
            chunk = out[pos:]
            reps, rest = divmod(count, dist)
            out.extend(chunk * reps + chunk[:rest])

    if n and src[0] > 17:
        t = src[0] - 17
        ip = 1
        if t < 4:
            label = "match_next"
        else:
            literals(t)
            label = "first_literal_run"
    else:
        label = "loop"

    while True:
        if label == "loop":
            if ip >= n:
                break
            t = src[ip]
            ip += 1
            if t >= 16:
                label = "match"
                continue
            if t == 0:
                while src[ip] == 0:
                    t += 255
                    ip += 1
                t += 15 + src[ip]
                ip += 1
            literals(t + 3)
            label = "first_literal_run"
        elif label == "first_literal_run":
            t = src[ip]
            ip += 1
            if t >= 16:
                label = "match"
                continue
            pos = len(out) - 0x801 - (t >> 2) - (src[ip] << 2)
            ip += 1
            match(pos, 3)
            label = "match_done"
        elif label == "match":
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
                    break                   # end of stream
                pos -= 0x4000
                count = t
            else:
                pos = len(out) - 1 - (t >> 2) - (src[ip] << 2)
                ip += 1
                match(pos, 2)
                label = "match_done"
                continue
            match(pos, count + 2)
            label = "match_done"
        elif label == "match_done":
            t = src[ip - 2] & 3
            label = "loop" if t == 0 else "match_next"
        else:                               # match_next
            literals(t)
            t = src[ip]
            ip += 1
            label = "match"
    return bytes(out)


def _reference(src, what: str) -> bytes:
    try:
        return decompress_reference(src)
    except IndexError:
        raise LzoError("%s: stream ends inside a token" % what) from None


def decompress(src, size: int) -> bytes:
    """One LZO1X stream that inflates to `size` bytes."""
    if _lzo_native is not None:
        try:
            return _lzo_native.decompress(bytes(src), size)
        except Exception:                   # noqa: BLE001 - bad sizes: the reference decoder decides
            pass
    out = _reference(src, "LZO stream")
    if len(out) != size:
        raise LzoError("LZO stream gave %d bytes, expected %d" % (len(out), size))
    return out


def decompress_blocks(payload, size: int) -> bytes:
    """A block stream (u32 n, n x u32 block size, the blocks) that inflates to `size` bytes."""
    n = struct.unpack_from("<I", payload, 0)[0]
    sizes = struct.unpack_from("<%dI" % n, payload, 4)
    if _lzo_native is not None:
        try:
            out = bytearray()
            o = 4 + 4 * n
            for i, bs in enumerate(sizes):
                want = size - len(out) if i + 1 == n else min(BLOCK_SIZE, size - len(out))
                out += _lzo_native.decompress(bytes(payload[o:o + bs]), want)
                o += bs
            if len(out) == size:
                return bytes(out)
        except Exception:                   # noqa: BLE001 - a block off the block-size rule: decode it exactly below
            pass
    out = bytearray()
    o = 4 + 4 * n
    for bs in sizes:
        out += _reference(payload[o:o + bs], "LZO block")
        o += bs
    if len(out) != size:
        raise LzoError("block stream gave %d bytes, expected %d" % (len(out), size))
    return bytes(out)


def compress(data: bytes) -> bytes:
    """LZO1X-1 compression."""
    if _lzo_native is not None:
        return _lzo_native.compress(data)
    raise RuntimeError("the native LZO module is not built; cannot compress")
