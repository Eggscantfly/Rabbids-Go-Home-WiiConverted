"""Edge animation data of Rabbids Go Home (Wii): the EdgeAnimAnimation blob ('EA03', big-endian) that ends a track list
of list version >= 0x14, and the EdgeAnimSkeleton ('ES01') every skin model of version >= 8 carries.  This module
decodes both and evaluates the channels the way the Wii runtime does.

EdgeAnimAnimation 'EA03': layout from the RGH Wii executable's DWARF, reading from _edgeAnimEvaluate 804FFF14,
_edgeAnimGetBracketingKeyframes32 804FF5F4, _edgeAnimDecompressRotation 804FF4D4, edgeAnimProcessCommandList 80501240,
EDG::PlayAnim 8023597C.  All big-endian:
  +00 'EA03'  +04 f32 duration = (numFrames - 1) / sampleFrequency  +08 f32 sampleFrequency
  +0C u16 sizeHeader  +0E u16 numJoints  +10 u16 numFrames  +12 u16 numFrameSets
  +14 u16 evalBufferSizeRequired  +16 u16 x 4 numConst R/T/S/User  +1E u16 x 4 numAnim R/T/S/User
  +26 u16 flags  +28 u32 sizeJointsWeightArray  +2C u32 eaUserJointWeightArray (runtime)
  +30 offsets of: joint weights, frame set DMA array, frame set info array, const R, T, S, User data, packing specs
      (never set; the evaluator skips packed clips), custom data -- each relative to its own field, 0 = none;
      +54 3 pad words
  +60 channel tables, one u16 joint per channel: const R (padded to 8 entries), const T, S, U, animated R, T, S, U
      (padded to 4)
  constant values: R 6 bytes (48-bit quaternion), T and S 3 x f32, U f32
  frame set DMA: numFrameSets x {u32 size, u32 offset from the blob start}; info x {u16 base frame, u16 N}.  Set i
      covers frames base..base+N+1, base[i+1] = base[i] + N + 1, the last set (N = 0) sits on the final frame; the
      command list picks the set by binary search.
  frame set (contiguous, 16-aligned): 8 u16 block sizes (initial R, T, S, U, keys R, T, S, U; they include trailing
      alignment bytes), the initial blocks (every animated channel's value at the base frame), a bitmap of N bits per
      animated channel (R, T, S, U order, MSB first), the key streams (R 6 bytes, T / S 12, U 4 and 4-aligned).  Bit k
      set = a key at frame base + k + 1.  A channel is evaluated between the last key at or before the frame and the
      next one, the next set's initial value closing the set: lerp for T / S / U, slerp for R.
  48-bit quaternion: BE u16 w0, BE u32 w1; a = w0 & 0x7FFF, b = w1 >> 17, c = (w1 >> 2) & 0x7FFF, each
      * 1/23169.77 - 0.7071068; sqrt(1 - a^2 - b^2 - c^2) goes in at index w1 & 3 of (x, y, z, w).
  The runtime starts every joint from the skeleton's base pose; EDG::PlayAnim then writes joints 1..n-1 into the SKL
  bones (joint j = bone rank j); joint 0 is left to the list's EVE root tracks.  RGH's own EVE root tracks, exported
  from the same data, match this reading to 5e-5 on average.

EdgeAnimSkeleton 'ES01': see Skeleton; the joint name hashes are the bones' SKL channels.
"""
from __future__ import annotations

import bisect
import math
import struct

__all__ = ["EdgeError", "Anim", "Channel", "Skeleton", "rot48", "slerp", "skin_channels", "skin_skeleton"]


class EdgeError(ValueError):
    pass


_F32 = struct.Struct("<f")


def f32(x: float) -> float:
    """Round to an IEEE single, as the runtime's float arithmetic does."""
    return _F32.unpack(_F32.pack(x))[0]


def _bits_f32(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u))[0]


ROT48_SCALE = _bits_f32(0x3835065D)      # 1 / 23169.77: a 15-bit word onto [0, 1.414]
ROT48_BIAS = _bits_f32(0x3F3504F3)       # 0.70710677


