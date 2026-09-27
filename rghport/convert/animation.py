"""Animation: Wii track lists (spec:trl) and skin models (spec:skn) -> the package records the RGH PC executable loads.

    body = animation.convert("spec:trl", key, ctx.wii_body(key), ctx)   # PC record body (bytes)
    body = animation.convert("spec:skn", key, ctx.wii_body(key), ctx)

spec:skn  SKL::p_Callback_LoadSkn 005DC180.  Same stream on both platforms (formats.anim.skn, level 3); the Edge
          skeleton (skin version >= 8: u32 size + blob -> SKL_tt_Model+0x1C) is EdgeAnimSkeleton 'ES01', big-endian on
          the Wii and swapped field by field on the PC (es01_to_pc).

spec:trl  EVE::p_Callback_LoadTrl 005A5A00 -> pt_LoadTrl 005A59B0 -> LoadTrl 005A51E0.  Same stream (formats.anim.trl);
          what the PC release does differently:
          * no Edge animation (u32 size 0, EVE_tt_ListTracks+0x24 stays NULL, so EVE::f32_PlayTrlAni 00529800 plays the
            EVE tracks instead of the Edge evaluator FUN_0054a4a0).  bake_edge turns the EA03 blob into one translation /
            rotation / scale track per skin bone rank 1..n-1 (Edge joint j = rank j; rank 0 stays on the list's own root
            tracks), keyed on the blob's key frames with the decoded values, the kinds the blob leaves alone from the
            ES01 base pose: what EDG::PlayAnim (Wii) and FUN_0054a4a0 (PC) write into the bones.  A joint without an
            ES01 parent (the pelvis, channel 1 of the rabbid skins) is stored by Edge relative to joint 0, the root
            motion, and by the PC lists in the model space of the list, root motion included: such a joint the blob
            animates is baked as joint 0 composed with it (compose_root);
          * T/R/S tracks are plain: no compact (param2 0x80000000) or compressed (0x20000 + 0x2000000 / 0x4000000 /
            0x8000000) storage and no inline 48-bit rotations (event flag 0x400).  plain_storage decodes them with the PC
            executable's own arithmetic (MTH_Vec3Uncompress48 006D0880, MTH_QuaternionUncompress48 006EF380, 32 006EF9A0,
            64 006EEBA0, MTH_Matrix33Uncompressb16 006F4CD0; constants from the executable's .rdata);
          * script events keep their node words (same ids in both releases); the two runtime pointers of the signal
            inside a trigger node (SCR_tt_Signal_ +0x4C modifier, +0x50 signal entry: binarizer heap garbage on both
            platforms, SCR::ResolveSignal 00518670 does not always overwrite +0x50) are zeroed;
          * the leading word (memsize, read by LOA_p_LoadRef 006D7790 and unused by the callbacks) is the size of the
            list with its mode-3 fields: kept up to date for the tracks bake_edge adds (42 bytes of mode-3 fields per
            track).

The Edge bake needs the skin whose bones the clip drives: SkinIndex (a walk of the Wii archive's animation modifiers,
skin_walk, plus a scan of every package's skin records, kept in the cache folder) gives the owner skins of a list,
then the owner skins of the other lists of its packages (votes), the skins of its packages, any skin with the blob's
joint count; ctx.anim_skins = {trl key: skin key} or convert(..., skin=KEY) override it.  On the lists both releases
share, 1220 of 1242 picks fit the PC's bone channels.
"""
from __future__ import annotations

import collections
import json
import math
import os
import struct

from ..archive.index import fingerprint
from ..archive.packages import Archive, walk as walk_package
from ..formats import anim as A
from ..formats import edge as E
from ..formats import stream as F

KINDS = ("spec:trl", "spec:skn")
SKIN_INDEX_FORMAT = 1


class AnimError(ValueError):
    pass


_F32 = struct.Struct("<f")


def f32(x: float) -> float:
    return _F32.unpack(_F32.pack(x))[0]


# ============================================================================
# skins: the ES01 Edge skeleton in PC byte order
# ============================================================================
ES01_HEADER = 0x30


def es01_to_pc(blob: bytes) -> bytes:
    """EdgeAnimSkeleton, big-endian (Wii) -> the PC skin's byte order: the header (tag and two sizes u32, joint / user
    channel / SIMD quad counts and a pad u16, seven self-relative offsets u32), the SIMD hierarchy (8 u16 per quad), the
    base pose (12 floats per joint), the parent indices (i16) and the joint / user channel name hashes (u32) are swapped;
    user channel flags (u8) and padding stay.  A blob already in PC order ('10SE') is returned as is."""
    b = bytes(blob)
    if b[:4] == b"10SE":
        return b
    if b[:4] != b"ES01":
        raise AnimError(f"not an ES01 skeleton: {b[:4]!r}")
    if len(b) < ES01_HEADER:
        raise AnimError("ES01 skeleton shorter than its header")
    nj, nu, nq, _pad = struct.unpack_from(">4H", b, 0x0C)
    raw = struct.unpack_from(">7I", b, 0x14)
    base, parents, jhash, uhash, unhash, _uflags, custom = [0x14 + 4 * i + v if v else 0 for i, v in enumerate(raw)]
    if custom:
        raise AnimError("ES01 custom data (layout unknown)")
    out = bytearray(b)

    def swap(size: int, at: int, n: int):
        if n <= 0:
            return
        end = at + size * n
        if at < 0 or end > len(b):
            raise AnimError("ES01 array past the blob")
        for p in range(at, end, size):
            out[p:p + size] = b[p:p + size][::-1]

    swap(4, 0x00, 3)
    swap(2, 0x0C, 4)
    swap(4, 0x14, 7)
    swap(2, ES01_HEADER, 8 * nq)
    if base:
        swap(4, base, 12 * nj)            # floats swapped as words: bit patterns kept
    if parents:
        swap(2, parents, nj)
    if jhash:
        swap(4, jhash, nj)
    if uhash:
        swap(4, uhash, nu)
    if unhash:
        swap(4, unhash, nu)
    return bytes(out)


