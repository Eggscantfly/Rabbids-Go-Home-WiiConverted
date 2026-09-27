"""Import a Rabbids Go Home Wii save into the PC save format used by the port (PC executable + platform DLL).

WII FORMAT (Wii executable)
  index.dat    6 x M records of 0xC0 bytes, M = the largest SAV_SetStructureMaxSize count (90), record t*M + slot
               (RVL_NandHelper::pGetIndexHeader 80453D10).  The Wii keeps slot headers only here.
  record       big-endian: +0 u16 0x0067, +2 u16, +4 u32 valid, +8..+C u8 sec min hour mday tm_mon (localtime),
               +D u8, +E u16 tm_year+1900, +10 u32[10] user data 100..109, +38 UTF-16BE name (64 units),
               +B8 u32 data size (SAV_gt_SaveContext+8), +BC u32 runtime user-buffer pointer
               (SAV_b_WriteSlot 80072480 fills magic/valid/time/+B8 and SetIndexHeader copies the record).
  slot file    slt_<slot>_<t>.sav = [user buffer 0x2020 if the structure has one][data]; merged structure:
               slt_xx_<t>.sav, slot k at k*S_t = [user buffer][data] (SAV_b_SeekWriteSlot 8007291C writes at
               slot * Config+0x1C[t]).  Files are created at first boot with size count*align32(S_t)
               (RVL_BootUpFlow::b_CreatingFirstBootStep 80452668): the zero tail after the data is that padding.
  data         the SAV tmp buffer: entries {u32 key, u32 id, u32 len, u8 data[len]}, big-endian, key 0 = universe,
               id = the variable's offset in the universe variable buffer.

PC FORMAT (PC executable readers FUN_006f4830/FUN_006f44b0, FUN_006f49a0/FUN_006f4ac0/FUN_006f4580, header
writer FUN_006f4690; the platform DLL's SAV_SaveSlotEx writes the same)
  slt_<slot>_<t>.sav   [record 0xC0][user buffer 0x2020 if SAV_SetUserBufferSize(t) != 0][data: record.+B8 bytes]
  slt_xx_<t>.sav       slot k region at k*(0xC0 + S_t) = [record][user buffer][data], zero padded to 0xC0 + S_t;
                       S_t = SCR::SAVComputeSlotSize(t) (004F83E0) as stored in 0x00A936B4[t]
  record               the Wii record little-endian (u16/u32 fields and the UTF-16 name swapped, bytes copied),
                       +BC = 0 (runtime pointer, restored by the readers)
  user buffer          u32 0xFFFFFFFF, u32 0xFFFFFFFF, u32 0x2014, 0x2014 bytes (JPEG, not swapped)
  data                 the same entries little-endian; values swapped per universe variable type (universe.py)
"""
from __future__ import annotations

import array
import collections
import os
import struct

from . import universe

REC = 0xC0
UB = 0x2020
UB_LEN = 0x2014
MAGIC = 0x67
NTABLES = 6

# The Wii flow's structure configuration (GST_SaveManager SAVE_STRUCTURE_InitAll, run before SAV_InitSystem):
#   SAV_SetUserBufferSize(1, 6144); SAV_SetSlotsMerge(1);
#   SAV_SetStructureMaxSize(0, 3); (1, 90); (2, 1); (4, 0); (5, 0)
USERBUF = {1: 6144}
MERGED = {1}
COUNTS = {0: 3, 1: 90, 2: 1, 3: 0, 4: 0, 5: 0}
# GST_SaveManager FBUF_InitAll buffer sizes
FBUF_SIZES = {"buf_CustoJpeg": 120 * 1024, "buf_FigurineJpeg": 120 * 1024, "fbuf_Bitmap": 20 * 1024,
              "fbuf_Alpha": 10 * 1024}


class SaveImportError(Exception):
    pass


def swap_units(b, unit):
    if unit == 1:
        return bytes(b)
    code = {2: "H", 4: "I", 8: "Q"}[unit]
    a = array.array(code)
    if a.itemsize != unit:
        raise SaveImportError("array item size for %d-byte units" % unit)
    a.frombytes(bytes(b))
    a.byteswap()
    return a.tobytes()


