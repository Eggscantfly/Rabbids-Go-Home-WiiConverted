"""The icon of the port folder's executable: set_icon(exe, ico) replaces the executable's main icon with the images of
an .ico file, in the copy the port folder holds (the PC release's file is never touched).

The executable's resources (its .rsrc section: the icon, its icon group, the manifest) are parsed into a tree, the
first icon group - the one Explorer shows, and the one the window's class names - gets the .ico file's images under new
icon ids, and the section is written back at its place, larger when needed: .rsrc is the last section of the supported
executable, so nothing else moves.  The section header, SizeOfImage, the resource directory size and the checksum are
updated; the Authenticode signature that followed the section (invalid once the file changes) is dropped with its
directory entry.  Pure Python, deterministic: the same executable and icon give the same file.

The code sections are untouched: port.code_sha256 (the SHA-256 of everything but .rsrc) still identifies the executable,
and the platform DLL's byte signatures still match.

Resource directory format (winnt.h): IMAGE_RESOURCE_DIRECTORY {u32 characteristics, u32 time, u16 major, u16 minor,
u16 named entries, u16 id entries} then entries {u32 name (bit 31: offset of a u16-counted UTF-16 string), u32 offset
(bit 31: a subdirectory)}; leaves are IMAGE_RESOURCE_DATA_ENTRY {u32 RVA, u32 size, u32 code page, u32 0}.  Three
levels: type, name, language.  RT_ICON (3) holds one image each (a DIB without file header, or a PNG), RT_GROUP_ICON
(14) a GRPICONDIR {u16 0, u16 1, u16 count} + count x {u8 width, u8 height, u8 colors, u8 0, u16 planes, u16 bits,
u32 bytes, u16 icon id}; an .ico file is the same directory with u32 file offsets instead of the icon ids.
"""
from __future__ import annotations

import os
import struct

RT_ICON, RT_GROUP_ICON = 3, 14
_DIR = "<IIHHHH"
_ENTRY = "<II"
_DATA = "<IIII"


class IconError(ValueError):
    pass


# ------------------------------------------------------------------------------------------------------- the .ico file
def parse_ico(data: bytes) -> list[tuple]:
    """[(width, height, colors, planes, bits, image bytes)] of an .ico file."""
    if len(data) < 6:
        raise IconError("not an .ico file (too short)")
    reserved, kind, n = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1 or n == 0:
        raise IconError("not an .ico file")
    out = []
    for i in range(n):
        w, h, colors, _r, planes, bits, size, off = struct.unpack_from("<BBBBHHII", data, 6 + i * 16)
        if off + size > len(data):
            raise IconError("icon image %d lies past the end of the file" % i)
        out.append((w, h, colors, planes, bits, bytes(data[off:off + size])))
    return out


def group_data(images: list[tuple], first_id: int) -> bytes:
    """A GRPICONDIR naming the images as icon resources first_id, first_id + 1, ..."""
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    for i, (w, h, colors, planes, bits, blob) in enumerate(images):
        out += struct.pack("<BBBBHHIH", w, h, colors, 0, planes, bits, len(blob), first_id + i)
    return bytes(out)


def group_members(data: bytes) -> list[int]:
    """The icon ids a GRPICONDIR names."""
    if len(data) < 6:
        return []
    n, = struct.unpack_from("<H", data, 4)
    return [struct.unpack_from("<H", data, 6 + i * 14 + 12)[0] for i in range(n) if 6 + i * 14 + 14 <= len(data)]


