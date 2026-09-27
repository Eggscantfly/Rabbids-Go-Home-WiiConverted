"""The port folder: the converted bigfile with the files the RGH PC executable needs around it.

    report = assemble(pc_dir, out_dir, pc_exe=None, bigfile_name=None, lang="en", log=print)
    report = check_port(out_dir)
    copy_siblings(wii_main_bigfile, converted_bigfile)

  executable   the game's program in the PC folder (GAME_EXE, else the folder's only program), copied as it is; its
               SHA-256 is reported, and a build other than the one the platform DLL was written for
               (SUPPORTED_EXECUTABLES) is a warning, not a stop: the DLL verifies byte signatures before every patch.
               `pc_exe` (the command line's --pc-exe) names another executable file to copy instead.
  shaders      the release's shaders folder, which the executable loads its effects from, with the Wii lighting and
               glow changes of shaders.py applied to the copy
  video        the Bink video library the executable imports
  platform     the platform DLL built from platform/ (build/wiimote.dll) and its configuration (wiimote.ini, kept when
               the port folder already has one)
  sav          the save folder the platform DLL keeps the PC save files in
  siblings     copies of the Wii sibling bigfiles, named after the converted bigfile (SIBLINGS)
  launch       the command line the launcher starts the game with: [launch] switches= of options.ini (the release's
               switches, LAUNCH_SWITCHES); no batch file is written, the launcher finds the executable and the
               bigfile in the folder
"""
from __future__ import annotations

import collections
import hashlib
import json
import os
import re
import shutil
import struct

from ..archive.bigfile import FILE_HDR, Big, is_bigfile
from .shaders import PATCHES as SHADER_PATCHES, patch_shaders, shader_state

SUPPORTED_EXECUTABLES = {
    "208054AF049A1E72EBEA3ADC7FA9BC5FABAD162C47861E52FE81B86FACEED502":
        "Rabbids Go Home PC release executable (2010 build, PE32, image base 0x00400000)",
}
# The same executables by the SHA-256 of their code: every section but .rsrc, in header order (code_sha256).  A port
# folder's copy with another icon (assemble --icon; peicon.py) has another file hash but the same code hash.
SUPPORTED_CODE = {
    "CBE01CE4B11CAAF0B24D8DBF565616091E154294736DEF5E419DB0BC10CC789B":
        "Rabbids Go Home PC release executable (2010 build, PE32, image base 0x00400000), its icon changed",
}
GAME_EXE = "LyN_f.exe"                   # the PC release's program; a folder without it: its only program (game_executable)
_NOT_GAME = ("launcher", "unins", "setup")   # program names that are never the game's
SHADERS_DIR = "shaders"
VIDEO_LIBRARY = "binkw32.dll"
PLATFORM_DLL = "wiimote.dll"
PLATFORM_INI = "wiimote.ini"
LAUNCHER_BUILD = "launcher"              # platform/build/launcher: the deployed launcher (build_launcher.bat)
LAUNCHER_EXE = "WiiConverted Launcher.exe"   # the stub in the port folder: the WC icon, starts launcher/RGHLauncher.exe
OLD_LAUNCHERS = ("Rabbids Go Home.exe",)     # earlier names of the stub, removed from a port folder on assemble
LAUNCHER_DIR = "launcher"                # the Qt window and its runtime, next to the stub
SAV_DIR = "sav"
DEFAULT_ICON = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "moon.ico")
                                         # the moon: the icon the port folder's executable gets unless told otherwise
OLD_LAUNCH_FILE = "play.cmd"             # the batch file earlier setups wrote into the port folder: removed on assemble
OPTIONS_INI = "options.ini"              # the game's settings next to the executable; [launch] switches= is the command
                                         # line the launcher starts the game with (the launcher writes it too)

# The sibling bigfiles next to the converted one: (branch, extension) of "<base>.<branch>.<extension>.bf".  Streamed
# sounds and videos are shadow shortcuts; in binary loading the executable opens "<main bigfile path minus
# extension>.<branch>.<extension>.bf" (BIG_S_P4::b_OpenDynAccess 006BDA80, Shadow_u32_GetShadowBFIndex), and the Wii
# sample records name ".wii.sns".
SIBLINGS = (("wii", "sns"), ("$hd$", "bik"))

# The converted bigfile's name: the executable gives a bigfile whose file name starts with "RGH" its BIG flag 1, as the
# release's own bigfile has; the siblings are named from the part before the extension, so the base has no dot.
BIGFILE_PREFIX = "RGH"
_BASE_CHARS = re.compile(r"[A-Za-z0-9_-]+")

