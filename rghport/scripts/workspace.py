"""Inputs and cache of the scripts phase.

The phase reads the user's files and writes everything it derives into `<cache>/scripts/`:

    wii_bigfile   the Wii game's main bigfile
    wii_module    the Wii script module (SCR2CPP build of the scripts)
    pc_folder     the PC release folder: its main bigfile and its executable
    wii_elf       optional: the symbol-rich Wii executable (shipped unused on the Japanese Wii disc)
    cache         the folder the user chose for generated data

Modules ask the configured workspace for paths and archives (`workspace.ws()`); nothing is resolved at import time, so
the command line configures the workspace first and every module sees the same inputs.
"""
from __future__ import annotations

import hashlib
import json
import os

from ..archive import bigfile as _bigfile
from ..archive import index as _index
from ..archive import packages as _packages


class InputError(Exception):
    pass


def _main_bigfile(path: str) -> str:
    """A bigfile path, or the main bigfile of a folder: the `.bf` file whose name has no second extension (the sound
    and video shadow archives carry one, e.g. `<name>.<platform>.sns.bf`)."""
    if os.path.isfile(path):
        return path
    if not os.path.isdir(path):
        raise InputError("not a file or folder: %s" % path)
    cands = []
    for f in sorted(os.listdir(path)):
        stem, ext = os.path.splitext(f)
        if ext.lower() == ".bf" and "." not in stem and "$" not in stem:
            p = os.path.join(path, f)
            if _bigfile.is_bigfile(p):
                cands.append(p)
    if len(cands) != 1:
        raise InputError("expected one main bigfile in %s, found %s" % (path, [os.path.basename(c) for c in cands]))
    return cands[0]


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Workspace:
    def __init__(self, cache: str, wii_bigfile: str | None = None, wii_module: str | None = None,
                 pc_folder: str | None = None, wii_elf: str | None = None, log=None):
        self.cache = os.path.abspath(cache)
        self.wii_bigfile = _main_bigfile(wii_bigfile) if wii_bigfile else None
        self.wii_module = wii_module
        self.pc_folder = pc_folder
        self.wii_elf = wii_elf
        self.log = log or (lambda msg: None)
        self._big = {}
        self._rec = {}
        self._arch = {}
        self._rso = None
        self._elf = None
        for what, p in (("Wii script module", wii_module), ("Wii executable", wii_elf)):
            if p and not os.path.isfile(p):
                raise InputError("%s not found: %s" % (what, p))

    # ---- cache -------------------------------------------------------------
    def path(self, *parts: str) -> str:
        p = os.path.join(self.cache, "scripts", *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    def exists(self, *parts: str) -> bool:
        return os.path.exists(os.path.join(self.cache, "scripts", *parts))

    def load_json(self, name: str, default=None):
        p = os.path.join(self.cache, "scripts", name)
        if not os.path.exists(p):
            return default
        with open(p, encoding="utf-8") as f:
            return json.load(f)

    def save_json(self, name: str, obj, indent=None):
        p = self.path(name)
        tmp = p + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(obj, f, indent=indent)
        os.replace(tmp, p)
        return p

    # ---- inputs ------------------------------------------------------------
    def require(self, *names: str):
        for n in names:
            if not getattr(self, n):
                raise InputError("this step needs the input %s" % n)

    @property
    def pc_bigfile(self) -> str:
        self.require("pc_folder")
        return _main_bigfile(self.pc_folder)

    def pc_executable(self) -> str:
        """The PC executable: the `.exe` of the PC folder that exports the script registration functions."""
        self.require("pc_folder")
        from . import pcexe
        found = [os.path.join(self.pc_folder, f) for f in sorted(os.listdir(self.pc_folder))
                 if f.lower().endswith(".exe") and pcexe.is_game_executable(os.path.join(self.pc_folder, f))]
        if len(found) != 1:
            raise InputError("expected one game executable in %s, found %s" % (self.pc_folder, found))
        return found[0]

    def big(self, which: str) -> _bigfile.Big:
        """`wii` or `pc`: the main bigfile, opened once."""
        if which not in self._big:
            path = self.wii_bigfile if which == "wii" else self.pc_bigfile
            if not path:
                raise InputError("this step needs the %s bigfile" % which)
            self._big[which] = _bigfile.Big(path)
        return self._big[which]

    def records(self, which: str) -> dict[int, int]:
        """record key -> the first package holding it (cached index, rebuilt when the bigfile changes)."""
        if which not in self._rec:
            big = self.big(which)
            self._rec[which] = _index.load_records(big, self.path("%s_records.json" % which), self.log)
        return self._rec[which]

    def archive(self, which: str) -> _packages.Archive:
        if which not in self._arch:
            self._arch[which] = _packages.Archive(self.big(which))
        return self._arch[which]

    def body(self, which: str, key: int) -> bytes | None:
        """A package record's body (level 3, in-memory size first), found through the record index."""
        pk = self.records(which).get(key)
        if pk is None:
            return None
        p = self.archive(which).package(pk)
        return p.body(key) if p else None

    def entry(self, which: str, key: int) -> bytes | None:
        """A loose bigfile entry (universe files, global libraries) by key."""
        big = self.big(which)
        e = big.find(key)
        return big.read(e) if e is not None else None

    def rso(self):
        if self._rso is None:
            self.require("wii_module")
            from .rso import RSO
            self._rso = RSO(self.wii_module)
        return self._rso

    def elf(self):
        if self._elf is None:
            self.require("wii_elf")
            from .elf import Elf
            self._elf = Elf(self.wii_elf)
        return self._elf


_WS: Workspace | None = None


def configure(**kw) -> Workspace:
    global _WS
    _WS = Workspace(**kw)
    return _WS


def ws() -> Workspace:
    if _WS is None:
        raise InputError("the scripts workspace is not configured")
    return _WS