# ---------------------------------------------------------------------------------------------------------------
# records
# ---------------------------------------------------------------------------------------------------------------
def rec_fields(r, endian):
    e = endian
    magic, w2, valid = struct.unpack_from(e + "HHI", r, 0)
    year = struct.unpack_from(e + "H", r, 0xE)[0]
    ud = struct.unpack_from(e + "10I", r, 0x10)
    name_units = struct.unpack_from(e + "64H", r, 0x38)
    size, ubp = struct.unpack_from(e + "II", r, 0xB8)
    name = []
    for u in name_units:
        if u == 0:
            break
        name.append(chr(u))
    return {"magic": magic, "w2": w2, "valid": valid, "sec": r[8], "min": r[9], "hour": r[10], "mday": r[11],
            "mon": r[12], "b0d": r[13], "year": year, "ud": ud, "name_units": name_units, "name": "".join(name),
            "size": size, "ubp": ubp}


def pc_record(rec_be, size):
    f = rec_fields(rec_be, ">")
    r = bytearray(REC)
    struct.pack_into("<HHI", r, 0, f["magic"], f["w2"], f["valid"])
    r[8:0xE] = rec_be[8:0xE]
    struct.pack_into("<H", r, 0xE, f["year"])
    struct.pack_into("<10I", r, 0x10, *f["ud"])
    struct.pack_into("<64H", r, 0x38, *f["name_units"])
    struct.pack_into("<II", r, 0xB8, size, 0)
    return bytes(r)


def pc_userbuf(block_be):
    """(pc bytes, present) for a Wii 0x2020 user buffer block."""
    if len(block_be) >= 12:
        k, i, ln = struct.unpack_from(">III", block_be, 0)
        if k == 0xFFFFFFFF and i == 0xFFFFFFFF and ln == UB_LEN and len(block_be) >= UB:
            return struct.pack("<III", k, i, ln) + bytes(block_be[12:UB]), True
    return struct.pack("<III", 0xFFFFFFFF, 0xFFFFFFFF, UB_LEN) + bytes(UB_LEN), False


# ---------------------------------------------------------------------------------------------------------------
# entries
# ---------------------------------------------------------------------------------------------------------------
def walk_entries(buf, off, size, endian):
    """[(key, id, len, payload offset)] or raises."""
    out = []
    p, end = off, off + size
    while p < end:
        if p + 12 > end:
            raise SaveImportError("entry header truncated at +%#x" % (p - off))
        k, i, ln = struct.unpack_from(endian + "III", buf, p)
        if p + 12 + ln > end:
            raise SaveImportError("entry id %#x len %d overruns the data at +%#x" % (i, ln, p - off))
        out.append((k, i, ln, p + 12))
        p += 12 + ln
    return out


def convert_data(buf, off, size, leaves, lenient, notes):
    """Wii entries -> PC entries (same order, same sizes)."""
    out = bytearray()
    for k, i, ln, p in walk_entries(buf, off, size, ">"):
        payload = buf[p:p + ln]
        if k != 0:
            raise SaveImportError("entry id %#x has key %#x (not a universe entry)" % (i, k))
        leaf = leaves.get(i)
        if leaf is None or leaf.kind == "untyped":
            what = "no saved universe variable at id %#x" % i if leaf is None else \
                "variable %s has type %#x without a swap rule" % (leaf.name, leaf.type)
            if not lenient:
                raise SaveImportError(what)
            unit = 4 if ln % 4 == 0 else 1
            notes.append("%s: %d bytes swapped as %d-byte units (--lenient)" % (what, ln, unit))
            conv = swap_units(payload, unit)
        elif leaf.kind == "raw":
            conv = bytes(payload)
        else:
            if ln != leaf.size or ln % leaf.kind:
                raise SaveImportError("%s: entry len %d, variable size %d" % (leaf.name, ln, leaf.size))
            conv = swap_units(payload, leaf.kind)
        out += struct.pack("<III", k, i, ln)
        out += conv
    return bytes(out)


def leaf_data_size(leaves, fb_size_of):
    total = 0
    for leaf in leaves.values():
        total += 12 + (fb_size_of(leaf) if leaf.kind == "raw" else leaf.size)
    return total


