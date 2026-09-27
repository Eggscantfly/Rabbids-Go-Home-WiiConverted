"""Regression check of a conversion against a reference the user records from their own files.

    python tests/regression.py record --out <port folder> --reference <file>   # after a conversion you trust
    python tests/regression.py check  --out <port folder> --reference <file>   # after converting again

`record` writes the SHA-256 of every bigfile of the port folder and of every entry of the converted bigfile to the
reference file (keep it outside the repository: it is derived from game files).  `check` recomputes them and lists
what differs: whole files, then the first differing entries of the converted bigfile (key, name, sizes), which points
at the converter to look at.  Exit code 0 when everything matches, 1 otherwise.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from rghport.archive.bigfile import FILE_HDR, Big, is_bigfile  # noqa: E402

FORMAT = 1


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def bigfiles(out: str) -> list[str]:
    return sorted(fn for fn in os.listdir(out) if fn.lower().endswith(".bf") and is_bigfile(os.path.join(out, fn)))


def main_bigfile(out: str, names: list[str]) -> str:
    mains = [fn for fn in names if "." not in fn[:-3]]
    if len(mains) != 1:
        raise SystemExit("expected one converted bigfile in %s, found %s" % (out, mains or names))
    return mains[0]


def entry_hashes(path: str) -> dict[str, dict]:
    """{KEY: {"name", "stored", "sha1"}} of every entry, over its file header, payload and reference table."""
    big = Big(path)
    out = {}
    try:
        for e in big.entries():
            ln, lu, lr, fl = big.file_header(e)
            raw = big._read(e.pos, FILE_HDR + ln + lr)
            out["%08X" % e.key] = {"name": e.name, "stored": len(raw), "sha1": hashlib.sha1(raw).hexdigest()}
    finally:
        big.close()
    return out


def snapshot(out: str) -> dict:
    names = bigfiles(out)
    main = main_bigfile(out, names)
    return {"format": FORMAT, "main": main, "files": {fn: sha256_file(os.path.join(out, fn)) for fn in names},
            "entries": entry_hashes(os.path.join(out, main))}


def cmd_record(a) -> int:
    snap = snapshot(a.out)
    with open(a.reference, "w") as f:
        json.dump(snap, f, indent=0)
    for fn, h in snap["files"].items():
        print("%s  %s" % (h, fn))
    print("reference written: %d files, %d entries of %s" % (len(snap["files"]), len(snap["entries"]), snap["main"]))
    return 0


def cmd_check(a) -> int:
    with open(a.reference) as f:
        ref = json.load(f)
    cur = snapshot(a.out)
    bad = 0
    for fn in sorted(set(ref["files"]) | set(cur["files"])):
        r, c = ref["files"].get(fn), cur["files"].get(fn)
        state = "same" if r == c else ("missing" if c is None else ("new" if r is None else "DIFFERS"))
        print("%-8s %s  %s" % (state, c or r, fn))
        bad += state != "same"
    if ref["files"].get(ref["main"]) != cur["files"].get(cur["main"]):
        diffs = [k for k in sorted(set(ref["entries"]) | set(cur["entries"]))
                 if ref["entries"].get(k) != cur["entries"].get(k)]
        print("%d entries of %s differ" % (len(diffs), cur["main"]))
        for k in diffs[:a.show]:
            r, c = ref["entries"].get(k), cur["entries"].get(k)
            print("   %s %-40s reference %s bytes, now %s bytes" % (k, (c or r)["name"], r and r["stored"],
                                                                  c and c["stored"]))
    print("RESULT: %s" % ("MATCH" if not bad else "%d file(s) differ" % bad))
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("record", cmd_record), ("check", cmd_check)):
        p = sub.add_parser(name)
        p.add_argument("--out", required=True, metavar="DIR", help="the port folder of a conversion")
        p.add_argument("--reference", required=True, metavar="FILE", help="reference file (outside the repository)")
        p.add_argument("--show", type=int, default=20, help="check: differing entries to list")
        p.set_defaults(func=fn)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