# How the release starts its executable, as ViD_CommandLine (00500AA0) and gameChecks (00409970) read the command line:
#   "<bigfile>"      the bigfile to open (BIG::b_Open 006BCC90; without one: "You must specify a bigfile to load !")
#   /binload/fe      loading mode DAT_00a723d8 = 3 (/binload/no 0, ed 1, de 2, fe 3), copied to ___Enable_Loading:
#                    binary loading, in which BIG_ChunkDecompress (006CF0E0) maps a world key to its per-world bins
#   /lang/<xx>       the two letters replace the default "fr" at 009C272C; gameChecks passes them to
#                    TXT::SetCurrentLang (004FCBD0), a case-insensitive lookup in TXT_LangShortName (009C2580:
#                    fr en da nl fi de it es pt sv pl ru ja ...; language.LANG_SHORT_NAMES).  The release's launcher
#                    (Launcher.exe, ApplicationLauncher.GetCommandLine) writes "/binload/fe /lang/<xx> [/fullscreen]
#                    [/vsync] /res<mode> /versionIndex:<n>" with xx one of en fr de it es nl.
# The release's launcher also wrote /versionindex:<n> (a switch ViD_CommandLine recognises; /pcversionDVD stores 5 in
# DAT_00a723b0, /pcversionCD<n> stores n).  It is not passed here: with it the game calls itself "Rabbids Go Home -
# DVD", the name of the disc release it came off, and this is the whole game.  A folder from an older setup has it in
# its switches; it is dropped when they are stored again (DROPPED_SWITCHES), and the game launcher drops it too.
LAUNCH_SWITCHES = ("/binload/fe", "/lang/{lang}")
DROPPED_SWITCHES = ("/versionindex",)
# launcher language -> TXT_LangShortName index: the release launcher's six, and Japanese for the Japanese disc.
# ViD::LoadTexts (004EBAA0) loads a world's texts only when the world's FCF language index has an entry for the current
# language (there is no fallback); a slot the Wii disc has no texts for shows English: see language_coverage and the
# conversion summary's "languages".
LANGUAGES = {"en": 1, "fr": 0, "de": 5, "it": 6, "es": 7, "nl": 3, "ja": 12}
LANGUAGE_NAMES = {"en": "English", "fr": "French", "de": "German", "it": "Italian", "es": "Spanish", "nl": "Dutch",
                  "ja": "Japanese"}


# ---------------------------------------------------------------------------------------------------------------- files
def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def code_sha256(path: str) -> str | None:
    """The SHA-256 of a PE file's sections other than .rsrc (raw data, in header order); None when the file is not a PE
    file."""
    with open(path, "rb") as f:
        d = f.read()
    try:
        if d[:2] != b"MZ":
            return None
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            return None
        nsec, = struct.unpack_from("<H", d, pe + 6)
        opt_size, = struct.unpack_from("<H", d, pe + 20)
        h = hashlib.sha256()
        off = pe + 24 + opt_size
        for i in range(nsec):
            name, _vsize, _va, raw_size, raw = struct.unpack_from("<8sIIII", d, off + i * 40)
            if name.rstrip(b"\0") != b".rsrc":
                h.update(d[raw:raw + raw_size])
        return h.hexdigest().upper()
    except struct.error:
        return None


def identify_executable(path: str) -> tuple[str, str | None]:
    """(the file's SHA-256, the release build it is - None for a build the platform DLL was not written for): by the
    file hash, else by the code hash (a copy with another icon)."""
    digest = sha256_file(path)
    what = SUPPORTED_EXECUTABLES.get(digest)
    if what is None:
        code = code_sha256(path)
        what = SUPPORTED_CODE.get(code) if code else None
    return digest, what


def same_content(a: str, b: str, chunk: int = 1 << 20) -> bool:
    """True when both files have the same size and the same bytes.  Compared chunk by chunk: as strong as comparing
    content hashes, and it stops at the first difference instead of reading both files to the end."""
    if os.path.getsize(a) != os.path.getsize(b):
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x = fa.read(chunk)
            if x != fb.read(chunk):
                return False
            if not x:
                return True


def copy_file(src: str, dst: str) -> None:
    """Copy through "<dst>.part", renamed when complete: an interrupted copy never leaves a file under the final name."""
    part = dst + ".part"
    try:
        shutil.copyfile(src, part)
        os.replace(part, dst)
    finally:
        if os.path.exists(part):
            os.remove(part)


def _copy_if_changed(src: str, dst: str) -> str:
    if os.path.exists(dst) and same_content(src, dst):
        return "present"
    copy_file(src, dst)
    return "copied"


