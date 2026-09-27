"""Magma menu packages: the "MAGMA" blobs of the bigfile entries FBF0xxxx (one per Magma file and language).

    t = parse(raw)            object tree (plain dicts and lists); build(t) == raw for an unchanged tree
    raw = build(t)            entry bytes (the 12-byte entry header, then the blob)
    set_census(t)             the 65 pool sizes recomputed from the tree (after adding or removing objects)

Loaders: magma::BinaryLoadVisitor (RGH Wii executable 804D96C8 ReadHeader .. 804DE7EC LoadMaterial; PC executable
0084A440 ReadHeader, 0084D4C0 VisitPackage).  Both builds read the same grammar; every field function below reads into
or writes from a dict, so parse and build share one code path.

Entry: u32 package key (little-endian), u32 size - 8, u32 size - 12, then the blob.  All numbers of the blob use its
own byte order (the marker tells: both orders occur on both discs).

Header    "MAGMA", u32 0xAB0000CD (byte-order marker), u32 0x001EAC58 (version), u8 data asserts (0), u8 N,
          (N - 1) x u32 type ids (CRC-32 of the ObjectTypeInfo names; the u8 type bytes of the data index this table)
Package   65 x i32 pool sizes (Census::Type, below), UserData, i16 x 2 display size, i16 x 2 display offset,
          u32 materials, u32 texture count, materials {id, str8 texture, f32 x 4 region}, font substitutes, fonts,
          font families {id, str8 font, [str8 package]}, areas {u8 kind, area}, char + string table,
          char + generic object table {id, i32 n, n x {id, FullLink}}, str8 default material
UserData  u32 id, u32 n, n x {u32 key, u32 type, value}: 2 i32, 7 f32, 12 bool (u8), 16 str8, 17/18/21 FullLink
          (area / element / keyframe), 19 {u32 string table, u32 string}, 20 nothing
FullLink  u16 n; n > 0: u8 last object type, n x u32 id (package, area, element, keyframe)
Area      ActionCaller, i32 frame rate, i32 current frame, i32 n, n x {u8 widget type, Element}, i16 x 4 box;
          Page + u32 n x {u8 input, u32 element} + char global selection; Button + i32 x 6 state frames;
          CheckBox + i32 x 12; Cursor + i16 x 2 hot spot
Element   ActionCaller, char hidden, char duplicatable, i32 mask mode, i32 n, n x Keyframe {ActionCaller, i32 frame,
          i32 interpolation, State}, widget; focusable classes + u32 n x {u8 input, u8 direction, u32 id} + char
          input controller

Ids are CRC-32 (zlib) of names (magma::Id::Hash, RGH 804E4A5C): pages, elements, types, actions, UserData keys; the
scripts use the same hash (MGM_GetObjectIDFromString).

Pool sizes (the 65 i32 of the package, Allocate*PoolChunk): the number of objects of each class, except
  * the five state pools (ScaleState, RectState, TextState, ImageState, RectShapeState): one more state per element
    of that state class (the element's current state) on top of the keyframe states;
  * TActionsIndex: the event slots of all event executers (every slot, empty or not).
That rule reproduces the stored sizes of all 96 Magma blobs of both discs.
"""
from __future__ import annotations

import copy
import struct
import zlib

MAGIC = b"MAGMA"
MARKER = 0xAB0000CD
VERSION = 0x001EAC58


def crc(s) -> int:
    """magma::Id::Hash: CRC-32 of the name."""
    if isinstance(s, str):
        s = s.encode("latin-1", "replace")
    return zlib.crc32(s) & 0xFFFFFFFF