# ------------------------------------------------------------------------------------------------- the PE executable
class Executable:
    """The headers of a PE32 or PE32+ file and its last section, which must be .rsrc."""

    def __init__(self, data: bytes):
        self.data = bytearray(data)
        if data[:2] != b"MZ":
            raise IconError("not an executable")
        self.pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe:self.pe + 4] != b"PE\0\0":
            raise IconError("not a PE executable")
        self.nsec, = struct.unpack_from("<H", data, self.pe + 6)
        opt_size, = struct.unpack_from("<H", data, self.pe + 20)
        self.opt = self.pe + 24
        magic, = struct.unpack_from("<H", data, self.opt)
        if magic not in (0x10B, 0x20B):
            raise IconError("not a PE32 or PE32+ executable")
        self.section_align, self.file_align = struct.unpack_from("<II", data, self.opt + 32)
        self.sections = self.opt + opt_size
        self.datadir = self.opt + (96 if magic == 0x10B else 112)     # the data directories: PE32 / PE32+
        last = self.sections + (self.nsec - 1) * 40
        name = bytes(data[last:last + 8]).rstrip(b"\0")
        if name != b".rsrc":
            raise IconError("the last section is %r, not .rsrc: this executable's layout is not supported" % name)
        self.rsrc_hdr = last
        _n, self.rsrc_vsize, self.rsrc_va, self.rsrc_raw_size, self.rsrc_raw = struct.unpack_from("<8sIIII", data, last)

    def rsrc(self) -> bytes:
        return bytes(self.data[self.rsrc_raw:self.rsrc_raw + self.rsrc_raw_size])

    def replace_rsrc(self, section: bytes) -> bytes:
        """The file with `section` as its .rsrc section (padded to the file alignment), the headers updated and
        anything after the section (a signature) dropped."""
        fa = self.file_align
        padded = section + b"\0" * (-len(section) % fa)
        out = bytearray(self.data[:self.rsrc_raw]) + padded
        struct.pack_into("<II", out, self.rsrc_hdr + 8, len(section), self.rsrc_va)
        struct.pack_into("<I", out, self.rsrc_hdr + 16, len(padded))
        image = self.rsrc_va + len(section)
        image += -image % self.section_align
        struct.pack_into("<I", out, self.opt + 56, image)                   # SizeOfImage
        struct.pack_into("<II", out, self.datadir + 2 * 8, self.rsrc_va, len(section))   # resource directory
        struct.pack_into("<II", out, self.datadir + 4 * 8, 0, 0)            # certificate table: dropped
        struct.pack_into("<I", out, self.opt + 64, checksum(out))
        return bytes(out)


