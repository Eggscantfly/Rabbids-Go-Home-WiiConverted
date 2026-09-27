"""The Options screen of the port: an OPTIONS entry in the in-game pause menu and the Options page it opens.

    raw = patch_blob(key, raw, template)     a Magma blob (bigfile entry FBF0xxxx) with the additions, or None
    template = MenuTemplates.from_blob(raw)  the in-game menu blob the Options page copies its areas from

The additions are made at convert time from the user's own Magma blobs; nothing is shipped.

In-game pause menu (InGame.mgb, page IGM_P_E3, every language):
  CONTINUE / RESTART / SUBTITLES / EXIT become CONTINUE / RESTART / SUBTITLES / OPTIONS / EXIT.  The OPTIONS label is
  a copy of the EXIT label (same appear animation, same place and frames); EXIT, its mouse area and its animation move
  one row down (the SUBTITLES -> EXIT spacing of that blob) and appear one step later; the page's appear sounds get one
  more whoosh.  The OPTIONS mouse area reports mask 64 through the page's INGAME_OverElement command.  Script handle:
  generic object PCOPT_G_PauseOptions (the label).  The pause script (InGame_MagmaMenu_State_EXEC) navigates to it and
  pushes the Options page.

Options page (Common.mgb, every language; Common is loaded with every other Magma file, so any menu can push it):
  page PCOPT_P_Options, drawn and animated like the pause menu: the swirl vignette and a dimmed screen, the blue label
  buttons (a copy of the in-game label area, area PCOPT_A_Label) and the yellow selection arrows (a copy of the in-game
  arrows area, PCOPT_A_Arrows).  Every element the platform DLL drives has a generic object PCOPT_G_<name>:

    TabControls, TabAudio, TabGraphics    label buttons (tabs)
    Row<r>Name  (r 0..7)                  action names (Text)
    Row<r>Slot<s>  (s 0..2)               binding buttons (label buttons)
    Scroll, Note                          list position, and a note shown instead of the list (Text)
    Audio<a>Name  (a 0..3)                label buttons
    Audio<a>Track, Audio<a>Fill           volume bar frame and fill (RectShape; the fill's width is the volume)
    Audio<a>Value                         the volume in percent (Text)
    Gfx<g>Name, Gfx<g>Value  (g 0..6)     graphics setting names and values (label buttons, hidden until the graphics
                                          tab is shown)
    Reset, Back                           label buttons
    Hint                                  key help (Text)
    Arrows                                selection arrows
    CaptureDim, CapturePanel, CaptureText the "press a key" overlay (hidden until the DLL shows it)

  Element ids are PCOPT_E_<name>; ids are CRC-32 of the names (the scripts' and the DLL's lookups hash the same way).
  Label buttons use the label area's timeline like the pause menu: frame 0 selected, 10 not selected, 40 / 50 the
  greyed look.  Text is set at run time (MGM_SetWStringProperty, property 21).

Layout (Magma screen units, 854 x 480; label button of scale s placed at (x, y): its blob spans
[x - 30s, x + 166s] x [y - 2s, y + 50s], centre (x + 68s, y + 24s)) is in LAYOUT.  The DLL places the selection arrows
and tests the mouse against the same numbers (platform/wm_options.cpp, "the page"): change both together.
"""
from __future__ import annotations

from ..formats import magma as M

PAUSE_PAGE = "IGM_P_E3"
OPTIONS_PAGE = "PCOPT_P_Options"
LABEL_AREA = "PCOPT_A_Label"
ARROWS_AREA = "PCOPT_A_Arrows"
CURSOR_AREA = "CMN_Cursor"

# the in-game menu blob (InGame.mgb): areas copied into Common, elements of the pause page
IGM_LABEL_AREA = 0xC1EDFE16
IGM_ARROWS_AREA = 0x56E4875C
IGM_BUTTON_AREA = 0x5857B4CA
IGM_SUBTITLES_LABEL = 0xA739C986
IGM_EXIT_LABEL = 0x9409840E
IGM_EXIT_BUTTON = 0x8D8D78E0
IGM_SOUND_PLACEHOLDER = 0xCAF5C873
IGM_DIM = 0x25EE45B7                    # full-screen RectShape (black, alpha 0x96 after frame 3)
IGM_VIGNETTE = 0x3BF2CD98               # AreaInstance of Common's swirl vignette area
IGM_ARROWS = 0x96A50CD7                 # AreaInstance of the arrows area
COMMON_VIGNETTE_AREA = 0x0FC79F3E
OVER_ELEMENT = "INGAME_OverElement"
OPTIONS_OVER_MASK = 64

