"""Command line of the save import.

    rghport import-save --wii-save <folder> --out <folder> --bigfile <file> [--slot-size T=N ...] [--dump]
                        [--no-check] [--lenient]

`register(subparsers)` adds the `import-save` command to the rghport command line; `main()` runs it on its own
(`python -m rghport.saves import-save ...`).
"""
from __future__ import annotations

import argparse
import sys

from . import wii_import

DESCRIPTION = """Import a Rabbids Go Home Wii save into the port's PC save format.

--wii-save  the game's save data folder copied from a Wii or an emulator NAND (index.dat, slt_<slot>_<structure>.sav,
            slt_xx_<structure>.sav; banner.bin is not needed).  It is only read.
--out       the folder that receives the PC save files (the port's "sav" folder next to the PC executable).
--bigfile   the Wii bigfile or a converted one: its universe script model (entry 72002B9C) gives the type of every
            saved variable.
After writing, the files are read back the way the PC executable reads them and compared with the Wii source."""


def add_arguments(p: argparse.ArgumentParser) -> None:
    p.add_argument("--wii-save", required=True, metavar="DIR", help="Wii save data folder (read only)")
    p.add_argument("--out", required=True, metavar="DIR", help="output sav folder")
    p.add_argument("--bigfile", required=True, metavar="FILE", help="Wii or converted bigfile (universe model)")
    p.add_argument("--slot-size", action="append", default=[], metavar="T=N",
                   help="PC slot size S_T of a merged structure (default: the Wii one)")
    p.add_argument("--dump", action="store_true", help="print the decoded index and saved values")
    p.add_argument("--no-check", action="store_true", help="skip the read-back comparison")
    p.add_argument("--lenient", action="store_true", help="swap entries without a known type as 4-byte units")


def run(args: argparse.Namespace) -> int:
    slot_sizes = {}
    for item in args.slot_size:
        t, n = item.split("=", 1)
        slot_sizes[int(t, 0)] = int(n, 0)
    return wii_import.import_save(args.wii_save, args.out, args.bigfile, slot_sizes=slot_sizes,
                                  check=not args.no_check, lenient=args.lenient, show_dump=args.dump)


def register(subparsers) -> argparse.ArgumentParser:
    p = subparsers.add_parser("import-save", help="import a Wii save into the PC save format",
                              description=DESCRIPTION, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(p)
    p.set_defaults(func=run)
    return p


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="rghport")
    sub = ap.add_subparsers(dest="command", required=True)
    register(sub)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