def checksum(data: bytes) -> int:
    """The PE checksum (as imagehlp's CheckSumMappedFile computes it), with the checksum field taken as 0."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    field = pe + 24 + 64
    total = 0
    n = len(data)
    padded = data + (b"\0" if n % 2 else b"")
    for i in range(0, len(padded), 2):
        if i == field or i == field + 2:
            continue
        total += struct.unpack_from("<H", padded, i)[0]
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total & 0xFFFF) + n


# ------------------------------------------------------------------------------------------------ the resource tree
def parse_tree(section: bytes, section_va: int) -> dict:
    """{type: {name: {language: (data, code page)}}}; keys are ints (ids) or strs (names)."""

    def read_dir(off: int, depth: int) -> dict:
        _c, _t, _ma, _mi, n_named, n_id = struct.unpack_from(_DIR, section, off)
        out = {}
        p = off + 16
        for _ in range(n_named + n_id):
            name, data = struct.unpack_from(_ENTRY, section, p)
            p += 8
            if name & 0x80000000:
                so = name & 0x7FFFFFFF
                ln, = struct.unpack_from("<H", section, so)
                key = section[so + 2:so + 2 + ln * 2].decode("utf-16-le")
            else:
                key = name
            if data & 0x80000000:
                if depth == 2:
                    raise IconError("a resource directory deeper than three levels")
                out[key] = read_dir(data & 0x7FFFFFFF, depth + 1)
            else:
                rva, size, cp, _r = struct.unpack_from(_DATA, section, data)
                start = rva - section_va
                if not 0 <= start <= start + size <= len(section):
                    raise IconError("a resource lies outside the .rsrc section")
                out[key] = (bytes(section[start:start + size]), cp)
        return out

    return read_dir(0, 0)


def _sorted_keys(d: dict) -> list:
    names = sorted((k for k in d if isinstance(k, str)), key=lambda k: k.upper())
    ids = sorted(k for k in d if isinstance(k, int))
    return names + ids


def build_tree(tree: dict, section_va: int) -> bytes:
    """A .rsrc section: the directories (types, then names, then languages), the data entries, the names, the data."""
    dirs = [tree]                                      # breadth first: the root, the type dirs, the name dirs
    for t in _sorted_keys(tree):
        dirs.append(tree[t])
    for t in _sorted_keys(tree):
        for n in _sorted_keys(tree[t]):
            dirs.append(tree[t][n])
    offsets = {}
    pos = 0
    for d in dirs:
        offsets[id(d)] = pos
        pos += 16 + 8 * len(d)
    leaves = []                                        # (data, code page) in directory order
    for t in _sorted_keys(tree):
        for n in _sorted_keys(tree[t]):
            for lang in _sorted_keys(tree[t][n]):
                leaves.append(tree[t][n][lang])
    entry_pos = {id(leaf): pos + 16 * i for i, leaf in enumerate(leaves)}
    pos += 16 * len(leaves)
    strings = {}
    for d in dirs:
        for k in d:
            if isinstance(k, str) and k not in strings:
                strings[k] = pos
                pos += 2 + 2 * len(k)
    pos += -pos % 4
    data_pos = {}
    for leaf in leaves:
        data_pos[id(leaf)] = pos
        pos += len(leaf[0])
        pos += -pos % 4
    out = bytearray(pos)
    for d in dirs:
        keys = _sorted_keys(d)
        n_named = sum(1 for k in keys if isinstance(k, str))
        struct.pack_into(_DIR, out, offsets[id(d)], 0, 0, 0, 0, n_named, len(keys) - n_named)
        p = offsets[id(d)] + 16
        for k in keys:
            child = d[k]
            name = (0x80000000 | strings[k]) if isinstance(k, str) else k
            if isinstance(child, dict):
                data = 0x80000000 | offsets[id(child)]
            else:
                data = entry_pos[id(child)]
            struct.pack_into(_ENTRY, out, p, name, data)
            p += 8
    for leaf in leaves:
        blob, cp = leaf
        struct.pack_into(_DATA, out, entry_pos[id(leaf)], section_va + data_pos[id(leaf)], len(blob), cp, 0)
        out[data_pos[id(leaf)]:data_pos[id(leaf)] + len(blob)] = blob
    for k, at in strings.items():
        struct.pack_into("<H", out, at, len(k))
        out[at + 2:at + 2 + 2 * len(k)] = k.encode("utf-16-le")
    return bytes(out)


# ----------------------------------------------------------------------------------------------------------- set_icon
def with_icon(exe_data: bytes, ico_data: bytes) -> tuple[bytes, dict]:
    """(the executable with the .ico file's images as its main icon, {"group", "language", "images", "replaced"})."""
    exe = Executable(exe_data)
    tree = parse_tree(exe.rsrc(), exe.rsrc_va)
    images = parse_ico(ico_data)
    groups = tree.get(RT_GROUP_ICON) or {}
    icons = tree.setdefault(RT_ICON, {})
    if groups:
        gkey = _sorted_keys(groups)[0]                 # the first group: what Explorer shows
        langs = groups[gkey]
        lang = _sorted_keys(langs)[0]
        old_members = group_members(langs[lang][0])
        cp = langs[lang][1]
    else:
        gkey, lang, old_members, cp = 1, 1033, [], 0
        tree[RT_GROUP_ICON] = groups
    kept = {k: v for k, v in icons.items() if k not in old_members}
    first_id = max([k for k in kept if isinstance(k, int)] + [0]) + 1
    tree[RT_ICON] = kept
    for i, img in enumerate(images):
        tree[RT_ICON][first_id + i] = {lang: (img[5], cp)}
    groups[gkey] = {lang: (group_data(images, first_id), cp)}
    section = build_tree(tree, exe.rsrc_va)
    return exe.replace_rsrc(section), {"group": gkey, "language": lang, "images": len(images),
                                       "replaced": len(old_members)}


def set_icon(exe_path: str, ico_path: str) -> dict:
    """Replace the main icon of the executable file `exe_path` with the images of `ico_path`; returns with_icon's
    report."""
    with open(exe_path, "rb") as f:
        exe_data = f.read()
    with open(ico_path, "rb") as f:
        ico_data = f.read()
    out, rep = with_icon(exe_data, ico_data)
    tmp = exe_path + ".part"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, exe_path)
    return rep
