"""Textures: Wii texture banks and texture header records -> PC data.

    pc_bank = textures.convert_bank(wii_bank)                  # an FEF0W bank entry of the Wii archive
    body = textures.convert("texture", key, wii_body, ctx)     # texture header record (same format on both platforms)

Bank (per world key W, bigfile entry FEF0W; read by K3D_texturelist::u32_LoadBinData 0041D270 from ViD::LoadTextures
004EB850 in the PC executable; the outer layout is the same on both platforms):

    u16 count
    count x { u32 key                        texdata key, matched to a K3D_texture by FindTexFromHTexData 0041D0F0
              u32 key                        LOA_Resolve 006D8500: file key
              u32 len(texdata) + 4           LOA_Resolve: bytes that follow (skipped when no texture wants the key)
              u32 len(texdata) + 1           LOA_p_LoadRef 006D7790: source length (with the byte only loose files keep)
              texdata }                      K3D_texture::LoadTexDataFromBuffer 00449A40:
    texdata: u32 version (<= 1), u8 format, u8 levels, u8 flags (bit 0: cube map, 6 faces), u8 type,
             [a byte only loose files keep], faces x levels x { u16 w, u16 h, pixels }

Wii pixels take u32_GetMemNeeded(w, h, fmt, tiled=1) bytes in the GX layout; the PC reads u32_GetMemNeeded(w, h, fmt, 0)
bytes of little-endian D3D texels (format ids unchanged, D3D formats: FUN_00492A80).  The GX layouts are
formats/texture.py; comparing converted banks with the PC release's added two rules: Wii A8L8 texels outside the image
take one padding byte, and one 8-bit texture is stored in plain rows.  FBF0xxxx entries (Magma language blobs) are
copied as they are.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

from ..formats import texture as tex

KINDS = ("texture",)
TEX_MAGIC = 0xC0DEC0DE
MAX_RECORD_VERSION = 13      # K3D_texture::pu32_LoadFromBuffer returns 0 above 13

# K3D pixel format id -> (name, D3D format the PC display creates (FUN_00492A80), Wii bank layout)
FORMATS = {
    0x00: ("A8R8G8B8", "D3DFMT_A8R8G8B8", "4x4 tiles of 64 bytes: 16 (A,R) byte pairs, then 16 (G,B) pairs"),
    0x09: ("DXT1", "D3DFMT_DXT1", "GX CMPR: 8x8 tiles of 2x2 DXT1 blocks, big-endian colours, indices MSB first"),
    0x11: ("A8", "D3DFMT_A8", "8x4 tiles of bytes"),
    0x12: ("L8", "D3DFMT_L8", "8x4 tiles of bytes"),
    0x13: ("A8L8", "D3DFMT_A8L8", "4x4 tiles of big-endian u16 (A<<8|L); a texel outside the image takes 1 byte"),
    0x1B: ("ACPR", "D3DFMT_DXT1", "as 0x09 (the id stays 0x1B on the PC)"),
}
COMPRESSED = (0x09, 0x1B)
EIGHT_BIT = (0x11, 0x12)


def mem_needed(w: int, h: int, fmt: int, tiled: bool) -> int:
    """K3D_texture::u32_GetMemNeeded 00433C10 (bits per texel: FUN_00433710)."""
    return tex.mem_needed(w, h, fmt, tiled)


# =============================================================================================================
# bank
# =============================================================================================================
@dataclass
class Entry:
    """One bank entry: the K3D_texture_tt_texdata stream stored under a texdata key."""
    key: int
    version: int
    fmt: int
    nlev: int
    flags: int
    typ: int
    levels: list                 # [(w, h, pixel bytes)], faces x levels in stored order
    sizes: tuple = (0, 0)        # the two size words as stored

    @property
    def dims(self) -> list:
        return [(w, h) for w, h, _ in self.levels]


def parse_bank(data: bytes, tiled: bool) -> list[Entry]:
    """Every entry of a bank in stored order, duplicates kept.  `tiled`: Wii layout (the Wii engine tiles version >= 1
    texdata); False: PC layout."""
    if len(data) < 2:
        raise ValueError("bank shorter than its count")
    n = struct.unpack_from("<H", data, 0)[0]
    o = 2
    out = []
    for i in range(n):
        if o + 24 > len(data):
            raise ValueError(f"entry {i} at {o:#x}: truncated")
        k1, k2, size, src_len, ver = struct.unpack_from("<IIIII", data, o)
        fmt, nlev, flags, typ = data[o + 20], data[o + 21], data[o + 22], data[o + 23]
        end = o + 12 + size
        if k1 != k2:
            raise ValueError(f"entry {i} at {o:#x}: keys {k1:08X} / {k2:08X}")
        if ver > 1:
            raise ValueError(f"entry {k1:08X}: texdata version {ver} (LoadTexDataFromBuffer reads <= 1)")
        if src_len != size - 3 or end > len(data):
            raise ValueError(f"entry {k1:08X}: size words {size:#x} / {src_len:#x}")
        p = o + 24
        levels = []
        for _ in range(nlev * (6 if flags & 1 else 1)):
            w, h = struct.unpack_from("<HH", data, p)
            p += 4
            sz = mem_needed(w, h, fmt, tiled and ver >= 1)
            levels.append((w, h, bytes(data[p:p + sz])))
            p += sz
        if p != end:
            raise ValueError(f"entry {k1:08X}: levels end at {p:#x}, the size word says {end:#x}")
        out.append(Entry(k1, ver, fmt, nlev, flags, typ, levels, (size, src_len)))
        o = end
    if o != len(data):
        raise ValueError(f"{len(data) - o} bytes after the last entry")
    return out


def texdata_bytes(e: Entry) -> bytes:
    out = bytearray(struct.pack("<IBBBB", e.version, e.fmt, e.nlev, e.flags, e.typ))
    for w, h, px in e.levels:
        out += struct.pack("<HH", w, h)
        out += px
    return bytes(out)


def build_bank(entries) -> bytes:
    """PC bank bytes from entries whose pixels are in the PC layout."""
    if len(entries) > 0xFFFF:
        raise ValueError("a bank holds at most 65535 entries")
    out = bytearray(struct.pack("<H", len(entries)))
    for e in entries:
        td = texdata_bytes(e)
        out += struct.pack("<IIII", e.key, e.key, len(td) + 4, len(td) + 1)
        out += td
    return bytes(out)


# =============================================================================================================
# pixels: Wii GX layout -> PC layout, one level
# =============================================================================================================
def untile_a8l8(src: bytes, w: int, h: int) -> bytes:
    """Wii A8L8 (0x13) -> D3DFMT_A8L8 rows (little-endian u16 A<<8|L).  4x4 tiles of big-endian texels; the Wii writer
    gives each tile texel outside the image ONE padding byte, so levels narrower or lower than 4 have short rows and
    short tiles (a 2x2 level is ff38 ff2e aa ff03 ff03 aa ...)."""
    if w % 4 == 0 and h % 4 == 0:
        t = np.frombuffer(src, ">u2", w * h).reshape(h // 4, w // 4, 4, 4)
        return t.transpose(0, 2, 1, 3).reshape(h, w).astype("<u2").tobytes()
    out = np.zeros((h, w), "<u2")
    p = 0
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            for y in range(4):
                for x in range(4):
                    if tx + x < w and ty + y < h:
                        out[ty + y, tx + x] = (src[p] << 8) | src[p + 1]
                        p += 2
                    else:
                        p += 1
    return out.tobytes()


def raster_8bit(src: bytes, w: int, h: int) -> bytes:
    """An 8-bit level stored in plain rows (then padding up to the tiled size)."""
    return bytes(src[:w * h])


def level_to_pc(fmt: int, src: bytes, w: int, h: int, layout: str = "tiled") -> bytes:
    """One Wii level -> the bytes LoadTexDataFromBuffer reads on the PC."""
    if fmt in COMPRESSED:
        px = tex.cmpr_to_dxt1(src, w, h)             # lossless block transcode
    elif fmt in EIGHT_BIT:
        px = raster_8bit(src, w, h) if layout == "raster" else tex.untile_8bit(src, w, h)
    elif fmt == 0x13:
        px = untile_a8l8(src, w, h)
    elif fmt == 0x00:
        px = tex.untile_rgba8(src, w, h)              # planar A,R / G,B tiles -> BGRA
    else:
        raise ValueError(f"texture format {fmt:#x}: no Wii bank holds it, no conversion rule")
    need = mem_needed(w, h, fmt, False)
    if len(px) != need:
        raise ValueError(f"{w}x{h} format {fmt:#x}: {len(px)} bytes, the PC reads {need}")
    return px


def _roughness(img: np.ndarray) -> float:
    a = img.astype(np.int16)
    r = 0.0
    if a.shape[1] > 1:
        r += float(np.abs(np.diff(a, axis=1)).sum())
    if a.shape[0] > 1:
        r += float(np.abs(np.diff(a, axis=0)).sum())
    return r


def layout_8bit(e: Entry, ratio: float = 0.4) -> tuple[str, float]:
    """Layout of a Wii A8/L8 texture: 'tiled' (GX 8x4 tiles, every such texture of the shared banks but one) or 'raster'
    (plain rows then padding: texdata DB008B61, no header field marks it).  Both readings of every level where they
    differ are scored by total variation; 'raster' wins only below `ratio` times the tiled score (shared banks:
    DB008B61 scores 0.27, the lowest tiled texture 0.61).  Returns (layout, raster / tiled score)."""
    t_sum = r_sum = 0.0
    for w, h, src in e.levels:
        t = np.frombuffer(tex.untile_8bit(src, w, h), np.uint8).reshape(h, w)
        r = np.frombuffer(src, np.uint8, w * h).reshape(h, w)
        if np.array_equal(t, r):
            continue
        t_sum += _roughness(t)
        r_sum += _roughness(r)
    if t_sum == 0.0:
        return "tiled", 1.0
    q = r_sum / t_sum
    return ("raster" if q < ratio else "tiled"), q


def convert_entry(e: Entry, sniff: bool = True) -> tuple[Entry, str]:
    """Wii entry -> PC entry (same key, version, format id, flags, type, level dims); returns (entry, layout)."""
    if e.flags & 1:
        raise ValueError(f"texdata {e.key:08X}: cube map (no Wii bank holds one; face layout unchecked)")
    if e.fmt not in FORMATS:
        raise ValueError(f"texdata {e.key:08X}: format {e.fmt:#x} has no conversion rule")
    layout = "tiled"
    if sniff and e.fmt in EIGHT_BIT:
        layout, _q = layout_8bit(e)
    levels = [(w, h, level_to_pc(e.fmt, src, w, h, layout)) for w, h, src in e.levels]
    return Entry(e.key, e.version, e.fmt, e.nlev, e.flags, e.typ, levels), layout


def convert_bank_entries(wii_bank: bytes, sniff: bool = True) -> list[tuple[Entry, Entry, str]]:
    """[(Wii entry, PC entry, layout)] in the Wii bank's order."""
    return [(e,) + convert_entry(e, sniff) for e in parse_bank(wii_bank, tiled=True)]