def convert_skn(body: bytes, report: dict | None = None) -> bytes:
    o = F.parse(A.skn, body)
    if o.get("edge_skeleton"):
        o["edge_skeleton"] = es01_to_pc(o["edge_skeleton"])
        if report is not None:
            report["es01"] = report.get("es01", 0) + 1
    out, _ = F.emit(A.skn, o)
    return out


# ============================================================================
# the PC executable's decompressors (x87 double arithmetic, results stored as floats)
# ============================================================================
VEC48_DIV = 32767.0                    # 008A1130
Q48_SCALE = 4.3159689084859565e-05     # 008CC260
Q_BIAS = 0.7071067690849304            # 008A2268
Q32_SCALE = 0.0013810679083690047      # 008CC270
Q64_SCALE = 1.3487004935086588e-06     # 008CC250
_STATS = collections.Counter()


def _sqrt_f32(t: float) -> float:
    t = f32(t)
    if t > 0.0:
        return f32(math.sqrt(t))
    if t < 0.0:
        _STATS["negative sqrt argument (the PC executable would store NaN)"] += 1
    return 0.0


def vec48(v: bytes) -> tuple:
    """MTH_Vec3Uncompress48 006D0880: three i16 over 32767."""
    return tuple(f32(k / VEC48_DIV) for k in struct.unpack("<3h", v))


def quat48(v: bytes) -> tuple:
    """MTH_QuaternionUncompress48 006EF380 -> (x, y, z, w): two u16 (15 bits + a placement bit each) and an i16,
    v * 4.316e-5 - 0.7071, the fourth component off the unit length."""
    w0, w1, w2 = struct.unpack("<HHh", v)
    a = f32(f32((w0 & 0x7FFF) * Q48_SCALE) - Q_BIAS)
    b = f32(f32((w1 & 0x7FFF) * Q48_SCALE) - Q_BIAS)
    c = f32(f32(w2 * Q48_SCALE) - Q_BIAS)
    r = _sqrt_f32(((1.0 - a * a) - b * b) - c * c)
    if w0 & 0x8000:
        return (a, b, c, r) if w1 & 0x8000 else (a, r, b, c)
    return (a, b, r, c) if w1 & 0x8000 else (r, a, b, c)


def _place(sel: int, p: float, q: float, s: float, r: float) -> tuple:
    if sel == 0:
        return (r, p, q, s)
    if sel == 0x40000000:
        return (p, r, q, s)
    if sel == 0x80000000:
        return (p, q, r, s)
    return (p, q, s, r)


def quat32(v: bytes) -> tuple:
    """MTH_QuaternionUncompress32 006EF9A0: selector bits 30-31, three 10-bit fields."""
    u = struct.unpack("<I", v)[0]
    sc, bias = f32(Q32_SCALE), f32(Q_BIAS)
    p = f32(((u >> 20) & 0x3FF) * sc - bias)
    q = f32(((u >> 10) & 0x3FF) * sc - bias)
    s = f32((u & 0x3FF) * sc - bias)
    return _place(u & 0xC0000000, p, q, s, _sqrt_f32(((1.0 - p * p) - q * q) - s * s))


def quat64(v: bytes) -> tuple:
    """MTH_QuaternionUncompress64 006EEBA0: selector bits 30-31 of the first word, three 20-bit fields."""
    u0, u1 = struct.unpack("<II", v)
    sc, bias = f32(Q64_SCALE), f32(Q_BIAS)
    p = f32(((u0 >> 8) & 0xFFFFF) * sc - bias)
    q = f32((((u0 & 0xFF) << 12) | (u1 >> 20)) * sc - bias)
    s = f32((u1 & 0xFFFFF) * sc - bias)
    return _place(u0 & 0xC0000000, p, q, s, _sqrt_f32(((1.0 - p * p) - q * q) - s * s))


_Z4 = bytes(4)


def _diag_bytes(xb: bytes, yb: bytes, zb: bytes) -> bytes:
    """A 3x3 float matrix with the given diagonal words (MTH_Matrix33SetIdentity, then the diagonal)."""
    return xb + _Z4 + _Z4 + _Z4 + yb + _Z4 + _Z4 + _Z4 + zb


