"""Texture pixel data: sizes and the Wii GX layouts of the K3D pixel formats a texture bank holds.

A bank (the 0xFEF package of a world) is `u16 count` then entries of {u32 key, u32 key, u32, u32, then the
K3D_texture_tt_texdata stream: u32 version, u8 format, u8 levels, u8 flags, u8 type, then per level u16 w, u16 h,
pixels}.  Wii pixels are the GX layout (tiled, big-endian; format 9 is CMPR):

    0x09 DXT1 / 0x1B ACPR  GX CMPR: 8x8 tiles of 2x2 DXT1 blocks, big-endian colours, MSB-first indices
    0x11 A8, 0x12 L8       8x4 tiles of bytes
    0x13 A8L8              4x4 tiles of big-endian AL pairs
    0x00 A8R8G8B8          4x4 tiles of AR then GB planes
"""
from __future__ import annotations

import numpy as np

BITS = [32, 16, 32, 64, 32, 64, 128, 16, 32, 4, 8, 8, 16, 32, 16, 32,
        32, 8, 8, 16, 16, 0, 64, 16, 0, 0, 0, 4]
COMPRESSED = {0x09, 0x0A, 0x0B, 0x1B}


def mem_needed(w, h, fmt, tiled):
    """K3D_texture::u32_GetMemNeeded(w, h, fmt, tiled)."""
    bits = BITS[fmt] if fmt < len(BITS) else 0
    if fmt in COMPRESSED:
        r7 = 8 if tiled else 4
        bw = (2 if tiled else 1) if w < r7 else w >> 2
        bh = (2 if tiled else 1) if h < r7 else h >> 2
        return (bw * bh * bits) << 1
    if tiled:
        if fmt == 0x15:
            bits = 8
        mw = 8 if bits == 8 else 4
        return (bits * max(w, mw) * max(h, 4)) >> 3
    return (bits * w * h) >> 3


def _pad(w, h, fmt):
    if fmt in (0x09, 0x1B):
        return max(8, (w + 7) & ~7), max(8, (h + 7) & ~7)
    if fmt in (0x11, 0x12, 0x15):
        return max(8, (w + 7) & ~7), max(4, (h + 3) & ~3)
    return max(4, (w + 3) & ~3), max(4, (h + 3) & ~3)


def cmpr_to_dxt1(src: bytes, w: int, h: int) -> bytes:
    """GX CMPR -> DXT1, lossless: reorder the 4x4 blocks from 8x8 tiles to row-major, byte-swap the two colour words,
    and reverse the 2-bit index order inside every row byte."""
    pw, ph = _pad(w, h, 9)
    nbx, nby = pw // 4, ph // 4                       # blocks in the padded image
    tx, ty = pw // 8, ph // 8
    blk = np.frombuffer(src, np.uint8, tx * ty * 4 * 8).reshape(ty, tx, 2, 2, 8)
    # tile (ty, tx), block row (2), block col (2) -> block grid (nby, nbx)
    grid = blk.transpose(0, 2, 1, 3, 4).reshape(nby, nbx, 8)
    obx, oby = max(1, (w + 3) // 4), max(1, (h + 3) // 4)   # blocks the PC layout wants
    grid = grid[:oby, :obx]
    out = np.empty((oby, obx, 8), np.uint8)
    out[..., 0] = grid[..., 1]; out[..., 1] = grid[..., 0]   # colour 0, little-endian
    out[..., 2] = grid[..., 3]; out[..., 3] = grid[..., 2]   # colour 1
    idx = grid[..., 4:8]
    # each row byte holds texels 0..3 in bits 7-6, 5-4, 3-2, 1-0 on GX; DXT wants texel 0 in bits 1-0
    rev = ((idx & 0xC0) >> 6) | ((idx & 0x30) >> 2) | ((idx & 0x0C) << 2) | ((idx & 0x03) << 6)
    out[..., 4:8] = rev
    return out.tobytes()


def untile_8bit(src: bytes, w: int, h: int) -> bytes:
    """8x4 tiles of bytes -> rows."""
    pw, ph = _pad(w, h, 0x11)
    t = np.frombuffer(src, np.uint8, pw * ph).reshape(ph // 4, pw // 8, 4, 8)
    img = t.transpose(0, 2, 1, 3).reshape(ph, pw)
    return img[:h, :w].tobytes()


def untile_rgba8(src: bytes, w: int, h: int) -> bytes:
    """4x4 tiles of 64 bytes (16 AR pairs, then 16 GB pairs) -> BGRA rows (D3D A8R8G8B8 in memory)."""
    pw, ph = _pad(w, h, 0)
    t = np.frombuffer(src, np.uint8, pw * ph * 4).reshape(ph // 4, pw // 4, 2, 16, 2)
    ar = t[:, :, 0].reshape(ph // 4, pw // 4, 4, 4, 2)
    gb = t[:, :, 1].reshape(ph // 4, pw // 4, 4, 4, 2)
    img = np.empty((ph // 4, pw // 4, 4, 4, 4), np.uint8)
    img[..., 0] = gb[..., 1]      # B
    img[..., 1] = gb[..., 0]      # G
    img[..., 2] = ar[..., 1]      # R
    img[..., 3] = ar[..., 0]      # A
    img = img.transpose(0, 2, 1, 3, 4).reshape(ph, pw, 4)
    return img[:h, :w].tobytes()