def _copy_tree(src: str, dst: str) -> tuple[int, list[str]]:
    """Copy the files of `src` into `dst` (folders created as needed) where they differ; (copied, [files in use])."""
    copied, locked = 0, []
    for folder, _dirs, files in os.walk(src):
        rel = os.path.relpath(folder, src)
        target = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(target, exist_ok=True)
        for fn in files:
            a, b = os.path.join(folder, fn), os.path.join(target, fn)
            if os.path.exists(b) and same_content(a, b):
                continue
            try:
                copy_file(a, b)
                copied += 1
            except OSError:
                locked.append(os.path.join(rel, fn) if rel != "." else fn)
    return copied, locked


def repository_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# ------------------------------------------------------------------------------------------------------------- bigfiles
def is_main_bigfile_name(fn: str) -> bool:
    """A bigfile name without a branch part (not "<base>.<branch>.<ext>.bf")."""
    return fn.lower().endswith(".bf") and "." not in fn[:-3]


def find_main_bigfile(folder: str) -> str:
    """The main bigfile of a game folder: a bigfile whose name has no branch part; the largest when there are several."""
    cands = []
    for fn in os.listdir(folder):
        p = os.path.join(folder, fn)
        if is_main_bigfile_name(fn) and os.path.isfile(p) and is_bigfile(p):
            cands.append(p)
    if not cands:
        raise FileNotFoundError("no bigfile in %s" % folder)
    return max(cands, key=os.path.getsize)


def check_bigfile_name(name: str) -> str | None:
    """None when `name` can be the converted bigfile's file name, otherwise the reason it cannot."""
    base, dot, ext = name.partition(".")
    if not dot or ext.lower() != "bf":
        return "the name must be <base>.bf with no other dot (the sibling bigfiles are named <base>.<branch>.<ext>.bf)"
    if not base.startswith(BIGFILE_PREFIX):
        return 'the name must start with "%s" (the executable gives such bigfiles BIG flag 1, as the release\'s has)' % (
            BIGFILE_PREFIX)
    if not _BASE_CHARS.fullmatch(base):
        return "the name may only use letters, digits, _ and - (the launcher passes it to cmd)"
    return None


def sibling_name(bigfile_name: str, branch: str, ext: str) -> str:
    return "%s.%s.%s.bf" % (bigfile_name.split(".")[0], branch, ext)


def find_sibling(main_path: str, branch: str, ext: str) -> str | None:
    """The sibling bigfile "<base>.<branch>.<ext>.bf" of a bigfile (file names compared without case)."""
    folder, name = os.path.split(os.path.abspath(main_path))
    want = sibling_name(name, branch, ext).lower()
    for fn in os.listdir(folder):
        if fn.lower() == want:
            return os.path.join(folder, fn)
    return None


def copy_siblings(wii_main: str, out: str, log=print) -> dict:
    """Copy the Wii sibling bigfiles next to the converted bigfile `out`, under its base name.  A sibling already there
    is kept only when it has the Wii file's size and bytes (same_content); anything else is copied again.  Returns
    {sibling name: "copied" | "present" | "missing in the Wii folder"}."""
    res = {}
    folder, name = os.path.split(os.path.abspath(out))
    for branch, ext in SIBLINGS:
        dst = os.path.join(folder, sibling_name(name, branch, ext))
        src = find_sibling(wii_main, branch, ext)
        sib = os.path.basename(dst)
        if src is None:
            res[sib] = "missing in the Wii folder"
            continue
        if os.path.exists(dst):
            if log:
                log("comparing %s with the Wii file" % sib)
            if same_content(src, dst):
                res[sib] = "present"
                continue
        if log:
            log("copying %s" % sib)
        copy_file(src, dst)
        res[sib] = "copied"
    return res


def bigfile_state(path: str) -> str | None:
    """None when the file table of a bigfile reads and its last entry ends inside the file, otherwise what is wrong."""
    try:
        big = Big(path)
    except (OSError, ValueError) as ex:
        return "cannot read its file table (%s)" % ex
    try:
        entries = big.entries()
        if not entries:
            return "it has no entries"
        last = max(entries, key=lambda e: e.pos)
        length, _user, refs, _flags = big.file_header(last)
        end = last.pos + FILE_HDR + length + refs
        if end > big.size:
            return "truncated: its last entry ends at byte %d, the file has %d bytes" % (end, big.size)
        return None
    finally:
        big.close()


def language_coverage(bigfile: str) -> tuple[int, collections.Counter]:
    """(FCF language indexes that list at least one language, {language: indexes listing it}) of a bigfile."""
    from . import language
    big = Big(bigfile)
    try:
        listed = 0
        langs = collections.Counter()
        for e in big.entries():
            if e.ext == "bin" and e.key >> 20 == 0xFCF:
                users = {r.user for r in language.parse_fcf(big.read(e))}
                if users:
                    listed += 1
                    langs.update(users)
        return listed, langs
    finally:
        big.close()