# ObjectTypeInfo names (the type table every RGH blob carries names 91 of its 128 non-zero ids with these)
TYPE_NAMES = (
    "CreateObject", "AreaInstance", "Font", "ExternalFont", "NamedObject", "EngineObject", "Acceptor", "Area",
    "UserData", "ActionCaller", "Text", "Widget", "TextBase", "IScrollable", "AutonomousAreaInstance", "PageInstance",
    "Slider", "Focusable", "AreaLink", "Cursor", "Element", "PageFocusable", "Checkable", "Radioable", "Image",
    "RectShape", "Placeholder", "ButtonInstance", "CheckBoxInstance", "RadioButtonInstance", "Material", "FontFamily",
    "ScaleState", "State", "RotationState", "PosState", "RectState", "TextBaseState", "ImageState", "RectShapeState",
    "Window", "TextState", "Button", "CheckBox", "Page", "AnonymousType", "GlyphFont", "StringResource", "PixmapFont",
    "EngineObjectGroup", "Package", "StringTable", "EngineRoot", "Keyframe", "DisplayConfiguration", "Action",
    "Texture", "FullLink", "WindowSection", "ActionExecuter", "UserDataItem", "StretchableWindowSection",
    "ActionExecuterEvent", "ActionExecuterInputable", "ActionExecuterFocusable", "ActionExecuterEditbox",
    "ActionExecuterListbox", "ActionExecuterPage", "ActionExecuterPageInstance", "ActionExecuterSlider",
    "AreaHandler", "PageHandler", "Handler", "DrawHandler", "GenericObject", "EventTriggeredTimingStrategy",
    "TickTimingStrategy", "NoTimingStrategy", "SyncTimingStrategy", "TextScrollerPageHandler",
    "TextScrollerEventHandler", "TextScrollerDrawHandler", "EventHandler", "TimingStrategy", "ActionContinue",
    "ActionStop", "ActionPopPage", "ActionPushPage", "ActionGotoFrameIndex", "ActionGotoKeyFrame", "Viewport",
)
TYPE_BY_CRC = {crc(n): n for n in TYPE_NAMES}

AREA_INSTANCES = ("AreaInstance", "AutonomousAreaInstance", "ButtonInstance", "CheckBoxInstance",
                  "RadioButtonInstance", "PageInstance")
# Factory::MakeElement (RGH 804C9134): element class per widget
ELEMENT_CLASS = {"ButtonInstance": "Focusable", "Slider": "Focusable", "PageInstance": "PageFocusable",
                 "CheckBoxInstance": "Checkable", "RadioButtonInstance": "Radioable"}
# Factory::MakeState (RGH 804C88B0): keyframe state class per widget
STATE_CLASS = {"Image": "ImageState", "Text": "TextState", "RectShape": "RectShapeState", "Placeholder": "RectState",
               "Window": "RectState", "Slider": "ScaleState"}
STATE_CLASS.update({n: "ScaleState" for n in AREA_INSTANCES})
# executers with event slots (VisitActionExecuterEvent) and their slot counts
EVENT_SLOTS = {"ActionExecuterEvent": 3, "ActionExecuterInputable": 11, "ActionExecuterFocusable": 15,
               "ActionExecuterPage": 16, "ActionExecuterPageInstance": 16, "ActionExecuterSlider": 16,
               "ActionExecuterEditbox": 17, "ActionExecuterListbox": 17}
FONT_DATA = ("PixmapFont", "ExternalFont")

# UserData item types (magma::VariableType)
VAR_INT32, VAR_FLOAT32, VAR_BOOL, VAR_STRING = 2, 7, 12, 16
VAR_AREA_LINK, VAR_ELEMENT_LINK, VAR_STRING_ID, VAR_POINTER, VAR_KEYFRAME_LINK = 17, 18, 19, 20, 21

# Census::Type index -> class counted in that pool (VisitPackage)
CENSUS = {0: "Area", 1: "Page", 2: "Button", 3: "CheckBox", 4: "Cursor", 5: "Element", 6: "Focusable",
          7: "Checkable", 8: "Radioable", 11: "ScaleState", 12: "RectState", 13: "TextState", 15: "ImageState",
          16: "RectShapeState", 17: "Image", 18: "Text", 20: "RectShape", 23: "Slider", 24: "Placeholder",
          26: "AreaInstance", 27: "AutonomousAreaInstance", 28: "ButtonInstance", 29: "CheckBoxInstance",
          30: "RadioButtonInstance", 31: "Keyframe", 49: "StringResource", 50: "PageInstance", 51: "PageFocusable",
          52: "VariantContainer", 53: "FullLink", 54: "UserDataItem", 55: "ActionExecuter",
          56: "ActionExecuterEvent", 57: "ActionExecuterInputable", 58: "ActionExecuterFocusable",
          59: "ActionExecuterPage", 63: "ActionExecuterSlider", 64: "TActionsIndex"}
