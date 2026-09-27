"""rghport command line.

    rghport convert  --wii DIR --pc DIR --out DIR [--cache DIR] [--pc-exe FILE] [--lang en|fr|es|...] [--import-save DIR]
                     [--name RGH_WC.bf] [--no-options-page] [--script-overrides DIR[;DIR]] [--strict]
                     [--no-siblings] [--twins scripts|none|all] [--prefix FFF,FEF] [--only KEY,KEY] [--limit N]
    rghport assemble --pc DIR --out DIR [--pc-exe FILE] [--name RGH_WC.bf] [--lang en|fr|es|...]
    rghport check    --out DIR
    rghport kinds    --wii DIR [--cache DIR]
    rghport inspect  [--wii DIR] [--pc DIR]
    rghport import-save --wii-save DIR --out DIR --bigfile FILE ...

convert   converts the whole Wii archive of --wii into <out>/<name>, copies the sibling bigfiles next to it, assembles
          the rest of the port folder (see assemble) and, with --import-save, imports a Wii save into <out>/sav.
          Everything derived from the game files (record indexes, record kinds, the skin index) is generated into the
          cache folder and reused while the input bigfiles do not change.  The cache defaults to "<out>.cache" next to
          the port folder; a cache inside the port folder is refused.
assemble  copies the verified executable, the shaders, the video library, the platform DLL and its configuration into
          the port folder, creates the sav folder and stores the launch line in options.ini.
check     verifies a port folder and lists what is missing or wrong.
inspect   describes a Wii game data folder and a PC release folder as JSON, for the setup (the launcher window).

Exit codes: 0 done (the port folder is complete); 1 error; 2 bad or refused option; 3 the port folder is incomplete
(each problem is listed); 4 the port folder was built but the save import failed.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

from .archive.bigfile import Big
from .archive.index import load_records
from .convert.port import LANGUAGES


class CliError(Exception):
    """A bad or refused option: reported without a traceback, exit code 2."""


def _log(msg: str):
    print("%s  %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def _hex_list(text: str) -> list[int]:
    return [int(x, 16) for x in text.split(",") if x.strip()]


def default_script_overrides() -> list[str]:
    """The script_overrides folder next to the packaged converter or the repository, as the folders its layers.txt
    lists (later folders win; wii_records and overrides without a list): the patched script records the game does
    not start without.  [] when there is none."""
    from .convert.port import repository_root
    bases = []
    if getattr(sys, "frozen", False):
        bases.append(os.path.dirname(os.path.abspath(sys.executable)))
    bases.append(repository_root())
    for base in bases:
        folder = os.path.join(base, "script_overrides")
        if not os.path.isdir(folder):
            continue
        names = ["wii_records", "overrides"]
        try:
            with open(os.path.join(folder, "layers.txt"), encoding="utf-8") as f:
                listed = [ln.strip() for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
            if listed:
                names = listed
        except OSError:
            pass
        dirs = [n if os.path.isabs(n) else os.path.join(folder, n) for n in names]
        return [d for d in dirs if os.path.isdir(d)]
    return []


def _dirs(text: str) -> list[str]:
    return [d for d in text.replace(os.pathsep, ";").split(";") if d.strip()]


def _inside(path: str, folder: str) -> bool:
    """True when `path` is `folder` or lies inside it."""
    p, f = os.path.realpath(path), os.path.realpath(folder)
    try:
        return os.path.normcase(os.path.commonpath([p, f])) == os.path.normcase(f)
    except ValueError:                  # different drives
        return False


def default_cache(out: str) -> str:
    """The cache folder next to the port folder: "<out>.cache"."""
    return os.path.normpath(os.path.abspath(out)) + ".cache"


def cache_folder(out: str, cache: str | None) -> str:
    cache = cache or default_cache(out)
    if _inside(cache, out):
        raise CliError("the cache folder %s is inside the port folder %s: give --cache a folder outside it (default %s)"
                       % (cache, out, default_cache(out)))
    return cache


def _check_name(name: str):
    from .convert.port import check_bigfile_name
    why = check_bigfile_name(name)
    if why:
        raise CliError("--name %s: %s" % (name, why))


def _check_pc_exe(path: str | None):
    if path and not os.path.isfile(path):
        raise CliError("--pc-exe %s: no such file" % path)


def _check_icon(path: str | None):
    if path and path.lower() != "none" and not os.path.isfile(path):
        raise CliError("--icon %s: no such file" % path)


def cmd_kinds(args) -> int:
    from .convert.port import find_main_bigfile
    from .walk.kinds import load_kinds
    wii = Big(find_main_bigfile(args.wii))
    cache = args.cache or os.path.join(os.getcwd(), "cache")
    rec = load_records(wii, os.path.join(cache, "wii_records.json"), _log)
    kinds = load_kinds(wii, rec, os.path.join(cache, "kinds.json"), _log)
    _log("%d record kinds in %s" % (len(kinds), os.path.join(cache, "kinds.json")))
    return 0


def cmd_assemble(args) -> int:
    from .convert.port import assemble
    if args.name:
        _check_name(args.name)
    _check_pc_exe(args.pc_exe)
    _check_icon(args.icon)
    rep = assemble(args.pc, args.out, args.pc_exe, bigfile_name=args.name, lang=args.lang, log=_log, icon=args.icon)
    print(json.dumps(rep, indent=1))
    return 3 if rep["problems"] else 0


def cmd_check(args) -> int:
    from .convert.port import check_port
    if not os.path.isdir(args.out):
        print("rghport check: %s is not a folder" % args.out, file=sys.stderr)
        return 1
    rep = check_port(args.out)
    print("port folder %s" % os.path.abspath(args.out))
    for line in rep["ok"]:
        print("  ok       " + line)
    for line in rep["warnings"]:
        print("  warning  " + line)
    for line in rep["problems"]:
        print("  PROBLEM  " + line)
    print("RESULT: %s" % ("COMPLETE" if not rep["problems"] else "INCOMPLETE, %d problem(s)" % len(rep["problems"])))
    return 3 if rep["problems"] else 0


def cmd_inspect(args) -> int:
    from .checks import report
    print(json.dumps(report(args.wii, args.pc), indent=1))
    return 0


def cmd_convert(args) -> int:
    from .convert.build import build_bigfile
    from .convert.builder import load_entry_overrides, load_overrides
    from .convert.context import Context
    from .convert.port import GAME_EXE, SAV_DIR, assemble, check_executable, copy_siblings, find_main_bigfile
    from .walk.kinds import load_kinds

    t0 = time.time()
    _check_name(args.name)
    _check_pc_exe(args.pc_exe)
    _check_icon(args.icon)
    cache = cache_folder(args.out, args.cache)
    import_save = None
    if args.import_save:
        if not os.path.isdir(args.import_save):
            raise CliError("--import-save %s: not a folder" % args.import_save)
        try:
            from .saves import import_save
        except ImportError as ex:
            raise CliError("--import-save: the save import is not available (%s)" % ex) from None
    wii_path = find_main_bigfile(args.wii)
    pc_path = find_main_bigfile(args.pc)
    os.makedirs(cache, exist_ok=True)
    exe = check_executable(args.pc, args.pc_exe)
    if "file" not in exe:
        _log("warning: no executable in %s (the game's program, %s); the bigfile is converted, the executable is left "
             "out of the port folder" % (args.pc, GAME_EXE))
    elif exe["build"] is None:
        _log("warning: %s is not the release build the platform DLL was written for (SHA-256 %s): the DLL patches only "
             "what it recognises" % (os.path.basename(exe["file"]), exe["sha256"]))
    wii = Big(wii_path)
    pc = Big(pc_path)
    _log("Wii bigfile %s, PC bigfile %s, cache %s" % (os.path.basename(wii_path), os.path.basename(pc_path), cache))
    wii_rec = load_records(wii, os.path.join(cache, "wii_records.json"), _log)
    pc_rec = load_records(pc, os.path.join(cache, "pc_records.json"), _log)
    kinds = load_kinds(wii, wii_rec, os.path.join(cache, "kinds.json"), _log)
    ctx = Context(wii, pc, wii_rec, pc_rec, cache, _log, kinds=kinds, options=vars(args))
    dirs = _dirs(args.script_overrides) if args.script_overrides else default_script_overrides()
    if dirs and not args.script_overrides:
        _log("script overrides: %s" % os.path.dirname(dirs[0]))
    elif not dirs:
        _log("warning: no script_overrides folder next to the converter or the repository: the archive is built from "
             "the game's own script records and the game will not start")
    features = set() if args.no_options_page else {"options_page"}
    overrides = load_overrides(dirs, features)
    entry_overrides = load_entry_overrides(dirs)
    if dirs:
        _log("script overrides: %d records, %d entries" % (len(overrides), len(entry_overrides)))
    out_bf = os.path.join(args.out, args.name)
    twin_kinds = [k.strip() for k in (args.twin_kinds or "").split(",") if k.strip()]
    if twin_kinds:
        _log("PC records also taken for kinds: %s" % ", ".join(twin_kinds))
    summary = build_bigfile(ctx, kinds, out_bf, overrides, entry_overrides, twins=args.twins,
                            allow_missing=not args.strict, prefixes=_hex_list(args.prefix), only=_hex_list(args.only),
                            limit=args.limit, log=_log, twin_kinds=twin_kinds,
                            options_page=not args.no_options_page)
    _log("%s: %d entries, builder sources %s, missing %s, errors %d, script hooks %s" % (
        args.name, summary["entries"], summary["builder_sources"], summary["builder_missing"], len(summary["errors"]),
        summary["script_hooks"] or "none"))
    code = 0
    if not summary["test_run"]:
        problems = []
        if not args.no_siblings:
            for name, state in copy_siblings(wii_path, out_bf, _log).items():
                if state not in ("copied", "present"):
                    problems.append("%s: %s" % (name, state))
                    _log("port folder: %s: %s" % (name, state))
        rep = assemble(args.pc, args.out, args.pc_exe, bigfile_name=args.name, lang=args.lang, log=_log, icon=args.icon)
        problems += rep["problems"]
        code = 3 if problems else 0
        _log("port folder %s: %s" % (args.out, "%d problem(s), listed above" % len(problems) if problems else "complete"))
        if import_save is not None:
            rc = import_save(args.import_save, os.path.join(args.out, SAV_DIR), out_bf, log=_log)
            if rc:
                _log("the save import failed (its exit code %d)" % rc)
                code = 4
    _log("done in %.0f s" % (time.time() - t0))
    return code


def build_parser(script_options: bool = False) -> argparse.ArgumentParser:
    """The argument parser; `script_options` adds the scripts phase's convert options (hooks.py add_arguments)."""
    ap = argparse.ArgumentParser(prog="rghport", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    langs = sorted(LANGUAGES)

    p = sub.add_parser("convert", help="build the port folder from the Wii game data and the PC release")
    p.add_argument("--wii", required=True, metavar="DIR", help="Wii game data folder (the main bigfile and its siblings)")
    p.add_argument("--pc", required=True, metavar="DIR", help="PC release folder (executable, shaders, bigfile)")
    p.add_argument("--out", required=True, metavar="DIR", help="port folder to write")
    p.add_argument("--cache", metavar="DIR", help="folder for the data generated from the game files (default "
                                                  "<out>.cache next to the port folder; not inside the port folder)")
    p.add_argument("--pc-exe", metavar="FILE", help="an executable file to copy instead of the PC folder's game program")
    p.add_argument("--lang", choices=langs, default=None, help="language the launcher starts the game in (default: the "
                                                                "one the port folder already has, else en; a language "
                                                                "the Wii disc lacks shows English texts)")
    p.add_argument("--import-save", metavar="DIR", help="Wii save data folder to import into <out>/sav")
    p.add_argument("--icon", metavar="FILE", help="an .ico file for the port folder's executable (default: the moon, "
                                                  "rghport/data/moon.ico; 'none' keeps the release's own icon)")
    p.add_argument("--name", default="RGH_WC.bf",
                   help="file name of the converted bigfile: <base>.bf starting with RGH (default RGH_WC.bf)")
    p.add_argument("--no-options-page", action="store_true",
                   help="leave the game's pause menu as the Wii had it: no OPTIONS entry and no Options page (the "
                        "platform DLL's control bindings, volumes and graphics screen); the script records a layer's "
                        "needs.txt ties to the page are left out too")
    p.add_argument("--script-overrides", metavar="DIR[;DIR]",
                   help="folders of PC record bodies <KEY>.bin that replace built records (later folders win), each "
                        "with an entries/ folder of bigfile entries <KEY>.bin (default: the script_overrides folder "
                        "next to the converter or the repository, in the order of its layers.txt)")
    p.add_argument("--strict", action="store_true", help="stop at a record without a converter instead of passing Wii bytes")
    p.add_argument("--no-siblings", action="store_true", help="do not copy the sibling bigfiles")
    p.add_argument("--twins", choices=("scripts", "none", "all"), default="scripts",
                   help="records taken from the PC release when it holds the key (default: script records)")
    p.add_argument("--twin-kinds", default="", metavar="KIND[,KIND]",
                   help="record kinds also taken from the PC release when it holds the key, e.g. spec:trl (the PC "
                        "release's animation lists instead of the Edge animation bakes)")
    p.add_argument("--prefix", default="", help="test runs: only bins with these key prefixes, e.g. FFF,FEF")
    p.add_argument("--only", default="", help="test runs: only these entry keys")
    p.add_argument("--limit", type=int, default=0, help="test runs: stop after N bins")
    if script_options:
        from .convert.hooks import script_hooks
        hooks = script_hooks()
        if hooks.add_arguments is not None:
            hooks.add_arguments(p)
    p.set_defaults(func=cmd_convert)

    p = sub.add_parser("assemble", help="copy the executable, shaders, video library and platform files into a port "
                                        "folder and store the launch line")
    p.add_argument("--pc", required=True, metavar="DIR", help="PC release folder")
    p.add_argument("--out", required=True, metavar="DIR", help="port folder")
    p.add_argument("--pc-exe", metavar="FILE", help="an executable file to copy instead of the PC folder's game program")
    p.add_argument("--name", metavar="NAME", help="the port folder's converted bigfile (default: the one found there)")
    p.add_argument("--lang", choices=langs, default=None, help="language the launcher starts the game in (default: the "
                                                                "one the port folder has, else en; a language the Wii "
                                                                "disc lacks shows English texts)")
    p.add_argument("--icon", metavar="FILE", help="an .ico file for the port folder's executable (default: the moon, "
                                                  "rghport/data/moon.ico; 'none' keeps the release's own icon)")
    p.set_defaults(func=cmd_assemble)

    p = sub.add_parser("check", help="verify a port folder")
    p.add_argument("--out", required=True, metavar="DIR", help="port folder to verify")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("kinds", help="generate the record kinds of the Wii archive")
    p.add_argument("--wii", required=True, metavar="DIR")
    p.add_argument("--cache", metavar="DIR")
    p.set_defaults(func=cmd_kinds)

    p = sub.add_parser("inspect", help="describe a Wii game data folder and a PC release folder as JSON (the setup)")
    p.add_argument("--wii", metavar="DIR", help="Wii game data folder")
    p.add_argument("--pc", metavar="DIR", help="PC release folder")
    p.set_defaults(func=cmd_inspect)

    try:
        from .saves import cli as saves_cli
    except ImportError:
        saves_cli = None
    if saves_cli is not None:
        saves_cli.register(sub)
    return ap


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    args = build_parser(script_options=argv[:1] == ["convert"]).parse_args(argv)
    try:
        return args.func(args)
    except CliError as ex:
        print("rghport %s: %s" % (args.command, ex), file=sys.stderr)
        return 2
    except (FileNotFoundError, NotADirectoryError) as ex:
        print("rghport %s: error: %s" % (args.command, ex), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