def _diag_floats(x: float, y: float, z: float) -> bytes:
    return struct.pack("<9f", x, 0.0, 0.0, 0.0, y, 0.0, 0.0, 0.0, z)


STORAGE_BITS = 0x80000000 | 0x20000 | 0x2000000 | 0x4000000 | 0x8000000


def pc_trs_value(typ: int, p2: int, flags: int, v: bytes) -> tuple[bytes, int]:
    """One stored T/R/S event value of a track (type, param2) and event flags -> (the PC's plain value bytes, the event
    flag bits the plain layout drops).  Plain values: translation 3 floats, rotation 4 floats (x, y, z, w), scale a 3x3
    float matrix -- what TranslationEvent_pu32_Load 005DF0B0, RotationEvent_pu32_Load 005DEAC0 and ScaleEvent_pu32_Load
    005DDE20 read, then GetPosTrl_tra / RotationEvent_GetOrientation / ScaleEvent_GetScale decode."""
    comp = bool(p2 & 0x20000)
    n = len(v)
    if typ == 2:
        if comp and p2 & 0x4000000:
            if n != 6:
                raise AnimError(f"compressed translation of {n} bytes")
            return struct.pack("<3f", *vec48(v)), 0
        if n != 12:
            raise AnimError(f"translation of {n} bytes")
        return v, 0
    if typ == 3:
        if comp and p2 & 0x8000000:
            q = quat64(v)
        elif (comp and p2 & 0x4000000) or flags & 0x400:
            q = quat48(v)
        elif comp and p2 & 0x2000000:
            q = quat32(v)
        else:
            if n != 16:
                raise AnimError(f"rotation of {n} bytes")
            return v, 0
        return struct.pack("<4f", *q), 0x400
    if typ == 4:
        if flags & 0x2000:                                     # diagonal, 3 floats (any storage)
            return _diag_bytes(v[0:4], v[4:8], v[8:12]), 0x2000
        if comp and p2 & 0x2000000:                            # 3 x i16: 1 + i / 32767 on the diagonal
            x, y, z = vec48(v)
            return _diag_floats(f32(x + 1.0), f32(y + 1.0), f32(z + 1.0)), 0
        if comp and p2 & 0x4000000:                            # 3 floats on the diagonal
            return _diag_bytes(v[0:4], v[4:8], v[8:12]), 0
        if comp and p2 & 0x8000000:                            # 9 x i16: identity + i / 32767
            m = [f32(k / VEC48_DIV) for k in struct.unpack("<9h", v)]
            for i in (0, 4, 8):
                m[i] = f32(m[i] + 1.0)
            return struct.pack("<9f", *m), 0
        if n != 36:
            raise AnimError(f"scale of {n} bytes")
        return v, 0
    raise AnimError(f"track type {typ} is not a T/R/S track")


def plain_storage(o: dict, report: dict | None = None) -> int:
    """Every T/R/S track of a parsed list in the PC's plain storage (see pc_trs_value); frames, event flags (but the
    dropped 0x400 / 0x2000), interpolation blocks and the other param2 bits are kept.  Returns the tracks changed."""
    rep = report if report is not None else {}
    changed = 0
    for t in o.get("tracks", []):
        typ, p2 = t.get("type"), t.get("param2", 0)
        if typ not in (2, 3, 4):
            continue
        evs = t.get("events", [])
        inline = 0x400 if typ == 3 else (0x2000 if typ == 4 else 0)
        if not p2 & STORAGE_BITS and not any(e.get("flags", 0) & inline for e in evs):
            continue
        if p2 & 0x80000000:
            if not t.get("flags", 0) & 0x4000 and len({e.get("flags", 0) for e in evs}) > 1:
                raise AnimError("compact track with differing event flags words (one word at run time)")
            if any(e.get("flags", 0) & 0x81A0 for e in evs):
                raise AnimError("compact track event with interpolation / curve flags")
        clear = 0
        for e in evs:
            v = e.get("value")
            if v is None:
                continue
            e["value"], c = pc_trs_value(typ, p2, e.get("flags", 0), v)
            clear |= c
        if clear:
            for e in evs:
                e["flags"] = e.get("flags", 0) & ~clear
        t["param2"] = p2 & ~STORAGE_BITS
        changed += 1
        key = "plain %s%s%s" % ("TRS"[typ - 2], " compact" if p2 & 0x80000000 else "",
                                " compressed %#x" % (p2 & 0x0E000000) if p2 & 0x20000 else "")
        rep[key] = rep.get(key, 0) + 1
    return changed


# ============================================================================
# script events: the runtime pointers of trigger nodes
# ============================================================================
TRIGGER_NODE = 0x00310000          # SCR::ResolveNodes 00518B40: signal at +1, stock at +0x16, next node at +0x39
TRIGGER_WORDS = 0x39
SIGNAL_POINTER_WORDS = (20, 21)    # node words of SCR_tt_Signal_ +0x4C (modifier) and +0x50 (signal entry)