STATE_POOLS = ("ScaleState", "RectState", "TextState", "ImageState", "RectShapeState")

_SIZES = {"B": 1, "b": 1, "H": 2, "h": 2, "I": 4, "i": 4, "f": 4}


class FormatError(ValueError):
    pass


class _Stream:
    """Reads into dicts (data given) or writes from them (data None).  Counts are not stored in the tree: they are
    read into a fresh list and written from len(list)."""

    def __init__(self, data: bytes | None, endian: str):
        self.reading = data is not None
        self.data = data
        self.pos = 0
        self.out = bytearray()
        self.E = endian
        self.types: list[int] = []

    def _raw(self, fmt: str):
        size = _SIZES[fmt]
        if self.pos + size > len(self.data):
            raise FormatError("read past the end at 0x%X" % self.pos)
        v = struct.unpack_from(self.E + fmt, self.data, self.pos)[0]
        self.pos += size
        return v

    def num(self, fmt: str, o: dict, k: str):
        if self.reading:
            o[k] = v = self._raw(fmt)
            return v
        v = o[k]
        self.out += struct.pack(self.E + fmt, v)
        return v

    def u8(self, o, k):
        return self.num("B", o, k)

    def i8(self, o, k):
        return self.num("b", o, k)

    def u16(self, o, k):
        return self.num("H", o, k)

    def i16(self, o, k):
        return self.num("h", o, k)

    def u32(self, o, k):
        return self.num("I", o, k)

    def i32(self, o, k):
        return self.num("i", o, k)

    def f32(self, o, k):
        return self.num("f", o, k)

    def nums(self, fmt: str, n: int, o: dict, k: str) -> list:
        if self.reading:
            size = _SIZES[fmt] * n
            if self.pos + size > len(self.data):
                raise FormatError("read past the end at 0x%X" % self.pos)
            o[k] = v = list(struct.unpack_from(self.E + fmt * n, self.data, self.pos))
            self.pos += size
            return v
        v = o[k]
        if len(v) != n:
            raise FormatError("%s: %d values, %d expected" % (k, len(v), n))
        self.out += struct.pack(self.E + fmt * n, *v)
        return v

    def count(self, fmt: str, o: dict, k: str) -> int:
        if self.reading:
            n = self._raw(fmt)
            if n > len(self.data):
                raise FormatError("implausible count %d for %s at 0x%X" % (n, k, self.pos))
            o[k] = []
            return n
        n = len(o[k])
        self.out += struct.pack(self.E + fmt, n)
        return n

    def values(self, fmt: str, o: dict, k: str, n: int) -> list:
        if self.reading:
            size = _SIZES[fmt] * n
            if self.pos + size > len(self.data):
                raise FormatError("read past the end at 0x%X" % self.pos)
            o[k].extend(struct.unpack_from(self.E + fmt * n, self.data, self.pos))
            self.pos += size
            return o[k]
        self.out += struct.pack(self.E + fmt * n, *o[k])
        return o[k]

    def item(self, o: dict, k: str, i: int) -> dict:
        if self.reading:
            d: dict = {}
            o[k].append(d)
            return d
        return o[k][i]

    def sub(self, o: dict, k: str) -> dict:
        if self.reading:
            o[k] = {}
        return o[k]

    def str8(self, o: dict, k: str) -> str:
        if self.reading:
            n = self._raw("I")
            b = self.data[self.pos:self.pos + n]
            if len(b) != n:
                raise FormatError("string past the end at 0x%X" % self.pos)
            self.pos += n
            o[k] = s = b.decode("latin-1")
            return s
        b = o[k].encode("latin-1")
        self.out += struct.pack(self.E + "I", len(b)) + b
        return o[k]

    def wstr(self, o: dict, k: str) -> str:
        if self.reading:
            n = self._raw("I")
            if self.pos + 2 * n > len(self.data):
                raise FormatError("wide string past the end at 0x%X" % self.pos)
            o[k] = s = "".join(map(chr, struct.unpack_from(self.E + "H" * n, self.data, self.pos)))
            self.pos += 2 * n
            return s
        units = [ord(c) for c in o[k]]
        self.out += struct.pack(self.E + "I", len(units)) + struct.pack(self.E + "H" * len(units), *units)
        return o[k]

    def type_name(self, t: int) -> str:
        h = self.types[t - 1] if 1 <= t <= len(self.types) else 0
        name = TYPE_BY_CRC.get(h)
        if name is None:
            raise FormatError("type byte %d (id %08X) is not a known class (at 0x%X)" % (t, h, self.pos))
        return name