def port_languages(bigfile: str) -> dict[str, int] | None:
    """{short name: language index} of the languages a converted bigfile has its own texts for, from the conversion
    summary <bigfile>.json ("languages", written since the per-language conversion); None for a bigfile without
    one - an older conversion, whose slots all show English."""
    summary = _read_json(bigfile + ".json")
    langs = summary.get("languages") if isinstance(summary, dict) else None
    if not isinstance(langs, dict):
        return None
    return {str(k): int(v) for k, v in langs.items() if isinstance(v, int)}


def _language_warning(bigfile: str, lang: str) -> str | None:
    name = os.path.basename(bigfile)
    listed, langs = language_coverage(bigfile)
    if listed and not langs.get(LANGUAGES[lang]):
        return ("the launcher starts in %s (%s, language %d), but none of the %d language indexes of %s lists that "
                "language: ViD::LoadTexts (004EBAA0) then loads no texts at all.  Choose a language of the conversion"
                % (lang, LANGUAGE_NAMES.get(lang, lang), LANGUAGES[lang], listed, name))
    own = port_languages(bigfile)
    if own is not None and lang not in own:
        return ("the launcher starts in %s (%s), but the Wii disc has no %s texts (it has %s): the game shows English"
                % (lang, LANGUAGE_NAMES.get(lang, lang), LANGUAGE_NAMES.get(lang, lang),
                   ", ".join(LANGUAGE_NAMES.get(k, k) for k in own)))
    return None


# ----------------------------------------------------------------------------------------------------------- executable
def game_executable(folder: str) -> str | None:
    """The game's program in a folder: GAME_EXE, else the largest .exe file whose name is not a launcher's, a setup's
    or an uninstaller's (the launcher window lives in a subfolder, so it is not in the way); None for none."""
    best, best_size = None, -1
    for fn in sorted(os.listdir(folder)):
        p = os.path.join(folder, fn)
        low = fn.lower()
        if not low.endswith(".exe") or not os.path.isfile(p):
            continue
        if low == GAME_EXE.lower():
            return p
        if any(word in low for word in _NOT_GAME):
            continue
        size = os.path.getsize(p)
        if size > best_size:
            best, best_size = p, size
    return best


def find_executable(pc_dir: str, pc_exe: str | None = None) -> str | None:
    """The game's executable: `pc_exe` when given, else game_executable(pc_dir); None for none.  Its hash is reported
    (check_executable), never required."""
    exe = pc_exe if pc_exe else game_executable(pc_dir)
    return exe if exe is not None and os.path.isfile(exe) else None


def check_executable(pc_dir: str, pc_exe: str | None = None) -> dict:
    """{"file", "sha256", "build"} of the game's executable - "build" names the release build the hash is, None for a
    build the platform DLL was not written for - and "known", the hashes of the builds it was written for; without
    "file" when the folder has no program."""
    exe = find_executable(pc_dir, pc_exe)
    rep = {"known": sorted(SUPPORTED_EXECUTABLES)}
    if exe is not None:
        digest, what = identify_executable(exe)
        rep["file"] = exe
        rep["sha256"] = digest
        rep["build"] = what
    return rep


# ------------------------------------------------------------------------------------------------------------- launcher
def extra_switches(switches) -> list[str]:
    """The switches of a launch line that rghport does not write itself (a user's own, e.g. /fps): they are kept when
    the line is stored again."""
    own = tuple(s.split("{")[0].rstrip(":0123456789").lower() for s in LAUNCH_SWITCHES) + DROPPED_SWITCHES
    return [s for s in switches if s.startswith("/") and not s.lower().startswith(own)]


def launch_switches(lang: str = "en", extra=()) -> list[str]:
    """The game's command line switches: the release's (LAUNCH_SWITCHES) with `lang`, then a user's own."""
    return [s.format(lang=lang) for s in LAUNCH_SWITCHES] + list(extra)


def read_ini(path: str) -> list[str]:
    """The lines of an ini file (bytes kept as they are), [] when there is none."""
    try:
        with open(path, encoding="latin-1") as f:
            return f.read().splitlines()
    except OSError:
        return []


def _ini_section(line: str) -> str | None:
    s = line.strip()
    return s[1:-1].strip().lower() if s.startswith("[") and s.endswith("]") else None


def _ini_key(line: str) -> str | None:
    s = line.strip()
    return s.split("=", 1)[0].strip().lower() if "=" in s and not s.startswith((";", "#")) else None