def _is_trigger_node(ws: list, i: int) -> bool:
    if ws[i] != TRIGGER_NODE or i + TRIGGER_WORDS > len(ws):
        return False
    name = struct.pack("<16I", *ws[i + 2:i + 18])
    nul = name.find(b"\0")
    return nul > 0 and all(32 <= c < 127 for c in name[:nul])


def zero_signal_pointers(o: dict, report: dict | None = None) -> int:
    """Zero the modifier / signal entry pointers inside the trigger nodes of the list's script events."""
    n = 0
    for t in o.get("tracks", []):
        if t.get("type") != 1:
            continue
        for e in t.get("events", []):
            ws = e.get("words")
            if not ws:
                continue
            i = 0
            while i < len(ws):
                if _is_trigger_node(ws, i):
                    for k in SIGNAL_POINTER_WORDS:
                        if ws[i + k]:
                            ws[i + k] = 0
                            n += 1
                    i += TRIGGER_WORDS
                else:
                    i += 1
    if report is not None and n:
        report["signal pointer words zeroed"] = report.get("signal pointer words zeroed", 0) + n
    return n


# ============================================================================
# the Edge bake
# ============================================================================
BONE_TRACK_FLAGS = 0x4900           # the PC release's bone tracks: one flags word per track (0x4000), 0x800, 0x100
# The PC release keys its bone tracks on (almost) every frame; the Edge blobs keep only the frames their compressor
# needed.  Sampling the clip on every list frame (Edge's own bracketing-key interpolation, EdgeChannel.sample) gives the
# PC player the density its data has.  False keeps the Edge key frames alone where the frequencies divide.
BAKE_EVERY_FRAME = True
TYPE_OF = {"T": 2, "R": 3, "S": 4}


def _pack(kind: str, v) -> bytes:
    if kind == "T":
        return struct.pack("<3f", *v)
    if kind == "R":
        return struct.pack("<4f", *v)
    return _diag_floats(*v)


def _qmul(a, b) -> tuple:
    """Quaternion product a * b, (x, y, z, w)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _qrot(q, v) -> tuple:
    """v rotated by the unit quaternion q (q v q*)."""
    p = _qmul(_qmul(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))
    return p[:3]


def _joint_value(chs: dict, kind: str, t: float, skeleton: E.Skeleton, j: int) -> tuple:
    """Edge joint j's value of `kind` at clip time t: its channel, else the ES01 base pose (what the evaluator starts
    every joint from)."""
    ch = chs.get(kind)
    return ch.sample(t) if ch is not None else tuple(skeleton.base_pose[j][kind])


def compose_root(root: dict, joint: dict, kind: str, t: float, skeleton: E.Skeleton, j: int) -> tuple:
    """A parentless joint's T or R in the model space of the list: joint 0 (T0 R0 S0) applied to the joint's own value,
    T = T0 + R0 (S0 T1), R = R0 R1.  On the 225 track lists both releases share whose root moves, this reproduces the
    PC release's pelvis tracks (largest difference 0.003 units, 0.06 degrees; the Edge values alone are off by up to
    24 units: the clip's root motion)."""
    t1 = _joint_value(joint, "T", t, skeleton, j)
    r1 = _joint_value(joint, "R", t, skeleton, j)
    t0 = _joint_value(root, "T", t, skeleton, 0)
    r0 = _joint_value(root, "R", t, skeleton, 0)
    s0 = _joint_value(root, "S", t, skeleton, 0)
    if kind == "T":
        d = _qrot(r0, (s0[0] * t1[0], s0[1] * t1[1], s0[2] * t1[2]))
        return tuple(f32(t0[i] + d[i]) for i in range(3))
    q = _qmul(r0, r1)
    nq = math.sqrt(sum(x * x for x in q))
    return tuple(f32(x / nq) for x in q) if nq > 1e-8 else (0.0, 0.0, 0.0, 1.0)


def _bake_events(kind: str, keys: list, looping: bool) -> list:
    """Events for keys [(frame, value)] (strictly increasing frames) and the terminator the lists end their tracks with:
    the loop point (base event only) in a looping list, a copy of the last key otherwise.  The PC player
    (EVE::f32_PlayOneTrackTrlAni 00527D60) blends events i and i + 1 and holds event n - 2 at the end."""
    evs = []
    for i, (f, v) in enumerate(keys):
        nxt = keys[i + 1][0] if i + 1 < len(keys) else f
        if not 0 <= nxt - f <= 0xFFFF:
            raise AnimError(f"frame delta {nxt - f}")
        evs.append({"flags": 0, "delta": nxt - f, "value": _pack(kind, v)})
    term = {"flags": 0, "delta": 0}
    if not looping:
        term["value"] = evs[-1]["value"]
    evs.append(term)
    return evs