def _named(st, o):
    st.u32(o, "id")


def _full_link(st, o):
    """VisitFullLink (RGH 804DD3CC)."""
    n = st.count("H", o, "ids")
    if n:
        st.u8(o, "last_type")
        st.values("I", o, "ids", n)


def _user_data(st, o):
    """VisitUserData (RGH 804D990C)."""
    _named(st, o)
    n = st.count("I", o, "data")
    for i in range(n):
        it = st.item(o, "data", i)
        st.u32(it, "key")
        t = st.u32(it, "type")
        if t == VAR_INT32:
            st.i32(it, "value")
        elif t == VAR_FLOAT32:
            st.f32(it, "value")
        elif t == VAR_BOOL:
            st.u8(it, "value")
        elif t == VAR_STRING:
            st.str8(it, "value")
        elif t in (VAR_AREA_LINK, VAR_ELEMENT_LINK, VAR_KEYFRAME_LINK):
            _full_link(st, st.sub(it, "value"))
        elif t == VAR_STRING_ID:
            v = st.sub(it, "value")
            st.u32(v, "table")
            st.u32(v, "string")


def _load_ref(st, o, k):
    """LoadMaterial / LoadFontFamily: char present; present: u32 id, str8 package ("" = this blob)."""
    r = st.sub(o, k)
    if st.u8(r, "present"):
        st.u32(r, "id")
        st.str8(r, "package")


def _action_executer(st, ex, kind):
    """VisitActionExecuter (RGH 804DDAD4) + VisitActionExecuterEvent (804DDBF8)."""
    n = st.count("I", ex, "actions")
    for i in range(n):
        a = st.item(ex, "actions", i)
        st.u32(a, "action")
        _user_data(st, a)
    if kind in EVENT_SLOTS:
        n = st.count("I", ex, "events")
        for i in range(n):
            ev = st.item(ex, "events", i)
            m = st.count("I", ev, "actions")
            st.values("I", ev, "actions", m)
    elif kind != "ActionExecuter":
        raise FormatError("action executer %s" % kind)


def _action_caller(st, o):
    """VisitActionCaller (RGH 804DD9F0)."""
    _user_data(st, o)
    if st.u8(o, "has_executer"):
        ex = st.sub(o, "executer")
        _action_executer(st, ex, st.type_name(st.u8(ex, "type")))


def _state(st, s, cls):
    """VisitState (RGH 804DC85C) and the derived states."""
    st.u32(s, "interpolation")
    st.u32(s, "color")
    st.f32(s, "rotation")
    st.nums("h", 2, s, "origin")
    if cls == "ScaleState":
        st.nums("h", 2, s, "pos")
        st.f32(s, "scale_x")
        st.f32(s, "scale_y")
        return
    st.nums("h", 4, s, "rect")
    if cls == "TextState":
        st.f32(s, "rel_offset_y")
        st.i16(s, "abs_offset_y")
        st.u32(s, "shadow_color")
        st.u16(s, "height")
        st.i8(s, "shadow_x")
        st.i8(s, "shadow_y")
        st.i16(s, "leading")
        st.i16(s, "tracking")
    elif cls == "ImageState":
        st.u32(s, "shadow_color")
        st.i8(s, "shadow_x")
        st.i8(s, "shadow_y")
        st.f32(s, "tiling_x")
        st.f32(s, "tiling_y")
        st.f32(s, "offset_x")
        st.f32(s, "offset_y")
        st.u8(s, "flip_h")
        st.u8(s, "flip_v")
        st.u8(s, "actual_size")
        st.nums("I", 4, s, "colors")
    elif cls == "RectShapeState":
        st.u8(s, "outline_weight")
        st.u32(s, "outline_color")
        st.nums("I", 4, s, "fill")
        st.u32(s, "shadow_color")
        st.i8(s, "shadow_x")
        st.i8(s, "shadow_y")
    elif cls != "RectState":
        raise FormatError("state class %s" % cls)