def ini_get(lines: list[str], section: str, key: str) -> str | None:
    """The value of `key` in `[section]`; None when there is no such line."""
    cur = None
    for ln in lines:
        sec = _ini_section(ln)
        if sec is not None:
            cur = sec
        elif cur == section.lower() and _ini_key(ln) == key.lower():
            return ln.split("=", 1)[1].strip()
    return None


def ini_set(lines: list[str], section: str, key: str, value: str) -> None:
    """Set `key` in `[section]`, keeping every other line as it is: the line is replaced where it stands, else added
    after the section's last line, else the section is added at the end."""
    cur = None
    end = None                           # the index after the section's last line with something on it
    for i, ln in enumerate(lines):
        sec = _ini_section(ln)
        if sec is not None:
            if cur == section.lower():
                break
            cur = sec
            if cur == section.lower():
                end = i + 1
        elif cur == section.lower():
            if _ini_key(ln) == key.lower():
                lines[i] = "%s=%s" % (key, value)
                return
            if ln.strip():
                end = i + 1
    if end is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines += ["[%s]" % section, "%s=%s" % (key, value)]
    else:
        lines.insert(end, "%s=%s" % (key, value))


def write_ini(path: str, lines: list[str]) -> None:
    with open(path, "w", encoding="latin-1", newline="\r\n") as f:     # as the launcher writes it
        f.write("\n".join(lines) + "\n")


def read_launch(out_dir: str) -> dict | None:
    """{"switches", "lang"} of the launch line stored in the port folder's options.ini; None when it has none."""
    text = ini_get(read_ini(os.path.join(out_dir, OPTIONS_INI)), "launch", "switches")
    if text is None:
        return None
    switches = text.split()
    lang = next((s[len("/lang/"):] for s in switches if s.lower().startswith("/lang/")), None)
    return {"switches": switches, "lang": lang}


def write_launch(out_dir: str, lang: str | None) -> dict:
    """Store the game's command line in options.ini ([launch] switches=): the release's switches with `lang`, then the
    switches a user added to the line that is there.  `lang` None keeps the language that line has (en without one).
    Returns {"switches", "lang", "result": "written" | "kept"}."""
    path = os.path.join(out_dir, OPTIONS_INI)
    lines = read_ini(path)
    old = read_launch(out_dir)
    if lang is None:
        lang = old["lang"].lower() if old and old.get("lang") and old["lang"].lower() in LANGUAGES else "en"
    switches = launch_switches(lang, extra_switches(old["switches"]) if old else ())
    if old and [s.lower() for s in old["switches"]] == [s.lower() for s in switches]:
        return {"switches": " ".join(old["switches"]), "lang": lang, "result": "kept"}
    if not lines:
        lines = ["; %s - the game's settings (the launcher and the Options screen of the pause menu write it)."
                 % OPTIONS_INI]
    ini_set(lines, "launch", "switches", " ".join(switches))
    write_ini(path, lines)
    return {"switches": " ".join(switches), "lang": lang, "result": "written"}


def _launch(out_dir: str, bigfile_name: str | None, lang: str | None, problems: list, warnings: list) -> dict:
    """The launch line of the port folder (write_launch); a play.cmd an earlier setup wrote there is removed, the
    launcher starts the game without it."""
    res = {"file": OPTIONS_INI}
    old = os.path.join(out_dir, OLD_LAUNCH_FILE)
    if os.path.isfile(old):
        try:
            with open(old, encoding="latin-1") as f:
                ours = "rghport" in f.read()
        except OSError:
            ours = False
        if not ours:
            warnings.append("%s is not a file of the setup's, left alone; the launcher starts the game without it"
                            % OLD_LAUNCH_FILE)
        else:
            try:
                os.remove(old)
                res["removed"] = OLD_LAUNCH_FILE
            except OSError as ex:
                warnings.append("%s (a batch file an earlier setup wrote) could not be removed: %s"
                                % (OLD_LAUNCH_FILE, ex))
    exe = find_executable(out_dir)
    if bigfile_name is None:
        try:
            bigfile_name = os.path.basename(find_main_bigfile(out_dir))
        except FileNotFoundError:
            pass
    if exe is None:
        why = "the port folder has no executable (the game's program)"
    elif bigfile_name is None or not os.path.isfile(os.path.join(out_dir, bigfile_name)):
        why = "the port folder has no converted bigfile" + (" " + bigfile_name if bigfile_name else "")
    else:
        why = check_bigfile_name(bigfile_name)
    if why:
        res["result"] = "not stored"
        problems.append("launch line not stored: " + why)
        return res
    try:
        res.update(write_launch(out_dir, lang))
    except OSError as ex:
        res["result"] = "not stored"
        problems.append("%s could not be written: %s" % (OPTIONS_INI, ex))
        return res
    warning = _language_warning(os.path.join(out_dir, bigfile_name), res["lang"])
    if warning:
        warnings.append(warning)
    return res