# ---------------------------------------------------------------------------------------------------------------
# Wii save
# ---------------------------------------------------------------------------------------------------------------
class WiiSave:
    def __init__(self, folder):
        self.folder = folder
        p = os.path.join(folder, "index.dat")
        if not os.path.exists(p):
            raise SaveImportError("%s: no index.dat" % folder)
        with open(p, "rb") as f:
            self.index = f.read()
        if len(self.index) % (REC * NTABLES):
            raise SaveImportError("index.dat size %d is not 6 x n x 0xC0" % len(self.index))
        self.per_table = len(self.index) // (REC * NTABLES)
        self._files = {}

    def record(self, t, slot):
        o = (t * self.per_table + slot) * REC
        return self.index[o:o + REC]

    def present(self, t, slot):
        return any(self.record(t, slot))

    def file(self, name):
        if name not in self._files:
            p = os.path.join(self.folder, name)
            data = None
            if os.path.exists(p):
                with open(p, "rb") as f:
                    data = f.read()
            self._files[name] = data
        return self._files[name]


def fmt_time(f):
    return "%04d tm_mon %02d mday %02d %02d:%02d:%02d" % (f["year"], f["mon"], f["mday"], f["hour"], f["min"], f["sec"])


def plan(ws, leaves_all, slot_sizes, lenient):
    """Decide and convert everything; returns (files {name: bytes}, report lines, per-slot info, Wii slot sizes)."""
    files = {}
    report = []
    info = []
    wii_S = {}
    for t in range(NTABLES):
        slots = [s for s in range(ws.per_table) if ws.present(t, s)]
        if not slots:
            continue
        leaves = leaves_all.get(t, collections.OrderedDict())
        has_ub = t in USERBUF
        merged = t in MERGED
        if merged:
            name = "slt_xx_%d.sav" % t
            src = ws.file(name)
            if src is None:
                raise SaveImportError("%s missing for structure %d" % (name, t))
            sizes = {rec_fields(ws.record(t, s), ">")["size"] for s in slots
                     if rec_fields(ws.record(t, s), ">")["valid"]}
            if len(sizes) != 1:
                raise SaveImportError("structure %d: valid records disagree on the data size %s" % (t, sorted(sizes)))
            data_size = sizes.pop()
            S = data_size + (UB if has_ub else 0)
            count = COUNTS.get(t) or ws.per_table
            aligned = (S + 0x1F) & ~0x1F
            if len(src) != count * aligned:
                report.append("structure %d: %s is %d bytes, expected %d x align32(%#x) = %d" % (
                    t, name, len(src), count, S, count * aligned))
            wii_S[t] = S
            S_pc = slot_sizes.get(t, S)
            if S_pc < S:
                raise SaveImportError("structure %d: --slot-size %#x smaller than the Wii slot size %#x" % (t, S_pc, S))
            region = REC + S_pc
            out = bytearray(count * region)
            for s in slots:
                rec = ws.record(t, s)
                f = rec_fields(rec, ">")
                if f["magic"] != MAGIC:
                    report.append("structure %d slot %d: index record without magic, left zero" % (t, s))
                    continue
                base_w = s * S
                notes = []
                ub_pc, ub_present = pc_userbuf(src[base_w:base_w + UB]) if has_ub else (b"", True)
                try:
                    data_pc = convert_data(src, base_w + (UB if has_ub else 0), f["size"], leaves, lenient, notes)
                    size = f["size"]
                except SaveImportError as ex:
                    if f["valid"]:
                        raise SaveImportError("structure %d slot %d (valid): %s" % (t, s, ex))
                    report.append("structure %d slot %d (invalid record): data not convertible (%s); header "
                                  "written with data size 0" % (t, s, ex))
                    data_pc, size = b"", 0
                o = s * region
                out[o:o + REC] = pc_record(rec, size)
                if has_ub:
                    out[o + REC:o + REC + UB] = ub_pc
                    if not ub_present:
                        report.append("structure %d slot %d: no user buffer block in the Wii region, zero block "
                                      "with the LE header written" % (t, s))
                d0 = o + REC + (UB if has_ub else 0)
                out[d0:d0 + len(data_pc)] = data_pc
                report.extend("structure %d slot %d: %s" % (t, s, n) for n in notes)
                info.append((t, s, f, size))
            files[name] = bytes(out)
            report.append("structure %d merged: %s, %d regions of 0xC0 + S_%d = %#x (Wii S_%d = %#x)" % (
                t, name, count, t, S_pc, t, S))
        else:
            for s in slots:
                rec = ws.record(t, s)
                f = rec_fields(rec, ">")
                if f["magic"] != MAGIC:
                    report.append("structure %d slot %d: index record without magic, skipped" % (t, s))
                    continue
                name = "slt_%d_%d.sav" % (s, t)
                src = ws.file(name)
                notes = []
                ub_pc = b""
                if src is None:
                    if f["valid"]:
                        raise SaveImportError("structure %d slot %d is valid but %s is missing" % (t, s, name))
                    data_pc, size = b"", 0
                    report.append("%s: Wii file missing, invalid header written alone" % name)
                else:
                    if has_ub:
                        ub_pc, ub_present = pc_userbuf(src[:UB])
                        if not ub_present:
                            report.append("%s: no user buffer block, zero block written" % name)
                    try:
                        data_pc = convert_data(src, UB if has_ub else 0, f["size"], leaves, lenient, notes)
                        size = f["size"]
                    except SaveImportError as ex:
                        if f["valid"]:
                            raise SaveImportError("%s (valid): %s" % (name, ex))
                        report.append("%s (invalid record): data not convertible (%s); header written with data "
                                      "size 0" % (name, ex))
                        data_pc, size = b"", 0
                    tail = len(src) - (UB if has_ub else 0) - f["size"]
                    if size and tail and any(src[len(src) - tail:]):
                        report.append("%s: %d bytes after the data are not zero padding" % (name, tail))
                files[name] = pc_record(rec, size) + ub_pc + data_pc
                report.extend("%s: %s" % (name, n) for n in notes)
                info.append((t, s, f, size))
    return files, report, info, wii_S