def _area_link(st, o):
    """VisitAreaLink (RGH 804DD288): u8 timing strategy type, u32 package id, char has area; u32 area id; char
    using a duplicated area."""
    st.u8(o, "timing")
    st.u32(o, "package")
    if st.u8(o, "has_area"):
        st.u32(o, "area")
    st.u8(o, "duplicated")


def _window_section(st, o, stretch):
    _load_ref(st, o, "material")
    st.i32(o, "blending")
    st.u8(o, "alpha_first")
    st.u8(o, "flip_h")
    st.u8(o, "flip_v")
    st.u8(o, "rotated")
    if stretch:
        st.i32(o, "stretch")


def _widget(st, w, kind):
    if kind == "Text":                            # VisitTextBase (RGH 804DB3EC) + VisitText (804DB6A0)
        if st.u8(w, "external") == 1:
            st.u32(w, "table")
            st.u32(w, "string")
        else:
            st.wstr(w, "text")
        st.i32(w, "align_x")
        st.i32(w, "align_y")
        st.u8(w, "wrapping")
        st.u8(w, "clipping")
        st.u8(w, "ellipsis")
        st.u8(w, "autosized")
        if st.u8(w, "has_extra"):
            st.u32(w, "extra")
        _load_ref(st, w, "font_family")
        st.u8(w, "bold")
        st.u8(w, "italic")
        st.u8(w, "underlined")
        st.i32(w, "blending")
        st.u8(w, "alpha_first")
    elif kind == "Image":                         # RGH 804DB7E4
        _load_ref(st, w, "material")
        st.i32(w, "blending")
        st.u8(w, "alpha_first")
        st.i32(w, "address_u")
        st.i32(w, "address_v")
    elif kind == "RectShape":                     # RGH 804DB8F8
        st.u8(w, "outlined")
        st.u8(w, "filled")
        st.i32(w, "blending")
    elif kind == "Window":                        # RGH 804DBC04: fill, 4 corners, 4 edges
        st.u8(w, "single_corner")
        st.u8(w, "single_edge")
        if st.reading:
            w["sections"] = []
        for i in range(9):
            _window_section(st, st.item(w, "sections", i), i == 0 or i >= 5)
    elif kind == "Slider":                        # RGH 804DB9E4
        st.i32(w, "range_min")
        st.i32(w, "range_max")
        st.i32(w, "margin")
        st.i32(w, "orientation")
        st.i32(w, "order")
        st.u8(w, "snapping")
        for part in ("header", "footer", "knob", "track"):
            if st.u8(w, "has_" + part):
                _area_link(st, st.sub(w, part))
    elif kind in AREA_INSTANCES:                  # VisitAreaInstance (RGH 804DBF44) + VisitPageInstance (804DC13C)
        if st.u8(w, "external") == 1:
            st.u32(w, "table")
            st.u32(w, "string")
        else:
            st.wstr(w, "label")
        _load_ref(st, w, "material")
        if st.u8(w, "has_link"):
            _area_link(st, st.sub(w, "link"))
        st.u32(w, "index_offset")
        if kind == "PageInstance":
            n = st.count("I", w, "default_focus")
            for i in range(n):
                d = st.item(w, "default_focus", i)
                st.u8(d, "input")
                st.u8(d, "direction")
                st.u32(d, "id")
    elif kind != "Placeholder":
        raise FormatError("widget %s" % kind)


def _element(st, e, kind):
    """VisitElement (RGH 804DC51C) + VisitFocusable (804DC704)."""
    _action_caller(st, e)
    st.u8(e, "hidden")
    st.u8(e, "duplicatable")
    st.i32(e, "mask_mode")
    n = st.count("i", e, "keyframes")
    scls = STATE_CLASS.get(kind)
    if scls is None:
        raise FormatError("no state class for widget %s" % kind)
    for i in range(n):
        k = st.item(e, "keyframes", i)            # VisitKeyframe (RGH 804DD1D0)
        _action_caller(st, k)
        st.i32(k, "frame")
        st.i32(k, "interpolation")
        _state(st, st.sub(k, "state"), scls)
    _widget(st, st.sub(e, "widget"), kind)
    if kind in ELEMENT_CLASS:
        n = st.count("I", e, "neighbors")
        for i in range(n):
            nb = st.item(e, "neighbors", i)
            st.u8(nb, "input")
            st.u8(nb, "direction")
            st.u32(nb, "id")
        st.u8(e, "input_controller")