def bake_edge(o: dict, anim: E.Anim, channels: list, skeleton: E.Skeleton | None, report: dict | None = None,
              extra_channels: dict | None = None) -> dict:
    """Replace the list's Edge animation by EVE tracks (see the module docstring); o is a level-3 parse, changed in place.
    channels: the skin's bone channel per rank (Edge joint); skeleton: its ES01 (base pose, joint hashes);
    extra_channels: {rank: [channel]} of other candidate skins (union_channels), baked with the same values."""
    rep = report if report is not None else {}
    n = anim.num_joints
    chans = list(channels)
    if len(chans) != n:
        raise AnimError(f"animation has {n} joints, skin {len(chans)} bones")
    if len(set(chans)) != n:
        raise AnimError("duplicate bone channels")
    if any(c > 0xFF for c in chans):
        raise AnimError("bone channel above 255 (the track channel is a byte)")
    if skeleton is not None and skeleton.joint_hashes and list(skeleton.joint_hashes) != chans:
        raise AnimError("ES01 joint hashes differ from the skin's bone channels")
    if anim.const_count["U"] or anim.anim_count["U"]:
        raise AnimError("Edge user channels have no EVE track")
    ver = o.get("version", 0)
    lf = o.get("frequency", 60) if ver >= 4 else 60
    if not lf or anim.frequency <= 0:
        raise AnimError(f"frequencies {lf} / {anim.frequency}")
    step = lf / anim.frequency
    integral = abs(step - round(step)) < 1e-6 and round(step) >= 1
    end = int(round((anim.num_frames - 1) * step))
    looping = bool(o.get("flags", 0) & 8)
    byj = collections.defaultdict(dict)
    for ch in anim.channels():
        byj[ch.joint][ch.kind] = ch
    tracks = o.setdefault("tracks", [])
    roots = {}
    for t in tracks:
        if t.get("type") in (2, 3, 4) and t.get("channel") == 0:
            roots.setdefault(t["type"], t)
    # parentless joints the blob animates, in a clip whose root the blob animates too: joint 0 composed with them
    composed = set()
    if skeleton is not None and skeleton.parents and byj.get(0):
        composed = {j for j in range(1, n) if j < len(skeleton.parents) and skeleton.parents[j] < 0 and byj.get(j)}
    baked, srcs = [], collections.Counter()
    for j in range(1, n):
        for kind in ("T", "R", "S"):
            ch = byj.get(j, {}).get(kind)
            if j in composed and kind != "S":
                keys, src = [(f, compose_root(byj[0], byj[j], kind, f / step, skeleton, j))
                             for f in range(end + 1)], "composed with the root"
            elif ch is None:
                if skeleton is None:
                    raise AnimError(f"joint {j} {kind}: no Edge channel and no ES01 base pose")
                v = tuple(skeleton.base_pose[j][kind])
                keys, src = ([(0, v), (end, v)] if end > 0 else [(0, v)]), "base pose"
            elif ch.const:
                v = ch.keys[0][1]
                keys, src = ([(0, v), (end, v)] if end > 0 else [(0, v)]), "constant"
            elif integral and not BAKE_EVERY_FRAME:
                s = int(round(step))
                keys, src = [(f * s, v) for f, v in ch.keys], "key frames"
            else:
                keys, src = [(f, ch.sample(f / step)) for f in range(end + 1)], "every frame"
            srcs[src] += 1
            typ = TYPE_OF[kind]
            root = roots.get(typ)
            for c in [chans[j]] + [x for x in (extra_channels or {}).get(j, []) if x <= 0xFF]:
                t = {"type": typ, "channel": c, "flags": BONE_TRACK_FLAGS, "modifier": 0, "param1": 0,
                     "obj_idx": root.get("obj_idx") if root else (0xFFFF if ver >= 0x19 else 0xFFFFFFFF),
                     "u16_a": root.get("u16_a", 0) if root else 0, "skeleton": root.get("skeleton", 0) if root else 0,
                     "param2": (root.get("param2", 0) & 0xFFFF) if root else 0}
                if ver < 0x19:
                    t["extra"] = 0
                t["events"] = _bake_events(kind, keys, looping)
                t["n_events"] = len(t["events"])
                baked.append(t)
    taken = {(t["channel"], t["type"]) for t in baked}
    kept = [t for t in tracks if not (t.get("type") in (2, 3, 4) and (t.get("channel"), t.get("type")) in taken)]
    replaced = len(tracks) - len(kept)
    at = 0
    while at < len(kept) and kept[at].get("type") in (2, 3, 4) and kept[at].get("channel") == 0:
        at += 1
    # track 0 ends on the clip's last frame (the list length the PC takes without Edge data)
    stretched = False
    if kept and kept[0].get("type") in (2, 3, 4) and len(kept[0].get("events", [])) >= 2:
        evs = kept[0]["events"]
        last = sum(e.get("delta", 0) for e in evs[:-1])
        if last < end:
            evs[-2]["delta"] = evs[-2].get("delta", 0) + (end - last)
            if evs[-2]["delta"] > (0xFF if evs[-2].get("flags", 0) & 0x1000 else 0xFFFF):
                raise AnimError("track 0 cannot be stretched to the clip length")
            stretched = True
    o["tracks"] = kept[:at] + baked + kept[at:]
    o["n_tracks"] = len(o["tracks"])
    o["edge_anim"] = b""
    o["edge_anim_size"] = 0
    rep.update({"edge joints": n, "edge frames": anim.num_frames, "tracks baked": len(baked),
                "keys baked": sum(len(t["events"]) for t in baked), "tracks replaced": replaced,
                "track 0 stretched": stretched})
    for k, v in srcs.items():
        rep["baked from " + k] = v
    return o


