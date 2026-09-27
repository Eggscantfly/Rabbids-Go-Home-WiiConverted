"""Folder checks for the setup: what a Wii game data folder and a PC release folder hold, in words.

    rghport inspect [--wii DIR] [--pc DIR]      -> JSON on stdout (report)

The setup is the launcher window (launcher/, C++) in setup mode; it runs this command in the background for the two
folders whose checks need the archive reader and the executable's hash, and reads the JSON.  No Qt here.
"""
from __future__ import annotations

import os


def size_text(n: float) -> str:
    for unit in ("bytes", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return ("%d %s" % (n, unit)) if unit == "bytes" else ("%.1f %s" % (n, unit))
        n /= 1024.0
    return "%d" % n


def wii_languages(main: str) -> list[str]:
    """The /lang codes of the languages a Wii archive has texts for (its text indexes), in the launcher's order;
    empty when the archive cannot be read."""
    from .archive.bigfile import Big
    from .convert.language import archive_languages, language_names
    from .convert.port import LANGUAGES
    try:
        big = Big(main)
        try:
            names = language_names(archive_languages(big))
        finally:
            big.close()
    except Exception:          # noqa: BLE001 - a damaged archive: the conversion reports it
        return []
    return [code for code in LANGUAGES if code in names]


def inspect_wii(folder: str) -> tuple[str, str, list[str]]:
    """(state, text, the disc's languages) of a Wii game data folder: state "ok", "warn", "bad" or "" (nothing
    picked)."""
    from .convert.port import LANGUAGE_NAMES, SIBLINGS, find_main_bigfile, find_sibling
    if not folder:
        return "", "The folder with RGH.BF, RGH.wii.sns.BF and RGH.$hd$.bik.BF from your Wii disc.", []
    if not os.path.isdir(folder):
        return "bad", "This is not a folder.", []
    try:
        main = find_main_bigfile(folder)
    except (OSError, ValueError):
        return "bad", "No game archive (RGH.BF) in this folder.", []
    found = ["%s (%s)" % (os.path.basename(main), size_text(os.path.getsize(main)))]
    missing = []
    for (branch, ext), what in zip(SIBLINGS, ("sounds", "videos")):
        if find_sibling(main, branch, ext):
            found.append(what)
        else:
            missing.append(what)
    langs = wii_languages(main)
    texts = " Texts in %s." % ", ".join(LANGUAGE_NAMES.get(c, c) for c in langs) if langs else ""
    if missing:
        return "warn", "Found %s, but the %s archive is missing: the game needs it.%s" % (
            ", ".join(found), " and the ".join(missing), texts), langs
    return "ok", "Found " + ", ".join(found) + "." + texts, langs


def inspect_pc(folder: str) -> tuple[str, str]:
    """(state, text) of a PC release folder: its archive, shaders, video library and the game's executable."""
    from .convert.port import GAME_EXE, check_executable, find_main_bigfile
    if not folder:
        return "", "The folder of the PC release: its executable, shaders folder, binkw32.dll and archive."
    if not os.path.isdir(folder):
        return "bad", "This is not a folder."
    try:
        main = find_main_bigfile(folder)
    except (OSError, ValueError):
        return "bad", "No game archive (.bf) in this folder."
    missing = [n for n in ("shaders", "binkw32.dll") if not os.path.exists(os.path.join(folder, n))]
    if missing:
        return "bad", "This folder has no %s." % " and no ".join(missing)
    try:
        rep = check_executable(folder)
    except OSError as ex:
        return "bad", "Cannot read the folder: %s" % ex
    base = "Found %s (%s)" % (os.path.basename(main), size_text(os.path.getsize(main)))
    if "file" not in rep:
        return "warn", "%s, but no program: the game folder would have no executable (%s)." % (base, GAME_EXE)
    name = os.path.basename(rep["file"])
    if rep["build"] is None:
        return "warn", "%s and %s, a build the platform DLL was not written for: it may run unpatched." % (base, name)
    return "ok", "%s and %s." % (base, name)


def report(wii: str | None, pc: str | None) -> dict:
    """The JSON of `rghport inspect`: "wii" and "pc" for the folders given, and "languages", every language the tool
    writes with its name, in the launcher's order."""
    from .convert.port import LANGUAGE_NAMES, LANGUAGES
    rep: dict = {"languages": [[code, LANGUAGE_NAMES.get(code, code)] for code in LANGUAGES]}
    if wii is not None:
        state, text, langs = inspect_wii(wii)
        rep["wii"] = {"state": state, "text": text, "languages": langs}
    if pc is not None:
        state, text = inspect_pc(pc)
        rep["pc"] = {"state": state, "text": text}
    return rep