def _area(st, a, kind):
    """VisitArea (RGH 804DB1B4) + VisitPage (804DC260) / VisitButton (804DC378) / VisitCheckBox (804DC408)."""
    _action_caller(st, a)
    st.i32(a, "frame_rate")
    st.i32(a, "current_frame")
    n = st.count("i", a, "elements")
    for i in range(n):
        e = st.item(a, "elements", i)
        _element(st, e, st.type_name(st.u8(e, "type")))
    st.nums("h", 4, a, "box")
    if kind == "Page":
        n = st.count("I", a, "default_elements")
        for i in range(n):
            d = st.item(a, "default_elements", i)
            st.u8(d, "input")
            st.u32(d, "id")
        st.u8(a, "global_selection")
    elif kind == "Button":
        st.nums("i", 6, a, "timings")
    elif kind == "CheckBox":
        st.nums("i", 12, a, "timings")
    elif kind == "Cursor":
        st.nums("h", 2, a, "hotspot")
    elif kind != "Area":
        raise FormatError("area kind %s" % kind)


def _font_data(st, f, kind):
    if kind == "ExternalFont":
        st.i32(f, "size")
        st.i32(f, "offset_y")


def _package(st, p):
    """VisitPackage (RGH 804D9CBC)."""
    st.nums("i", 65, p, "census")
    _user_data(st, p)
    st.nums("h", 2, p, "display_size")
    st.nums("h", 2, p, "display_offset")
    nm = st.count("I", p, "materials")
    st.u32(p, "texture_count")
    for i in range(nm):
        m = st.item(p, "materials", i)
        _named(st, m)
        st.str8(m, "texture")
        st.nums("f", 4, m, "region")
    substs = set()
    n = st.count("I", p, "font_substs")
    for i in range(n):
        f = st.item(p, "font_substs", i)
        kind = st.type_name(st.u8(f, "type"))
        st.str8(f, "name")
        if kind in FONT_DATA:
            _font_data(st, f, kind)
        substs.add(st.str8(f, "subst"))
    n = st.count("I", p, "fonts")
    for i in range(n):
        f = st.item(p, "fonts", i)
        kind = st.type_name(st.u8(f, "type"))
        st.str8(f, "name")
        st.str8(f, "file")
        if f["name"] not in substs and kind in FONT_DATA:
            _font_data(st, f, kind)
    n = st.count("I", p, "font_families")
    for i in range(n):
        ff = st.item(p, "font_families", i)
        _named(st, ff)
        if st.str8(ff, "font"):
            st.str8(ff, "package")
    n = st.count("I", p, "areas")
    for i in range(n):
        a = st.item(p, "areas", i)
        _area(st, a, st.type_name(st.u8(a, "type")))
    if st.u8(p, "has_string_table") == 1:
        t = st.sub(p, "string_table")
        _named(st, t)
        n = st.count("i", t, "strings")
        for i in range(n):
            s = st.item(t, "strings", i)
            _named(st, s)
            st.wstr(s, "text")
    if st.u8(p, "has_generic_table") == 1:
        t = st.sub(p, "generic_table")
        _named(st, t)
        n = st.count("i", t, "objects")
        for i in range(n):
            g = st.item(t, "objects", i)
            _named(st, g)
            _full_link(st, st.sub(g, "link"))
    st.str8(p, "default_material")


def is_magma(raw: bytes) -> bool:
    return len(raw) >= 17 and raw[12:17] == MAGIC