# ============================================================================
# which skin a list animates
# ============================================================================
ANIM_MODIFIERS = {6: "skl", 13: "eve", 14: "ani", 15: "aci", 37: "sp"}
ANIM_STREAMS = {
    "skl": (A.skl, A.refs_skl), "eve": (A.eve, A.refs_eve), "ani": (A.ani, A.refs_ani),
    "aci": (A.aci, A.refs_aci), "sp": (A.sp, A.refs_sp),
    "skn": (A.skn, A.refs_skn), "ack": (A.ack, A.refs_ack), "act": (A.act, A.refs_act),
    "trl": (A.trl, A.refs_trl), "mpk": (A.mpk, A.refs_mpk),
}


def skin_walk(big, rec_index: dict[int, int]):
    """The animation resources the worlds' modifiers reach, remembering which objects' skins reach each track list.
    Per world (in key order): the world's animation modifiers and those of its objects (with the skin of the object's
    SKL as owner), then everything their keys reach.  Returns (track lists {(package, key): body}, skin records
    {(package, key): body}, owners {(package, key): skin keys}, first package {(package, key): the package the walk
    reached the list from})."""
    arc = Archive(big)

    def body_of(pk, key):
        p = arc.package(pk)
        b = p.body(key) if p is not None else None
        if b is None and key in rec_index:
            q = arc.package(rec_index[key])
            b = q.body(key) if q is not None else None
            if b is not None:
                return rec_index[key], b
        return pk, b

    queue = collections.deque()
    seen_mod = set()
    for w in sorted(arc.worlds):
        p = arc.package(w)
        if p is None or p.body(w) is None:
            continue
        wo = F.parse(F.world, p.body(w))
        cands = [(r, None) for r in wo["mod_refs"] if r.type in ANIM_MODIFIERS]
        for r in wo["obj_refs"]:
            if not r.is_modifier:
                continue
            b = p.body(r.key)
            if b is None:
                continue
            ob = F.parse(F.obj, b)
            mods = [m for m in ob["refs"] if m.is_modifier and m.type in ANIM_MODIFIERS]
            owner = None
            for m in mods:
                if m.type == 6 and p.body(m.key) is not None:
                    try:
                        sk = F.parse(A.skl, p.body(m.key)).get("skn")
                        owner = sk or None
                    except Exception:      # noqa: BLE001
                        pass
            for m in mods:
                cands.append((m, owner))
        for m, owner in cands:
            dup = (p.key, m.key) in seen_mod
            seen_mod.add((p.key, m.key))
            queue.append((p.key, m.key, ANIM_MODIFIERS[m.type], owner, dup))

    done, children, reach = set(), {}, collections.defaultdict(set)
    trls, skins, first_pk = {}, {}, {}
    while queue:
        pk, key, kind, owner, dup = queue.popleft()
        if owner:
            reach[(pk, key, kind)].add(owner)
        if (pk, key, kind) in done or dup:
            continue
        done.add((pk, key, kind))
        where, b = body_of(pk, key)
        if b is None:
            continue
        if where != pk:
            if (where, key, kind) in done:
                continue
            done.add((where, key, kind))
        fn, refs_fn = ANIM_STREAMS[kind]
        try:
            o = F.parse(fn, b)
        except Exception:                  # noqa: BLE001
            continue
        if kind == "trl":
            trls[(where, key)] = b
            first_pk[(where, key)] = pk
        elif kind == "skn":
            skins[(where, key)] = b
        kids = [(k, rk) for k, rk in refs_fn(o) if rk in ANIM_STREAMS]
        children[(pk, key, kind)] = kids
        for k, rk in kids:
            queue.append((pk, k, rk, owner, False))
    for _ in range(20):                    # owners through shared kits / nested lists
        changed = False
        for node, kids in children.items():
            os_ = reach.get(node)
            if not os_:
                continue
            for k, rk in kids:
                tgt = (node[0], k, rk)
                if not os_ <= reach[tgt]:
                    reach[tgt] |= os_
                    changed = True
        if not changed:
            break
    owners = collections.defaultdict(set)
    for (pk, key, kind), os_ in reach.items():
        if kind == "trl":
            owners[(pk, key)] |= os_
    return trls, skins, owners, first_pk