KEY_COMMAND_NAME, KEY_INT_VALUE = M.crc("CommandName"), M.crc("IntValue")
ACTION_STOP = M.crc("Stop")
KEY_STOP_AREA = 0x4BAB415E              # the Stop action's area link (Common page 1466033F uses it the same way)

ROWS = 8
SLOTS = 3
AUDIO_ROWS = 4
GFX_ROWS = 7
WHITE = 0xFFFFFFFF
BLUE = 0xFF2A5C9A
PANEL = 0xE0000000

LAYOUT = {
    "tab_scale": 0.8, "tabs": ((200, 42), (427, 42), (654, 42)),             # label centres
    "row_scale": 0.6, "row_y0": 102, "row_dy": 37, "slot_x0": 420, "slot_dx": 132, "name_x": (40, 340),
    "audio_scale": 0.8, "audio_y0": 112, "audio_dy": 62, "audio_label_cx": 230, "bar_x": (360, 680),
    "value_x": (690, 810),
    "gfx_y0": 100, "gfx_dy": 44, "gfx_name": (250, 0.75), "gfx_value": (575, 0.85),   # label centre x, scale
    "footer_scale": 0.75, "footer": ((249, 412), (503, 412)), "hint": (40, 814, 452, 476),
    "scroll": (40, 340, 398, 420),
    "capture_panel": (177, 677, 160, 320),
}


def eid(name: str) -> int:
    return M.crc("PCOPT_E_" + name)


def gid(name: str) -> int:
    return M.crc("PCOPT_G_" + name)


def label_pos(cx: float, cy: float, s: float) -> tuple[int, int]:
    """Placement of a label button of scale s whose blob is centred on (cx, cy)."""
    return round(cx - 68 * s), round(cy - 24 * s)


class MenuTemplates:
    """What the Options page copies from an in-game menu blob (the same in every language)."""

    def __init__(self, t: dict):
        self.types = t["header"]["types"]
        self.package = t["package"]["id"]
        self.label_area = M.find_area(t, IGM_LABEL_AREA)
        self.arrows_area = M.find_area(t, IGM_ARROWS_AREA)
        page = M.find_area(t, PAUSE_PAGE)
        self.dim = M.find_element(page, IGM_DIM)
        self.vignette = M.find_element(page, IGM_VIGNETTE)
        self.arrows = M.find_element(page, IGM_ARROWS)
        self.label = M.find_element(page, IGM_SUBTITLES_LABEL)
        self.text = self.label_area["elements"][3]
        if M.type_name(t, self.text["type"]) != "Text":
            raise M.FormatError("in-game label area: element 3 is not the Text")

    @classmethod
    def from_blob(cls, raw: bytes) -> "MenuTemplates":
        return cls(M.parse(raw))


def find_template(big) -> MenuTemplates | None:
    """The templates from the first in-game menu blob of a bigfile (FBF entries), or None when it has none."""
    for e in big.entries():
        if e.key >> 20 != 0xFBF or e.ext != "bin":
            continue
        raw = big.read(e)
        if not M.is_magma(raw):
            continue
        t = M.parse(raw)
        if kind_of(t) == "ingame":
            return MenuTemplates(t)
    return None


def kind_of(t: dict) -> str | None:
    ids = {a["id"] for a in t["package"]["areas"]}
    if M.crc(PAUSE_PAGE) in ids:
        return "ingame"
    if M.crc(CURSOR_AREA) in ids and M.crc("CMN_P_Message") in ids:
        return "common"
    return None


# ---------------------------------------------------------------------------------------------------------------------
# in-game pause menu
# ---------------------------------------------------------------------------------------------------------------------
def _script_int(caller: dict, command: str) -> dict | None:
    """The IntValue item of the caller's DoInScript action with that command, or None."""
    if not caller.get("has_executer"):
        return None
    for a in caller["executer"]["actions"]:
        d = {it["key"]: it for it in a["data"]}
        if d.get(KEY_COMMAND_NAME, {}).get("value") == command and KEY_INT_VALUE in d:
            return d[KEY_INT_VALUE]
    return None


def _renumber_keyframes(e: dict, prefix: str):
    for i, k in enumerate(e["keyframes"]):
        k["id"] = M.crc("%s_kf%d" % (prefix, i))