# ---------------------------------------------------------------------------------------------------------------
# read back like the PC readers and compare with the Wii source
# ---------------------------------------------------------------------------------------------------------------
def self_check(out_dir, ws, leaves_all, slot_sizes, wii_S, info):
    problems = []
    checked_entries = 0
    for t, s, f_wii, size in info:
        has_ub = t in USERBUF
        if t in MERGED:
            name = "slt_xx_%d.sav" % t
            with open(os.path.join(out_dir, name), "rb") as fh:
                buf = fh.read()
            S_pc = slot_sizes.get(t, wii_S[t])
            count = COUNTS.get(t) or ws.per_table
            if len(buf) != count * (REC + S_pc):
                problems.append("%s: size %d != %d x (0xC0 + %#x)" % (name, len(buf), count, S_pc))
            off = s * (REC + S_pc)                           # FUN_006f4ac0 / FUN_006f49a0
            src = ws.file(name)
            src_off = s * wii_S[t]
        else:
            name = "slt_%d_%d.sav" % (s, t)
            with open(os.path.join(out_dir, name), "rb") as fh:
                buf = fh.read()
            off = 0                                           # FUN_006f4910 / FUN_006f4830
            src = ws.file(name)
            src_off = 0
        rec = buf[off:off + REC]
        f_pc = rec_fields(rec, "<")
        if struct.unpack_from("<H", rec, 0)[0] != MAGIC:     # FUN_006f44b0: cmp word ptr [eax], 0x67
            problems.append("%s slot %d: magic" % (name, s))
        for k in ("magic", "w2", "valid", "sec", "min", "hour", "mday", "mon", "b0d", "year", "ud", "name_units"):
            if f_pc[k] != f_wii[k]:
                problems.append("%s slot %d: field %s %r != %r" % (name, s, k, f_pc[k], f_wii[k]))
        if f_pc["size"] != size or f_pc["ubp"] != 0:
            problems.append("%s slot %d: size/ub pointer %#x/%#x" % (name, s, f_pc["size"], f_pc["ubp"]))
        p = off + REC
        if has_ub:
            ub = buf[p:p + UB]
            k, i, ln = struct.unpack_from("<III", ub, 0)
            if (k, i, ln) != (0xFFFFFFFF, 0xFFFFFFFF, UB_LEN):
                problems.append("%s slot %d: user buffer header" % (name, s))
            if src is not None and ub[12:] != src[src_off + 12:src_off + UB] and any(src[src_off:src_off + 12]):
                problems.append("%s slot %d: user buffer bytes differ" % (name, s))
            p += UB
        if not size:
            continue
        pc_entries = walk_entries(buf, p, size, "<")
        if sum(12 + e[2] for e in pc_entries) != size:
            problems.append("%s slot %d: entries do not fill the data size" % (name, s))
        wii_entries = walk_entries(src, src_off + (UB if has_ub else 0), size, ">")
        if len(pc_entries) != len(wii_entries):
            problems.append("%s slot %d: %d entries vs %d" % (name, s, len(pc_entries), len(wii_entries)))
            continue
        leaves = leaves_all.get(t, {})
        for (k1, i1, l1, p1), (k2, i2, l2, p2) in zip(pc_entries, wii_entries):
            if (k1, i1, l1) != (k2, i2, l2):
                problems.append("%s slot %d: entry header %#x/%#x/%d vs %#x/%#x/%d" % (name, s, k1, i1, l1, k2, i2, l2))
                break
            leaf = leaves.get(i1)
            a, b = buf[p1:p1 + l1], src[p2:p2 + l2]
            if leaf is not None and leaf.kind == "raw":
                ok = a == b
            else:
                unit = leaf.kind if leaf is not None and leaf.kind != "untyped" else (4 if l1 % 4 == 0 else 1)
                ok = swap_units(a, unit) == b
            if not ok:
                problems.append("%s slot %d: value of %s differs" % (name, s, leaf.name if leaf else hex(i1)))
            checked_entries += 1
    valid = {(t, s) for t, s, f, size in info if f["valid"]}
    pretitle = (1 if (2, 0) in valid else 0) + sum(1 for i in range(10) if (1, i) in valid)
    return problems, checked_entries, pretitle