def parse(raw: bytes) -> dict:
    if not is_magma(raw):
        raise FormatError("not a MAGMA entry")
    key, s8, s12 = struct.unpack_from("<III", raw, 0)
    if s8 != len(raw) - 8 or s12 != len(raw) - 12:
        raise FormatError("entry header sizes %d/%d for %d bytes" % (s8, s12, len(raw)))
    mk = raw[17:21]
    if mk == b"\xab\x00\x00\xcd":
        E = ">"
    elif mk == b"\xcd\x00\x00\xab":
        E = "<"
    else:
        raise FormatError("byte-order marker %s" % mk.hex())
    st = _Stream(raw, E)
    st.pos = 17
    t: dict = {"key": key, "endian": "big" if E == ">" else "little"}
    h = st.sub(t, "header")
    st.u32(h, "marker")
    if st.u32(h, "version") != VERSION:
        raise FormatError("version %08X" % h["version"])
    st.u8(h, "data_asserts")
    n = st._raw("B")
    st.nums("I", n - 1, h, "types")
    st.types = h["types"]
    _package(st, st.sub(t, "package"))
    if st.pos != len(raw):
        raise FormatError("%d bytes left after the package (at 0x%X)" % (len(raw) - st.pos, st.pos))
    return t


def build(t: dict) -> bytes:
    E = ">" if t["endian"] == "big" else "<"
    st = _Stream(None, E)
    h = t["header"]
    st.out += MAGIC
    st.u32(h, "marker")
    st.u32(h, "version")
    st.u8(h, "data_asserts")
    st.out += struct.pack("B", len(h["types"]) + 1)
    st.nums("I", len(h["types"]), h, "types")
    st.types = h["types"]
    _package(st, t["package"])
    total = 12 + len(st.out)
    return struct.pack("<III", t["key"], total - 8, total - 12) + bytes(st.out)


# ---------------------------------------------------------------------------------------------------------------------
# tree helpers
# ---------------------------------------------------------------------------------------------------------------------
def type_name(t: dict, b: int) -> str:
    h = t["header"]["types"]
    return TYPE_BY_CRC.get(h[b - 1] if 1 <= b <= len(h) else 0, "type%d" % b)


def type_byte(t: dict, name: str) -> int:
    """The u8 type byte of a class name in this blob's type table."""
    want = crc(name)
    for i, h in enumerate(t["header"]["types"]):
        if h == want:
            return i + 1
    raise FormatError("class %s is not in the type table" % name)


def census(t: dict) -> list[int]:
    """The 65 pool sizes of the tree (see the module docstring)."""
    c = [0] * 65
    inv = {v: k for k, v in CENSUS.items()}

    def add(name, n=1):
        if name in inv:
            c[inv[name]] += n

    def ud(o):
        add("UserDataItem", len(o["data"]))
        if o["data"]:
            add("VariantContainer")
        for it in o["data"]:
            if it["type"] in (VAR_AREA_LINK, VAR_ELEMENT_LINK, VAR_KEYFRAME_LINK):
                add("FullLink")

    def caller(o):
        ud(o)
        if o["has_executer"]:
            ex = o["executer"]
            kind = type_name(t, ex["type"])
            add(kind)
            for a in ex["actions"]:
                ud(a)
            add("TActionsIndex", len(ex.get("events", [])))

    p = t["package"]
    ud(p)
    for a in p["areas"]:
        add(type_name(t, a["type"]))
        caller(a)
        for e in a["elements"]:
            wk = type_name(t, e["type"])
            add(ELEMENT_CLASS.get(wk, "Element"))
            add(wk)
            add(STATE_CLASS[wk])                  # the element's current state
            caller(e)
            for k in e["keyframes"]:
                add("Keyframe")
                add(STATE_CLASS[wk])
                caller(k)
    if p["has_string_table"] == 1:
        add("StringResource", len(p["string_table"]["strings"]))
    return c


def set_census(t: dict) -> None:
    t["package"]["census"] = census(t)


def find_area(t: dict, ident) -> dict:
    want = ident if isinstance(ident, int) else crc(ident)
    for a in t["package"]["areas"]:
        if a["id"] == want:
            return a
    raise KeyError("no area %s" % (("%08X" % ident) if isinstance(ident, int) else ident))


def find_element(area: dict, ident: int) -> dict:
    for e in area["elements"]:
        if e["id"] == ident:
            return e
    raise KeyError("no element %08X in area %08X" % (ident, area["id"]))


def clone(o):
    return copy.deepcopy(o)