def patch_pause(t: dict) -> bool:
    """Add OPTIONS above EXIT on IGM_P_E3.  False when the page already has it."""
    page = M.find_area(t, PAUSE_PAGE)
    if any(e["id"] == eid("PauseOptionsLabel") for e in page["elements"]):
        return False
    sub = M.find_element(page, IGM_SUBTITLES_LABEL)
    exit_label = M.find_element(page, IGM_EXIT_LABEL)
    exit_button = M.find_element(page, IGM_EXIT_BUTTON)
    step_y = exit_label["keyframes"][-1]["state"]["pos"][1] - sub["keyframes"][-1]["state"]["pos"][1]
    step_f = exit_label["keyframes"][0]["frame"] - sub["keyframes"][0]["frame"]
    if step_y <= 0 or step_f <= 0:
        raise M.FormatError("%s: unexpected label order (step %d px, %d frames)" % (PAUSE_PAGE, step_y, step_f))

    label = M.clone(exit_label)
    label["id"] = eid("PauseOptionsLabel")
    label["widget"]["label"] = "OPTIONS"
    _renumber_keyframes(label, "PCOPT_E_PauseOptionsLabel")
    button = M.clone(exit_button)
    button["id"] = eid("PauseOptionsButton")
    for a in button["executer"]["actions"]:
        value = _script_int({"has_executer": 1, "executer": {"actions": [a]}}, OVER_ELEMENT)
        if value is None:
            raise M.FormatError("%s: EXIT's mouse area has another command" % PAUSE_PAGE)
        value["value"] = OPTIONS_OVER_MASK
    _renumber_keyframes(button, "PCOPT_E_PauseOptionsButton")

    for e in (exit_label, exit_button):
        for k in e["keyframes"]:
            k["frame"] += step_f
            k["state"]["pos"][1] += step_y

    sounds = M.find_element(page, IGM_SOUND_PLACEHOLDER)
    last = sounds["keyframes"][-1]
    whoosh = M.clone(sounds["keyframes"][-2])
    whoosh["frame"] = sounds["keyframes"][-2]["frame"] + step_f
    whoosh["id"] = M.crc("PCOPT_E_PauseSound_kf0")
    last["frame"] += step_f
    sounds["keyframes"].insert(len(sounds["keyframes"]) - 1, whoosh)

    els = page["elements"]
    els.insert(els.index(exit_label), label)
    els.insert(els.index(exit_button), button)

    link_type = next(g["link"]["last_type"] for g in t["package"]["generic_table"]["objects"]
                     if g["link"]["ids"][-1] == IGM_EXIT_LABEL)
    t["package"]["generic_table"]["objects"].append(
        {"id": gid("PauseOptions"), "link": {"ids": [t["package"]["id"], page["id"], label["id"]],
                                             "last_type": link_type}})
    return True