def rot48(d: bytes, o: int = 0) -> tuple[float, float, float, float]:
    """_edgeAnimDecompressRotation (RGH 804FF4D4): a big-endian u16 then u32.  Three 15-bit components mapped by
    v * 4.316e-5 - 0.7071, the fourth off the unit length, placed by the low two bits of the u32 (the index of the
    dropped component).  Returns the Edge quaternion (x, y, z, w)."""
    w0 = (d[o] << 8) | d[o + 1]
    w1 = (d[o + 2] << 24) | (d[o + 3] << 16) | (d[o + 4] << 8) | d[o + 5]
    a = f32(f32(ROT48_SCALE * (w0 & 0x7FFF)) - ROT48_BIAS)
    b = f32(f32(ROT48_SCALE * (w1 >> 17)) - ROT48_BIAS)
    c = f32(f32(ROT48_SCALE * ((w1 >> 2) & 0x7FFF)) - ROT48_BIAS)
    t = f32(f32(f32(1.0 - f32(a * a)) - f32(b * b)) - f32(c * c))
    r = f32(math.sqrt(t)) if t > 0.0 else 0.0
    sel = w1 & 3
    if sel == 0:
        return (r, a, b, c)
    if sel == 1:
        return (a, r, b, c)
    if sel == 2:
        return (a, b, r, c)
    return (a, b, c, r)


def slerp(qa, qb, u: float, threshold: float = 0.9995):
    """Shortest-arc spherical interpolation, normalised; above the dot-product threshold a normalised lerp."""
    d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]
    if d < 0.0:
        qb = (-qb[0], -qb[1], -qb[2], -qb[3])
        d = -d
    if d > threshold or u <= 0.0:
        out = [f32(qa[i] + f32((qb[i] - qa[i]) * u)) for i in range(4)]
    else:
        th = math.acos(min(d, 1.0))
        s = math.sin(th)
        wa = math.sin((1.0 - u) * th) / s
        wb = math.sin(u * th) / s
        out = [f32(qa[i] * wa + qb[i] * wb) for i in range(4)]
    n = math.sqrt(sum(v * v for v in out))
    if n > 1e-8:
        return tuple(f32(v / n) for v in out)
    return (0.0, 0.0, 0.0, 1.0)


def _lerp3(a, b, u: float):
    return tuple(f32(a[i] + f32(f32(b[i] - a[i]) * u)) for i in range(len(a)))


def _align(n: int, a: int) -> int:
    return (n + a - 1) & ~(a - 1)


# ============================================================================
# EdgeAnimAnimation
# ============================================================================
KINDS = ("R", "T", "S", "U")
ELEM = {"R": 6, "T": 12, "S": 12, "U": 4}


class Channel:
    """One channel: kind R/T/S/U, the joint (user channel for U) it drives, constant or animated, and its keys
    [(frame, value)] -- value (x, y, z, w) for R, (x, y, z) for T and S, (v,) for U.  A constant channel has the single
    key (0, value)."""
    __slots__ = ("kind", "index", "joint", "const", "keys", "frames")

    def __init__(self, kind, index, joint, const, keys):
        self.kind, self.index, self.joint, self.const, self.keys = kind, index, joint, const, keys
        self.frames = [k[0] for k in keys]

    def sample(self, frame: float, rot_threshold: float = 0.99):
        """The value at `frame` (clip frames, clamped): the last key at or before floor(frame) blended toward the first
        key after it by (frame - fa) / (fb - fa) -- _edgeAnimGetBracketingKeyframes32 over the frame set, whose end
        key is the next frame set's initial value.  Rotations are lerped above the dot-product `rot_threshold`."""
        keys = self.keys
        if self.const or len(keys) == 1:
            return keys[0][1]
        if frame <= 0.0:
            return keys[0][1]
        last = keys[-1][0]
        if frame >= last:
            return keys[-1][1]
        L = int(frame)
        i = bisect.bisect_right(self.frames, L) - 1
        fa, va = keys[i]
        fb, vb = keys[i + 1]
        u = f32((frame - fa) / (fb - fa))
        if u <= 0.0:
            return va
        if self.kind == "R":
            return slerp(va, vb, u, rot_threshold)
        return _lerp3(va, vb, u)