def convert_bank(wii_bank: bytes, key: int | None = None, ctx=None, sniff: bool = True) -> bytes:
    """Wii FEF0W texture bank -> PC FEF0W texture bank (same key, same entries in the same order)."""
    return build_bank([pc for _w, pc, _l in convert_bank_entries(wii_bank, sniff)])


# =============================================================================================================
# texture header records (package records)
# =============================================================================================================
def record_header(body: bytes) -> tuple[int, int, int]:
    """(leading memory size, version, flags) of a texture header record; version 0 without the C0DEC0DE magic."""
    if len(body) < 8:
        raise ValueError("texture record shorter than 8 bytes")
    memsize, first = struct.unpack_from("<II", body, 0)
    if first != TEX_MAGIC:
        return memsize, 0, first
    if len(body) < 16:
        raise ValueError("texture record shorter than its header")
    ver, flags = struct.unpack_from("<II", body, 8)
    return memsize, ver, flags


def convert(kind: str, key: int, wii_body: bytes, ctx=None) -> bytes:
    """Texture header record (K3D_texture::pu32_LoadFromBuffer 0044A630 + LoadDesc 0044A380).  The binary format is the
    same on both platforms and the PC executable reads every version the Wii data holds (8, 9, 10, 13; the PC archive
    itself ships records of versions 8-10), so the body is kept byte for byte once its header checks."""
    if kind not in KINDS:
        raise ValueError(f"the texture converter converts {KINDS}, not {kind!r}")
    _memsize, ver, _flags = record_header(wii_body)
    if ver > MAX_RECORD_VERSION:
        raise ValueError(f"texture {key:08X}: version {ver} (the PC executable reads <= 13)")
    return bytes(wii_body)