# ---------------------------------------------------------------------------------------------------------------------
# Options page
# ---------------------------------------------------------------------------------------------------------------------
class _Page:
    def __init__(self, t: dict, tpl: MenuTemplates):
        self.t, self.tpl = t, tpl
        self.pkg = t["package"]["id"]
        self.elements: list[dict] = []
        self.generic: list[dict] = []
        self.page_id = M.crc(OPTIONS_PAGE)
        self.ty = {n: M.type_byte(t, n) for n in ("Page", "Area", "Element", "Text", "RectShape", "Placeholder",
                                                   "AreaInstance", "ActionExecuter", "TickTimingStrategy",
                                                   "NoTimingStrategy")}

    def add(self, name: str, e: dict, handle: bool = True):
        e["id"] = eid(name)
        _renumber_keyframes(e, "PCOPT_E_" + name)
        self.elements.append(e)
        if handle:
            self.generic.append({"id": gid(name), "link": {"ids": [self.pkg, self.page_id, e["id"]],
                                                          "last_type": self.ty["Element"]}})
        return e

    @staticmethod
    def one_keyframe(e: dict, state: dict):
        k = e["keyframes"][0]
        k["frame"], k["interpolation"] = 0, 0
        k["data"], k["has_executer"] = [], 0
        k.pop("executer", None)
        k["state"] = state
        e["keyframes"] = [k]

    def text(self, name, rect, text="", height=28, align_x=1, hidden=False, wrap=False):
        e = M.clone(self.tpl.text)
        st = M.clone(e["keyframes"][0]["state"])
        st["rect"] = list(rect)
        st["origin"] = [(rect[1] - rect[0]) // 2, (rect[3] - rect[2]) // 2]
        st["height"] = st["leading"] = height
        st["color"] = WHITE
        self.one_keyframe(e, st)
        e["widget"]["text"] = text
        e["widget"]["align_x"] = align_x
        e["widget"]["wrapping"] = 1 if wrap else 0
        e["hidden"] = 1 if hidden else 0
        return self.add(name, e)

    def rect(self, name, rect, fill, outline=0, weight=2, hidden=False, handle=True):
        e = M.clone(self.tpl.dim)
        st = M.clone(e["keyframes"][-1]["state"])
        st["rect"] = list(rect)
        st["origin"] = [(rect[1] - rect[0]) // 2, (rect[3] - rect[2]) // 2]
        st["fill"] = [fill] * 4
        st["outline_color"] = outline or WHITE
        st["outline_weight"] = weight
        st["interpolation"] = 0
        self.one_keyframe(e, st)
        e["widget"]["outlined"] = 1 if outline else 0
        e["widget"]["filled"] = 1
        e["hidden"] = 1 if hidden else 0
        return self.add(name, e, handle)

    def label(self, name, cx, cy, s, text, hidden=False):
        e = M.clone(self.tpl.label)
        st = M.clone(e["keyframes"][-1]["state"])
        st["pos"] = list(label_pos(cx, cy, s))
        st["scale_x"] = st["scale_y"] = s
        st["origin"] = [0, 0]
        st["color"] = WHITE
        st["interpolation"] = 0
        self.one_keyframe(e, st)
        e["widget"]["label"] = text
        e["widget"]["link"] = {"timing": self.ty["TickTimingStrategy"], "package": self.pkg, "has_area": 1,
                               "area": M.crc(LABEL_AREA), "duplicated": 1}
        e["hidden"] = 1 if hidden else 0
        return self.add(name, e)

    def build(self):
        L = LAYOUT
        vig = M.clone(self.tpl.vignette)
        vig["widget"]["link"] = {"timing": self.ty["NoTimingStrategy"], "package": self.pkg, "has_area": 1,
                                 "area": COMMON_VIGNETTE_AREA, "duplicated": 0}
        st = M.clone(vig["keyframes"][-1]["state"])
        st["interpolation"] = 0
        self.one_keyframe(vig, st)
        self.add("Vignette", vig, handle=False)
        self.rect("Dim", (0, 854, 0, 480), 0x96000000, handle=False)

        s = L["tab_scale"]
        for name, (cx, cy), text in zip(("TabControls", "TabAudio", "TabGraphics"), L["tabs"],
                                        ("CONTROLS", "AUDIO", "GRAPHICS")):
            self.label(name, cx, cy, s, text)

        s = L["row_scale"]
        for r in range(ROWS):
            cy = L["row_y0"] + r * L["row_dy"]
            self.text("Row%dName" % r, (L["name_x"][0], L["name_x"][1], cy - 16, cy + 16), "", 28, align_x=0)
            for k in range(SLOTS):
                self.label("Row%dSlot%d" % (r, k), L["slot_x0"] + k * L["slot_dx"], cy, s, "")
        self.text("Scroll", L["scroll"], "", 22, align_x=0)
        self.text("Note", (40, 814, 180, 260), "", 30, hidden=True, wrap=True)

        s = L["audio_scale"]
        for a in range(AUDIO_ROWS):
            cy = L["audio_y0"] + a * L["audio_dy"]
            self.label("Audio%dName" % a, L["audio_label_cx"], cy, s, "")
            x0, x1 = L["bar_x"]
            self.rect("Audio%dTrack" % a, (x0, x1, cy - 13, cy + 13), 0x80000000, outline=WHITE)
            self.rect("Audio%dFill" % a, (x0 + 3, x1 - 3, cy - 10, cy + 10), BLUE)
            self.text("Audio%dValue" % a, (L["value_x"][0], L["value_x"][1], cy - 16, cy + 16), "", 28)

        (name_cx, name_s), (value_cx, value_s) = L["gfx_name"], L["gfx_value"]
        for g in range(GFX_ROWS):
            cy = L["gfx_y0"] + g * L["gfx_dy"]
            self.label("Gfx%dName" % g, name_cx, cy, name_s, "", hidden=True)
            self.label("Gfx%dValue" % g, value_cx, cy, value_s, "", hidden=True)

        s = L["footer_scale"]
        for name, (x, y), text in (("Reset", L["footer"][0], "DEFAULTS"), ("Back", L["footer"][1], "BACK")):
            self.label(name, x + 68 * s, y + 24 * s, s, text)
        hx0, hx1, hy0, hy1 = L["hint"]
        self.text("Hint", (hx0, hx1, hy0, hy1), "", 20)

        arrows = M.clone(self.tpl.arrows)
        st = M.clone(arrows["keyframes"][-1]["state"])
        st["pos"], st["origin"], st["color"], st["interpolation"] = [0, 0], [0, 0], WHITE, 0
        self.one_keyframe(arrows, st)
        arrows["widget"]["link"] = dict(arrows["widget"]["link"], package=self.pkg, area=M.crc(ARROWS_AREA))
        self.add("Arrows", arrows)

        self.rect("CaptureDim", (0, 854, 0, 480), 0xB0000000, hidden=True)
        px0, px1, py0, py1 = L["capture_panel"]
        self.rect("CapturePanel", (px0, px1, py0, py1), PANEL, outline=WHITE, weight=3, hidden=True)
        self.text("CaptureText", (px0 + 12, px1 - 12, py0 + 10, py1 - 10), "", 30, hidden=True, wrap=True)

        stop = M.clone(self.tpl.dim)                 # a Placeholder-like holder of the Stop action: the page's timeline
        st = M.clone(stop["keyframes"][0]["state"])  # ends at frame 1 (no appear animation loops)
        st["fill"] = [0, 0, 0, 0]
        self.one_keyframe(stop, st)
        k = stop["keyframes"][0]
        k["frame"] = 1
        k["has_executer"] = 1
        k["executer"] = {"type": self.ty["ActionExecuter"], "actions": [
            {"action": ACTION_STOP, "id": ACTION_STOP, "data": [
                {"key": KEY_STOP_AREA, "type": M.VAR_AREA_LINK,
                 "value": {"ids": [self.pkg, self.page_id], "last_type": self.ty["Page"]}}]}]}
        self.add("Timeline", stop, handle=False)


COMMON_FILE = "\\common.mgb"                 # how the other Magma files name Common (materials, fonts)


def _relink(o, src_pkg: int, src_area: int, pkg: int, area: int):
    """In place: links into the source area point at `area` of package `pkg`; material and font references to Common
    become references inside this blob (Common itself)."""
    if isinstance(o, dict):
        ids = o.get("ids")
        if isinstance(ids, list) and len(ids) >= 2 and ids[0] == src_pkg and ids[1] == src_area:
            o["ids"] = [pkg, area] + ids[2:]
        for key in ("material", "font_family"):
            r = o.get(key)
            if isinstance(r, dict) and r.get("present") and r.get("package", "").lower() == COMMON_FILE:
                r["package"] = ""
        for v in o.values():
            _relink(v, src_pkg, src_area, pkg, area)
    elif isinstance(o, list):
        for v in o:
            _relink(v, src_pkg, src_area, pkg, area)


def _copy_area(t: dict, src: dict, new_id: int, src_pkg: int):
    """An area copied into this blob (Common): its links to the source area point at the copy."""
    a = M.clone(src)
    a["id"] = new_id
    _relink(a, src_pkg, src["id"], t["package"]["id"], new_id)
    return a


def add_options_page(t: dict, tpl: MenuTemplates) -> bool:
    """Add the Options page (and the two areas it instances) to a Common blob.  False when it is already there."""
    ids = {a["id"] for a in t["package"]["areas"]}
    if M.crc(OPTIONS_PAGE) in ids:
        return False
    if t["header"]["types"] != tpl.types:
        raise M.FormatError("Common and InGame blobs have different type tables")
    areas = t["package"]["areas"]
    areas.append(_copy_area(t, tpl.label_area, M.crc(LABEL_AREA), tpl.package))
    areas.append(_copy_area(t, tpl.arrows_area, M.crc(ARROWS_AREA), tpl.package))
    p = _Page(t, tpl)
    p.build()
    _relink(p.elements, -1, -1, 0, 0)        # the elements' font references (copied from the in-game blob) are local
    page = {"type": p.ty["Page"], "id": p.page_id, "data": [], "has_executer": 0, "frame_rate": 25,
            "current_frame": 0, "elements": p.elements, "box": [0, 0, 0, 0], "default_elements": [],
            "global_selection": 1}
    areas.append(page)
    gt = t["package"]
    if gt["has_generic_table"] != 1:
        raise M.FormatError("Common blob without a generic object table")
    gt["generic_table"]["objects"].extend(p.generic)
    return True


def patch_blob(key: int, raw: bytes, template: MenuTemplates | None) -> bytes | None:
    """The blob with the Options additions, or None when it is not one of the menus they go into (or has them)."""
    if not M.is_magma(raw):
        return None
    t = M.parse(raw)
    kind = kind_of(t)
    if kind == "ingame":
        changed = patch_pause(t)
    elif kind == "common":
        if template is None:
            raise M.FormatError("%08X: Common blob but no in-game menu blob to copy the Options areas from" % key)
        changed = add_options_page(t, template)
    else:
        return None
    if not changed:
        return None
    M.set_census(t)
    return M.build(t)