class FrameSet:
    __slots__ = ("index", "base", "n", "offset", "size", "sizes", "init", "bitmap", "keys", "end")


class Anim:
    """An EdgeAnimAnimation blob ('EA03', big-endian, as RGH ships it)."""

    def __init__(self, blob: bytes):
        if len(blob) < 0x60:
            raise EdgeError(f"Edge animation of {len(blob)} bytes")
        self.blob = bytes(blob)
        tag = self.blob[:4]
        if tag != b"EA03":
            raise EdgeError(f"not an EA03 Edge animation: tag {tag!r}")
        self.tag = tag.decode()
        self.e = e = ">"
        b = self.blob
        (self.duration, self.frequency) = struct.unpack_from(e + "ff", b, 4)
        (self.size_header, self.num_joints, self.num_frames, self.num_frame_sets,
         self.eval_buffer_size) = struct.unpack_from(e + "5H", b, 0x0C)
        c = struct.unpack_from(e + "8H", b, 0x16)
        self.const_count = dict(zip(KINDS, c[:4]))
        self.anim_count = dict(zip(KINDS, c[4:]))
        self.flags = struct.unpack_from(e + "H", b, 0x26)[0]
        (self.size_joint_weights, self.ea_user_joint_weights) = struct.unpack_from(e + "II", b, 0x28)
        self.raw_offsets = struct.unpack_from(e + "9I", b, 0x30)
        self.pad = b[0x54:0x60]
        names = ("joint_weights", "frame_set_dma", "frame_set_info", "const_R", "const_T",
                 "const_S", "const_U", "packing_specs", "custom_data")
        # every offset is relative to its own field; 0 = none
        self.off = {n: (0x30 + 4 * i + v if v else 0) for i, (n, v) in enumerate(zip(names, self.raw_offsets))}
        if self.off["packing_specs"]:
            raise EdgeError("bit-packed Edge animation (offsetPackingSpecs): _edgeAnimEvaluate skips those")
        # channel tables at 0x60: const R (padded to 8 entries), const T, S, U, anim R, T, S, U (to 4)
        o = 0x60
        self.const_table, self.anim_table = {}, {}
        for k in KINDS:
            n = self.const_count[k]
            self.const_table[k] = list(struct.unpack_from(f"{e}{n}H", b, o))
            o += 2 * _align(n, 8 if k == "R" else 4)
        for k in KINDS:
            n = self.anim_count[k]
            self.anim_table[k] = list(struct.unpack_from(f"{e}{n}H", b, o))
            o += 2 * _align(n, 4)
        self.tables_end = o
        if o > len(b):
            raise EdgeError("channel tables past the blob")
        self._read_constants()
        self._read_frame_sets()

    # ---- constants ------------------------------------------------------------
    def _read_constants(self):
        b, e = self.blob, self.e
        self.const_values = {}
        for k in KINDS:
            n = self.const_count[k]
            at = self.off[f"const_{k}"]
            if n and not at:
                raise EdgeError(f"{n} constant {k} channels without data")
            if at + n * ELEM[k] > len(b):
                raise EdgeError(f"constant {k} data past the blob")
            if k == "R":
                vals = [rot48(b, at + 6 * i) for i in range(n)]
            elif k == "U":
                vals = [struct.unpack_from(e + "f", b, at + 4 * i) for i in range(n)]
            else:
                vals = [struct.unpack_from(e + "3f", b, at + 12 * i) for i in range(n)]
            self.const_values[k] = vals

    # ---- frame sets -----------------------------------------------------------
    def _read_frame_sets(self):
        b, e = self.blob, self.e
        n_anim = sum(self.anim_count.values())
        self.frame_sets = []
        if not self.num_frame_sets:
            if n_anim:
                raise EdgeError("animated channels without frame sets")
            return
        dma, info = self.off["frame_set_dma"], self.off["frame_set_info"]
        if not dma or not info or dma + 8 * self.num_frame_sets > len(b) or info + 4 * self.num_frame_sets > len(b):
            raise EdgeError("frame set arrays missing or past the blob")
        if n_anim and self.num_frame_sets < 2:
            raise EdgeError("animated channels need two frame sets (the second holds the end keys)")
        expect_base = 0
        for i in range(self.num_frame_sets):
            fs = FrameSet()
            fs.index = i
            fs.size, fs.offset = struct.unpack_from(e + "II", b, dma + 8 * i)
            fs.base, fs.n = struct.unpack_from(e + "HH", b, info + 4 * i)
            if fs.base != expect_base:
                raise EdgeError(f"frame set {i} starts at frame {fs.base}, {expect_base} expected")
            expect_base = fs.base + fs.n + 1
            at = fs.offset
            if at + 16 > len(b) or at + fs.size > len(b):
                raise EdgeError(f"frame set {i} past the blob")
            fs.sizes = struct.unpack_from(e + "8H", b, at)
            p = at + 16
            fs.init = {}
            for j, k in enumerate(KINDS):
                cnt = self.anim_count[k]
                pad = fs.sizes[j] - cnt * ELEM[k]         # block sizes include trailing alignment bytes
                if not 0 <= pad < 16:
                    raise EdgeError(f"frame set {i}: initial {k} block of {fs.sizes[j]} bytes for {cnt} channels")
                fs.init[k] = p
                p += fs.sizes[j]
            nbits = fs.n * n_anim
            fs.bitmap = b[p:p + (nbits + 7) // 8]
            p += (nbits + 7) // 8
            fs.keys = {}
            for j, k in enumerate(KINDS):
                if k == "U":
                    p = at + _align(p - at, 4)            # the user keys are 4-aligned
                fs.keys[k] = p
                p += fs.sizes[4 + j]
            fs.end = p
            if p > len(b):
                raise EdgeError(f"frame set {i} keys past the blob")
            nxt = at + _align(p - at, 16)
            if i + 1 < self.num_frame_sets:
                o2 = struct.unpack_from(e + "I", b, dma + 8 * (i + 1) + 4)[0]
                if o2 != nxt:
                    raise EdgeError(f"frame set {i + 1} at {o2:#x}, {nxt:#x} expected (16-aligned after set {i})")
            self.frame_sets.append(fs)
        last = self.frame_sets[-1]
        if last.base + last.n != self.num_frames - 1:
            raise EdgeError(f"frame sets end at frame {last.base + last.n}, clip has {self.num_frames} frames")
        # the key streams must hold exactly the set bits
        for fs in self.frame_sets:
            region = 0
            for j, k in enumerate(KINDS):
                cnt = self.anim_count[k]
                ones = sum(self._bit(fs, region + c * fs.n + f) for c in range(cnt) for f in range(fs.n))
                if not 0 <= fs.sizes[4 + j] - ones * ELEM[k] < 16:
                    raise EdgeError(f"frame set {fs.index}: {ones} {k} keys, block of {fs.sizes[4 + j]} bytes")
                region += cnt * fs.n

    @staticmethod
    def _bit(fs: FrameSet, k: int) -> int:
        return (fs.bitmap[k >> 3] >> (7 - (k & 7))) & 1

    def _value(self, kind: str, at: int):
        b, e = self.blob, self.e
        if kind == "R":
            return rot48(b, at)
        if kind == "U":
            return struct.unpack_from(e + "f", b, at)
        return struct.unpack_from(e + "3f", b, at)

    # ---- channels ---------------------------------------------------------------
    def channels(self) -> list[Channel]:
        """Every channel with its keys: constant channels first (R, T, S, U), then the animated ones.  An animated
        channel's keys are, per frame set, its initial value at the set's base frame and a key at base + k + 1 for every
        set bit k of its N-bit region (_edgeAnimGetBracketingKeyframes32); the last frame set (N = 0) holds the values
        of the final frame."""
        if getattr(self, "_channels", None) is not None:
            return self._channels
        out = []
        for k in KINDS:
            for i, (j, v) in enumerate(zip(self.const_table[k], self.const_values[k])):
                out.append(Channel(k, i, j, True, [(0, v)]))
        for k in KINDS:
            per = [[] for _ in range(self.anim_count[k])]
            before = sum(self.anim_count[x] for x in KINDS[:KINDS.index(k)])
            for fs in self.frame_sets:
                start = before * fs.n                        # the kind's region of this set's bitmap
                seen = 0                                     # keys of the earlier channels of this kind
                for c in range(self.anim_count[k]):
                    per[c].append((fs.base, self._value(k, fs.init[k] + c * ELEM[k])))
                    cb = start + c * fs.n
                    for f in range(fs.n):
                        if self._bit(fs, cb + f):
                            per[c].append((fs.base + f + 1, self._value(k, fs.keys[k] + seen * ELEM[k])))
                            seen += 1
            for c in range(self.anim_count[k]):
                out.append(Channel(k, c, self.anim_table[k][c], False, per[c]))
        self._channels = out
        return out


# ============================================================================
# EdgeAnimSkeleton
# ============================================================================
class Skeleton:
    """An EdgeAnimSkeleton blob ('ES01' big-endian; layout from the RGH Wii executable's DWARF):
        +00 tag  +04 u32 sizeTotalWithoutNames  +08 u32 sizeTotal
        +0C u16 numJoints  +0E u16 numUserChannels  +10 u16 numSimdHierarchyQuads
        +14 offsetBasePose  +18 offsetParentIndicesArray  +1C offsetJointNameHashArray
        +20 offsetUserChannelNameHashArray  +24 offsetUserChannelNodeNameHashArray
        +28 offsetUserChannelFlagsArray  +2C offsetCustomData  +30 simdHierarchy[]
    (offsets relative to their own field, 0 = none).  The base pose is numJoints x EdgeAnimJointTransform: quaternion
    (x, y, z, w), translation (x, y, z, 1), scale (x, y, z, 1) as floats."""

    def __init__(self, blob: bytes):
        b = bytes(blob)
        if len(b) < 0x30:
            raise EdgeError(f"Edge skeleton of {len(b)} bytes")
        tag = b[:4]
        if tag != b"ES01":
            raise EdgeError(f"not an ES01 skeleton: tag {tag!r}")
        self.blob = b
        self.tag = tag.decode()
        e = ">"
        (self.size_without_names, self.size_total) = struct.unpack_from(e + "II", b, 4)
        (self.num_joints, self.num_user_channels, self.num_simd_quads) = struct.unpack_from(e + "3H", b, 0x0C)
        raw = struct.unpack_from(e + "7I", b, 0x14)
        names = ("base_pose", "parents", "joint_hashes", "user_hashes", "user_node_hashes", "user_flags", "custom")
        self.off = {n: (0x14 + 4 * i + v if v else 0) for i, (n, v) in enumerate(zip(names, raw))}
        n = self.num_joints
        if not self.off["base_pose"] or self.off["base_pose"] + 0x30 * n > len(b):
            raise EdgeError("skeleton base pose missing or past the blob")
        self.base_pose = []
        for j in range(n):
            v = struct.unpack_from(e + "12f", b, self.off["base_pose"] + 0x30 * j)
            self.base_pose.append({"R": v[0:4], "T": v[4:7], "S": v[8:11]})
        self.parents = list(struct.unpack_from(f"{e}{n}h", b, self.off["parents"])) if self.off["parents"] else []
        self.joint_hashes = list(struct.unpack_from(f"{e}{n}I", b, self.off["joint_hashes"])) if self.off["joint_hashes"] else []
        self.simd_hierarchy = list(struct.unpack_from(f"{e}{4 * self.num_simd_quads}H", b, 0x30))


def skin_channels(skn_o: dict) -> list[int]:
    """The channel of every bone rank of a parsed skin model (the bone reference's UserFlags low word,
    SKL_tt_BoneModel::u16_Channel).  Edge joint j = rank j: RGH's ES01 joint name hashes are exactly these channels."""
    return [r.user & 0xFFFF for r in skn_o.get("bones", [])]


def skin_skeleton(skn_o: dict) -> Skeleton | None:
    blob = skn_o.get("edge_skeleton") or b""
    return Skeleton(blob) if blob[:4] == b"ES01" else None