class SkinIndex:
    """Wii track list key -> the skin models that can play it: the skins of the objects whose SKL modifier and action
    kits reach the list (skin_walk), the skins of the packages holding the list, and every skin's bone channels.  Kept
    in the cache folder."""

    def __init__(self, data: dict):
        self.owners = {int(k, 16): [int(s, 16) for s in v] for k, v in data["owners"].items()}
        self.package_skins = {int(k, 16): [int(s, 16) for s in v] for k, v in data["package_skins"].items()}
        self.trl_packages = {int(k, 16): [int(p, 16) for p in v] for k, v in data["trl_packages"].items()}
        self.channels = {int(k, 16): v for k, v in data["channels"].items()}
        self.package_trls = collections.defaultdict(list)
        for k, ps in self.trl_packages.items():
            for p in ps:
                self.package_trls[p].append(k)

    @classmethod
    def load(cls, big, rec_index: dict[int, int], path: str, log=None) -> "SkinIndex":
        fp = fingerprint(big)
        if os.path.exists(path):
            try:
                with open(path) as f:
                    doc = json.load(f)
                if doc.get("format") == SKIN_INDEX_FORMAT and doc.get("source") == fp:
                    return cls(doc)
            except (OSError, ValueError, KeyError):
                pass
        if log:
            log("indexing the skin models of %s" % os.path.basename(big.path))
        data = cls.build(big, rec_index)
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"format": SKIN_INDEX_FORMAT, "source": fp, **data}, f)
        os.replace(tmp, path)
        return cls(data)

    @staticmethod
    def build(big, rec_index: dict[int, int]) -> dict:
        trls, skin_bodies, owners, first_pk = skin_walk(big, rec_index)
        channels, package_skins = {}, collections.defaultdict(set)
        for (where, key), b in skin_bodies.items():
            package_skins[where].add(key)
            if key not in channels:
                channels[key] = E.skin_channels(F.parse(A.skn, b))
        # every skin model record of every package: the walk only reaches skins through world objects' SKL modifiers
        for e in big.entries():
            if e.key & 0xFFF00000 != 0xFFF00000:
                continue
            try:
                d = big.read(e)
                rl, _ = walk_package(d)
            except Exception:                  # noqa: BLE001
                continue
            for o_, k, ln in rl:
                b = d[o_ + 8:o_ + 8 + ln]
                if b"ES01" not in b:
                    continue
                try:
                    so = F.parse(A.skn, b)
                    skel = E.skin_skeleton(so)
                except Exception:              # noqa: BLE001
                    continue
                ch = E.skin_channels(so)
                if not ch or skel is None or (skel.joint_hashes and list(skel.joint_hashes) != ch):
                    continue
                package_skins[e.key].add(k)
                channels.setdefault(k, ch)
        own = collections.defaultdict(set)
        for (_pk, key), v in owners.items():
            own[key] |= {s for s in v if s}
        trl_packages = collections.defaultdict(set)
        for where, key in trls:
            trl_packages[key].add(where)
        for (where, key), pk in first_pk.items():
            trl_packages[key].add(pk)
        return {"owners": {"%08X" % k: ["%08X" % s for s in sorted(v)] for k, v in sorted(own.items())},
                "package_skins": {"%08X" % k: ["%08X" % s for s in sorted(v)] for k, v in sorted(package_skins.items())},
                "trl_packages": {"%08X" % k: ["%08X" % p for p in sorted(v)] for k, v in sorted(trl_packages.items())},
                "channels": {"%08X" % k: v for k, v in sorted(channels.items())}}

    def _layouts(self, cands, joints: int):
        ok = [s for s in sorted(set(cands)) if len(self.channels.get(s, ())) == joints]
        lay = collections.OrderedDict()
        for s in ok:
            lay.setdefault(tuple(self.channels[s]), []).append(s)
        return ok, lay

    def choose(self, key: int, joints: int, package: int | None = None) -> tuple[int, str, list]:
        """(skin key, how, other candidate skins with a different bone layout) for a list whose Edge blob has `joints`
        joints; (0, reason, []) when no skin fits.  Order: owner skins; the owner skins of the other lists of the list's
        packages, by votes (skins with the same channels can still differ in their ES01 base pose, which fills the kinds
        the blob leaves alone); skins of the list's packages when they agree on one bone layout; every skin when they
        agree; else the most common layout among the package skins, then among all skins ("guess")."""
        ok, lay = self._layouts(self.owners.get(key, ()), joints)
        if ok:
            if len(lay) == 1:
                return ok[0], "owner", []
            return ok[0], f"owner ({len(lay)} layouts)", [v[0] for v in list(lay.values())[1:]]
        pkgs = set(self.trl_packages.get(key, ())) | ({package} if package else set())
        votes = collections.Counter()
        for p in pkgs:
            for other in self.package_trls.get(p, ()):
                if other != key:
                    for s in self.owners.get(other, ()):
                        if len(self.channels.get(s, ())) == joints:
                            votes[s] += 1
        if votes:
            top = max(votes.values())
            best = min(s for s, v in votes.items() if v == top)
            _, vlay = self._layouts(votes, joints)
            others = [v[0] for t, v in vlay.items() if t != tuple(self.channels[best])]
            return best, "package owner vote" + (f" ({len(vlay)} layouts)" if len(vlay) > 1 else ""), others
        pk_skins = set()
        for p in pkgs:
            pk_skins |= set(self.package_skins.get(p, ()))
        pok, play = self._layouts(pk_skins, joints)
        if len(play) == 1:
            return pok[0], "package skin", []
        aok, alay = self._layouts(self.channels, joints)
        if len(alay) == 1:
            return aok[0], "joint count", []
        for lays, what in ((play, "package guess"), (alay, "guess")):
            if lays:
                best = max(lays.values(), key=len)
                return best[0], f"{what} ({len(lays)} layouts)", [v[0] for v in lays.values() if v is not best]
        return 0, f"no skin with {joints} bones", []

    def pick(self, key: int, joints: int, package: int | None = None) -> tuple[int, str]:
        sk, how, _ = self.choose(key, joints, package)
        return sk, how