# ---------------------------------------------------------------------------------------------------------------
# dump
# ---------------------------------------------------------------------------------------------------------------
def decode_values(buf, off, size, leaves, endian):
    out = collections.OrderedDict()
    for k, i, ln, p in walk_entries(buf, off, size, endian):
        leaf = leaves.get(i)
        name = leaf.name if leaf else "id_%X" % i
        raw = buf[p:p + ln]
        if leaf is None or leaf.kind in ("untyped",):
            out[name] = ("bytes", raw)
        elif leaf.kind == "raw":
            out[name] = ("fixedbuffer", raw)
        elif leaf.type == 0x15:
            out[name] = ("float", struct.unpack(endian + "%df" % (ln // 4), raw))
        elif leaf.type == 0x16:
            out[name] = ("vector", struct.unpack(endian + "%df" % (ln // 4), raw))
        else:
            n = ln // leaf.kind
            code = {2: "H", 4: "i", 8: "q"}[leaf.kind]
            out[name] = ("int", struct.unpack(endian + "%d%s" % (n, code), raw))
    return out


def fb_text(raw):
    if not any(raw):
        return "empty"
    if raw[:3] == b"\xff\xd8\xff":
        end = raw.rfind(b"\xff\xd9")
        return "JPEG %d bytes" % (end + 2 if end >= 0 else len(raw))
    return "data"


def dump(ws, leaves_all, wii_S, info, log=print):
    log("index.dat: %d records, %d per structure" % (len(ws.index) // REC, ws.per_table))
    for t in range(NTABLES):
        for s in range(ws.per_table):
            if not ws.present(t, s):
                continue
            f = rec_fields(ws.record(t, s), ">")
            ud = " ".join("%d=%d" % (100 + k, v) for k, v in enumerate(f["ud"]) if v)
            log("  structure %d slot %2d valid %d name %-10r time %s data %#x ub_ptr %08X  %s" % (
                t, s, f["valid"], f["name"], fmt_time(f), f["size"], f["ubp"], ud))
    for t, s, f, size in info:
        if not f["valid"] or not size:
            continue
        leaves = leaves_all.get(t, {})
        if t in MERGED:
            src = ws.file("slt_xx_%d.sav" % t)
            off = s * wii_S[t] + (UB if t in USERBUF else 0)
            ub = fb_text(src[s * wii_S[t] + 12:s * wii_S[t] + UB]) if t in USERBUF else None
        else:
            src = ws.file("slt_%d_%d.sav" % (s, t))
            off = UB if t in USERBUF else 0
            ub = None
        vals = decode_values(src, off, size, leaves, ">")
        log("\nstructure %d slot %d %r (%d entries)%s" % (t, s, f["name"], len(vals),
                                                         ("  user buffer: " + ub) if ub else ""))
        maps = collections.OrderedDict()
        rest = []
        for name, (kind, v) in vals.items():
            if name.startswith("MAP_SAVED["):
                idx = int(name[len("MAP_SAVED["):name.index("]")])
                maps.setdefault(idx, {})[name.split(".", 1)[1]] = v[0]
            elif kind == "fixedbuffer":
                rest.append("%s: %s" % (name, fb_text(v)))
            elif kind in ("int", "float", "vector"):
                if any(v):
                    rest.append("%s = %s" % (name, ", ".join(("%g" % x) if kind != "int" else str(x) for x in v)
                                             if len(v) < 16 else "%d values, non-zero at %s" % (
                                                 len(v), [k for k, x in enumerate(v) if x])))
            else:
                rest.append("%s: %d bytes" % (name, len(v)))
        if maps:
            used = {i: m for i, m in maps.items() if any(m.values())}
            log("  MAP_SAVED: %d of %d maps with data; XS total %d, time-attack XS total %d, finished %d" % (
                len(used), len(maps), sum(m.get("m_NbXS", 0) for m in maps.values()),
                sum(m.get("m_NbXS_TimeAttack", 0) for m in maps.values()),
                sum(1 for m in maps.values() if m.get("m_finished"))))
            for i, m in used.items():
                log("    map %2d: XS %d reward %#x time %.2f XS_TA %d finished %d" % (
                    i, m.get("m_NbXS", 0), m.get("m_Reward", 0), m.get("m_timeAttack", 0.0),
                    m.get("m_NbXS_TimeAttack", 0), m.get("m_finished", 0)))
        for line in rest:
            log("  " + line)


# ---------------------------------------------------------------------------------------------------------------
def import_save(wii_save_dir, out_dir, bigfile, slot_sizes=None, check=True, lenient=False, show_dump=False,
                log=print):
    """Convert the Wii save in `wii_save_dir` into PC save files in `out_dir`.  Returns a process exit code."""
    slot_sizes = dict(slot_sizes or {})
    wii = os.path.realpath(wii_save_dir)
    out = os.path.realpath(out_dir)
    try:
        inside = os.path.normcase(os.path.commonpath([out, wii])) == os.path.normcase(wii)
    except ValueError:                              # different drives
        inside = False
    if inside:
        log("refused: the output folder is inside the Wii save folder")
        return 2
    try:
        ws = WiiSave(wii)
        leaves_all = universe.load_from_bigfile(bigfile)
        log("universe model (bigfile entry %08X): saved leaves %s" % (
            universe.UNIVERSE_MODEL_KEY, {t: len(v) for t, v in sorted(leaves_all.items())}))
        files, report, info, wii_S = plan(ws, leaves_all, slot_sizes, lenient)
    except (SaveImportError, universe.ModelError, ValueError, OSError) as ex:
        log("error: %s" % ex)
        return 1

    # the slot size the PC computes (SCR::SAVComputeSlotSize over the same variables and FBUF_InitAll sizes)
    for t, S in sorted(wii_S.items()):
        leaves = leaves_all.get(t, {})

        def fb_size(leaf):
            base = leaf.name.split("[")[0].split(".")[-1]
            return FBUF_SIZES.get(base, 0)
        computed = leaf_data_size(leaves, fb_size) + (UB if t in USERBUF else 0)
        log("S_%d: Wii %#x, computed from the universe variables + FBUF_InitAll sizes %#x%s, PC files use %#x" % (
            t, S, computed, "" if computed == S else " (DIFFERENT)", slot_sizes.get(t, S)))

    if show_dump:
        dump(ws, leaves_all, wii_S, info, log)

    os.makedirs(out, exist_ok=True)
    for name, data in sorted(files.items()):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        log("wrote %s (%d bytes)" % (name, len(data)))
    for line in report:
        log("note: " + line)
    if not check:
        return 0
    problems, n, pretitle = self_check(out, ws, leaves_all, slot_sizes, wii_S, info)
    log("self-check: %d slots, %d entries compared, %d problem(s); PRETITLE sum (album + figurines 0..9) = %d%s" % (
        len(info), n, len(problems), pretitle, " (MAINTITLE without first-boot figurines)" if pretitle == 11 else ""))
    for p in problems[:40]:
        log("  " + p)
    return 1 if problems else 0