# ------------------------------------------------------------------------------------------------------------- assemble
def assemble(pc_dir: str, out_dir: str, pc_exe: str | None = None, platform_dir: str | None = None, icon: str | None = None,
             bigfile_name: str | None = None, lang: str | None = "en", log=print) -> dict:
    if icon is None:
        icon = DEFAULT_ICON if os.path.isfile(DEFAULT_ICON) else None      # the moon (rghport/data/moon.ico)
    elif icon.lower() == "none":
        icon = None                                                        # the release's own icon
    """Copy the executable, shaders, video library and platform files into `out_dir`, create the save folder and store
    the launch line (the switches the launcher starts the game with, in options.ini; `bigfile_name` defaults to the
    port folder's converted bigfile; `lang` None keeps the language the folder has).  Returns {"executable",
    "shaders", "video", "platform", "sav", "launch", "problems": [...], "warnings": [...]}; the port folder is
    complete when there are no problems."""
    os.makedirs(out_dir, exist_ok=True)
    problems = []
    warnings = []
    rep = {}
    exe = check_executable(pc_dir, pc_exe)
    if "file" in exe:
        name = os.path.basename(exe["file"])
        dst = os.path.join(out_dir, name)
        exe["result"] = _copy_if_changed(exe["file"], dst)
        exe["file"] = name
        if exe["build"] is None:
            warnings.append("%s is not the release build the platform DLL was written for (SHA-256 %s): the DLL patches "
                            "only what it recognises" % (name, exe["sha256"]))
        if icon:
            from . import peicon
            try:
                exe["icon"] = peicon.set_icon(dst, icon)
                exe["icon"]["file"] = os.path.basename(icon)
            except (OSError, peicon.IconError) as ex:
                exe["icon"] = "not set"
                warnings.append("the executable keeps its own icon: %s: %s" % (icon, ex))
    else:
        exe["result"] = "not copied"
        problems.append("no executable in %s: the game's program (%s) is missing; --pc-exe names another file to copy"
                        % (pc_dir, GAME_EXE))
    rep["executable"] = exe
    src = os.path.join(pc_dir, SHADERS_DIR)
    if os.path.isdir(src):
        shutil.copytree(src, os.path.join(out_dir, SHADERS_DIR), dirs_exist_ok=True)
        wii = patch_shaders(os.path.join(out_dir, SHADERS_DIR))
        rep["shaders"] = "copied, %d of %d Wii lighting and glow changes applied" % (
            len(wii["applied"]) + len(wii["already"]), len(SHADER_PATCHES))
        if wii["differs"]:
            warnings.append("shaders differ from the PC release, left as they are: " + "; ".join(wii["differs"]))
    else:
        rep["shaders"] = "missing"
        problems.append("the PC folder has no %s folder" % SHADERS_DIR)
    src = next((os.path.join(pc_dir, fn) for fn in os.listdir(pc_dir) if fn.lower() == VIDEO_LIBRARY), None)
    if src is not None:
        rep["video"] = _copy_if_changed(src, os.path.join(out_dir, VIDEO_LIBRARY))
    else:
        rep["video"] = "missing"
        problems.append("the PC folder has no %s" % VIDEO_LIBRARY)
    platform_dir = platform_dir or os.path.join(repository_root(), "platform")
    dll = os.path.join(platform_dir, "build", PLATFORM_DLL)
    plat = {}
    if os.path.isfile(dll):
        plat["dll"] = _copy_if_changed(dll, os.path.join(out_dir, PLATFORM_DLL))
    else:
        plat["dll"] = "not built"
        problems.append("platform DLL not built (%s missing): run platform/build.bat" % os.path.join("platform", "build",
                                                                                                      PLATFORM_DLL))
    build = os.path.join(platform_dir, "build", LAUNCHER_BUILD)
    stub = os.path.join(build, LAUNCHER_EXE)
    if os.path.isfile(stub) and os.path.isdir(os.path.join(build, LAUNCHER_DIR)):
        dst = os.path.join(out_dir, LAUNCHER_EXE)
        plat["launcher"] = _copy_if_changed(stub, dst)
        copied, locked = _copy_tree(os.path.join(build, LAUNCHER_DIR), os.path.join(out_dir, LAUNCHER_DIR))
        plat["launcher"] += ", %d runtime files copied" % copied
        if locked:
            warnings.append("%d launcher file(s) in use, not updated (close the launcher and run assemble again): %s"
                            % (len(locked), ", ".join(locked[:5])))
        for old in OLD_LAUNCHERS:              # a stub under an earlier name (a small file of ours), no longer wanted
            p = os.path.join(out_dir, old)
            if os.path.isfile(p) and os.path.getsize(p) < 1 << 20:
                try:
                    os.remove(p)
                    plat["launcher"] += ", old %s removed" % old
                except OSError:
                    pass
    else:
        plat["launcher"] = "not built"
        warnings.append("launcher not built (%s missing): run build_launcher.bat" % os.path.join("platform", "build",
                                                                                                  LAUNCHER_BUILD))
    ini_dst = os.path.join(out_dir, PLATFORM_INI)
    ini_src = next((p for p in (os.path.join(platform_dir, "build", PLATFORM_INI), os.path.join(platform_dir, PLATFORM_INI))
                    if os.path.isfile(p)), None)
    if os.path.exists(ini_dst):
        plat["ini"] = "kept"
    elif ini_src is not None:
        shutil.copyfile(ini_src, ini_dst)
        plat["ini"] = "copied"
    else:
        plat["ini"] = "missing"
        problems.append("no %s template in platform/" % PLATFORM_INI)
    rep["platform"] = plat
    os.makedirs(os.path.join(out_dir, SAV_DIR), exist_ok=True)
    rep["sav"] = SAV_DIR
    rep["launch"] = _launch(out_dir, bigfile_name, lang, problems, warnings)
    rep["problems"] = problems
    rep["warnings"] = warnings
    if log:
        for p in problems:
            log("port folder: " + p)
        for w in warnings:
            log("port folder: warning: " + w)
    return rep