def union_channels(chosen: list, others: list) -> dict | None:
    """{rank: [channel, ...]} the other candidate bone layouts add to the chosen one, or None when a channel sits on
    different ranks in two layouts (then only the chosen layout is baked).  A track on a channel the actual skin lacks is
    inert: EVE::ComputeObjParamsTrl 00599670 finds no bone (channel past SKL+0x4C or an empty slot of the channel table
    SKL+0x50), so the play functions return on the NULL object."""
    rank_of = {c: r for r, c in enumerate(chosen)}
    extra = collections.defaultdict(list)
    for lay in others:
        for r, c in enumerate(lay):
            if c in rank_of:
                if rank_of[c] != r:
                    return None
            else:
                rank_of[c] = r
                extra[r].append(c)
    return dict(extra)


def skin_index(ctx) -> SkinIndex:
    """The skin index of the context's Wii archive (built into the cache folder on first use)."""
    idx = getattr(ctx, "_skin_index", None)
    if idx is None:
        idx = SkinIndex.load(ctx.wii_big, ctx.wii_rec, ctx.cache_path("skins.json"), getattr(ctx, "log", None))
        ctx._skin_index = idx
    return idx


def skin_model(ctx, skin_key: int) -> tuple[list, E.Skeleton | None]:
    """(bone channels by rank, ES01 skeleton) of a Wii skin model record."""
    cache = getattr(ctx, "_skin_models", None)
    if cache is None:
        cache = ctx._skin_models = {}
    if skin_key not in cache:
        body = ctx.wii_body(skin_key) if ctx is not None else None
        if body is None:
            raise AnimError(f"skin model {skin_key:08X} not found in the Wii archive")
        so = F.parse(A.skn, body)
        cache[skin_key] = (E.skin_channels(so), E.skin_skeleton(so))
    return cache[skin_key]


# ============================================================================
# track lists
# ============================================================================
def _track_mode3_bytes(ver: int) -> int:
    """Bytes per track of the fields a level-3 list does not stream (anim._track mode 3)."""
    return (8 if ver >= 0x11 else 0) + (2 if ver >= 0x19 else 0) + 28 + (4 if ver >= 3 else 0)


def trl_memsize(old_memsize: int, old_len: int, old_tracks: int, new_len: int, new_tracks: int, ver: int) -> int:
    return old_memsize + (new_len - old_len) + _track_mode3_bytes(ver) * (new_tracks - old_tracks)


def convert_trl(key: int, body: bytes, ctx=None, *, skin: int | None = None, report: dict | None = None,
                union: bool = True) -> bytes:
    """union: when the skin choice leaves other candidate bone layouts, also bake their channels where no channel
    changes rank (union_channels)."""
    rep = report if report is not None else {}
    o = F.parse(A.trl, body)
    n_before = len(o.get("tracks", []))
    plain_storage(o, rep)
    zero_signal_pointers(o, rep)
    blob = o.get("edge_anim") or b""
    if blob:
        anim = E.Anim(blob)
        sk = skin
        if sk is None:
            sk = (getattr(ctx, "anim_skins", None) or {}).get(key)
        how, others = "given", []
        if sk is None:
            pk = getattr(ctx, "wii_rec", {}).get(key) if ctx is not None else None
            sk, how, others = skin_index(ctx).choose(key, anim.num_joints, pk)
            if not sk:
                raise AnimError(f"{key:08X}: {how}")
        chans, skel = skin_model(ctx, sk)
        rep["skin"] = "%08X" % sk
        rep["skin pick"] = how
        extra = None
        if union and others:
            lays = []
            for s in others:
                try:
                    lays.append(skin_model(ctx, s)[0])
                except AnimError:
                    continue
            extra = union_channels(chans, lays)
            rep["union channels"] = "conflict" if extra is None else sum(len(v) for v in extra.values())
            if extra:
                rep["union"] = {str(r): v for r, v in extra.items()}
        bake_edge(o, anim, chans, skel, rep, extra)
    out, _ = F.emit(A.trl, o)
    ms = struct.unpack_from("<I", body, 0)[0]
    new_ms = trl_memsize(ms, len(body), n_before, len(out), len(o.get("tracks", [])), o.get("version", 0))
    return struct.pack("<I", new_ms & 0xFFFFFFFF) + out[4:]


def convert(kind: str, key: int, wii_body: bytes, ctx=None, **kw) -> bytes:
    """Wii package record body of `kind` -> the PC record body.  Keywords: report={} (what was done), and for spec:trl
    skin=KEY (the skin model the Edge blob drives)."""
    if kind == "spec:skn":
        return convert_skn(wii_body, kw.get("report"))
    if kind == "spec:trl":
        return convert_trl(key, wii_body, ctx, skin=kw.get("skin"), report=kw.get("report"), union=kw.get("union", True))
    raise KeyError(f"the animation converter converts {', '.join(KINDS)}, not {kind}")