# ---------------------------------------------------------------------------------------------------------------- check
def _read_json(path: str):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def check_port(out_dir: str, platform_dir: str | None = None) -> dict:
    """Verify a port folder: the game's executable (its SHA-256 reported), the shaders folder, the video library, the
    platform DLL and its
    configuration, one converted bigfile with both siblings named after it, the save folder and the launcher.  Returns
    {"ok": [...], "warnings": [...], "problems": [...]}; the port folder is complete when there are no problems."""
    ok, warnings, problems = [], [], []
    files = {fn.lower(): fn for fn in os.listdir(out_dir)}

    exe = find_executable(out_dir)
    if exe is not None:
        digest, what = identify_executable(exe)
        if what is not None:
            ok.append("executable %s: SHA-256 %s, %s" % (os.path.basename(exe), digest, what))
        else:
            ok.append("executable %s: SHA-256 %s" % (os.path.basename(exe), digest))
            warnings.append("%s is not the release build the platform DLL was written for (known: %s): the DLL patches "
                            "only what it recognises" % (os.path.basename(exe), ", ".join(sorted(SUPPORTED_EXECUTABLES))))
    else:
        problems.append("no executable (the game's program, %s): rghport assemble copies it from the PC folder" % GAME_EXE)

    shaders = os.path.join(out_dir, SHADERS_DIR)
    n = sum(len(fs) for _d, _s, fs in os.walk(shaders)) if os.path.isdir(shaders) else 0
    if n:
        ok.append("%s folder: %d files" % (SHADERS_DIR, n))
        wii = shader_state(shaders)
        if wii["applied"] or wii["differs"]:
            warnings.append("%s folder: %d of %d Wii lighting and glow changes present (rghport assemble applies them)%s"
                            % (SHADERS_DIR, len(wii["already"]), len(SHADER_PATCHES),
                               "; differs from the PC release: " + "; ".join(wii["differs"]) if wii["differs"] else ""))
        else:
            ok.append("%s folder: the %d Wii lighting and glow changes" % (SHADERS_DIR, len(SHADER_PATCHES)))
    elif os.path.isdir(shaders):
        problems.append("the %s folder is empty" % SHADERS_DIR)
    else:
        problems.append("no %s folder (the executable loads its effects from it)" % SHADERS_DIR)

    if VIDEO_LIBRARY in files:
        ok.append("video library %s" % files[VIDEO_LIBRARY])
    else:
        problems.append("no %s (the video library the executable imports)" % VIDEO_LIBRARY)

    if PLATFORM_DLL in files:
        ok.append("platform DLL %s" % files[PLATFORM_DLL])
        built = os.path.join(platform_dir or os.path.join(repository_root(), "platform"), "build", PLATFORM_DLL)
        if os.path.isfile(built) and not same_content(built, os.path.join(out_dir, files[PLATFORM_DLL])):
            warnings.append("%s differs from the build in platform/build (rghport assemble copies the current one)"
                            % PLATFORM_DLL)
    else:
        problems.append("no %s (the platform DLL: Wii remote and saves; rghport assemble copies it)" % PLATFORM_DLL)
    if LAUNCHER_EXE.lower() in files and os.path.isfile(os.path.join(out_dir, LAUNCHER_DIR, "RGHLauncher.exe")):
        ok.append("launcher %s and its %s folder" % (files[LAUNCHER_EXE.lower()], LAUNCHER_DIR))
    else:
        warnings.append("no %s with its %s folder (the launcher window; rghport assemble copies them)"
                        % (LAUNCHER_EXE, LAUNCHER_DIR))
    if PLATFORM_INI in files:
        ok.append("platform configuration %s" % files[PLATFORM_INI])
    else:
        problems.append("no %s (the platform DLL's configuration; rghport assemble copies the template)" % PLATFORM_INI)

    launch = read_launch(out_dir)
    mains = sorted(fn for fn in os.listdir(out_dir)
                   if is_main_bigfile_name(fn) and is_bigfile(os.path.join(out_dir, fn)))
    main = None
    if not mains:
        problems.append("no converted bigfile (<base>.bf)")
    elif len(mains) == 1:
        main = mains[0]
    else:
        problems.append("several converted bigfiles (%s): keep the one the launcher should start" % ", ".join(mains))
    main_ok = False
    if main:
        why = check_bigfile_name(main)
        state = bigfile_state(os.path.join(out_dir, main))
        summary = _read_json(os.path.join(out_dir, main + ".json"))
        if why:
            problems.append("%s: %s" % (main, why))
        if state:
            problems.append("%s: %s" % (main, state))
        if summary and summary.get("test_run"):
            problems.append("%s is a test run (--prefix / --only / --limit): not a complete bigfile" % main)
        elif summary and summary.get("errors"):
            warnings.append("%s: %d entries were passed through after a conversion error (listed in %s.json)"
                            % (main, len(summary["errors"]), main))
        main_ok = not (why or state or (summary and summary.get("test_run")))
        if main_ok:
            ok.append("converted bigfile %s: %d bytes" % (main, os.path.getsize(os.path.join(out_dir, main))))
            own = port_languages(os.path.join(out_dir, main))
            if own:
                ok.append("languages with their own texts: %s" % ", ".join("%s (%s)" % (LANGUAGE_NAMES.get(k, k), k)
                                                                             for k in own))
            elif summary:
                warnings.append("%s was converted before languages were kept apart: every language shows English "
                                "texts (convert again for the disc's other languages)" % main)
        for branch, ext in SIBLINGS:
            want = sibling_name(main, branch, ext)
            real = files.get(want.lower())
            what = "the Wii streamed sounds" if ext == "sns" else "the Wii videos"
            if real is None:
                problems.append("no %s (%s, named after %s)" % (want, what, main))
                continue
            state = bigfile_state(os.path.join(out_dir, real)) if is_bigfile(os.path.join(out_dir, real)) \
                else "not a bigfile"
            if state:
                problems.append("%s: %s" % (real, state))
            else:
                ok.append("sibling %s (%s): %d bytes" % (real, what, os.path.getsize(os.path.join(out_dir, real))))

    sav = os.path.join(out_dir, SAV_DIR)
    if os.path.isdir(sav):
        saves = [fn for fn in os.listdir(sav) if fn.lower().endswith(".sav")]
        ok.append("%s folder: %s" % (SAV_DIR, "%d save files" % len(saves) if saves else
                                     "no save file yet (the game starts with its first boot)"))
    else:
        problems.append("no %s folder (the platform DLL keeps the saves there)" % SAV_DIR)

    if OLD_LAUNCH_FILE in files:
        warnings.append("%s is left over from an earlier setup (rghport assemble removes it; the launcher starts the "
                        "game without it)" % files[OLD_LAUNCH_FILE])
    if launch is None:
        ok.append("launch line: none stored in %s yet; the launcher starts the game with %s"
                  % (OPTIONS_INI, " ".join(launch_switches())))
    else:
        low = [s.lower() for s in launch["switches"]]
        lacking = []
        for s in LAUNCH_SWITCHES:
            if s.startswith("/lang/"):
                if launch["lang"] is None:
                    lacking.append("/lang/<xx>")
            elif s.lower() not in low:
                lacking.append(s)
        if lacking:
            problems.append("%s [launch] switches (%s) lack %s" % (OPTIONS_INI, " ".join(launch["switches"]),
                                                                    " ".join(lacking)))
        elif launch["lang"].lower() not in LANGUAGES:
            warnings.append("the launch line starts the game in language %s, which the tool does not write"
                            % launch["lang"])
        else:
            if main_ok:
                warning = _language_warning(os.path.join(out_dir, main), launch["lang"].lower())
                if warning:
                    warnings.append(warning)
            ok.append("launch line (%s [launch]): %s" % (OPTIONS_INI, " ".join(launch["switches"])))
    return {"ok": ok, "warnings": warnings, "problems": problems}
