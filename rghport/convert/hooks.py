"""Integration interface of the scripts phase (the package rghport.scripts).

The build looks up these optional functions in rghport.scripts when that package is present; a function it does not
define changes nothing:

    add_arguments(parser)                 extra options of `rghport convert` (e.g. the Wii script module); their values
                                          reach prepare() through ctx.options
    prepare(ctx)                          once per build, before the first entry is written.  ctx (convert.context)
                                          carries both archives and their record indexes, ctx.kinds (record key ->
                                          kind), ctx.cache_dir (generated files go below it) and ctx.options (the parsed
                                          command line: "wii", "pc", "pc_exe", "out", "cache", ...)
    convert(kind, key, body, ctx)         a package record of a script kind ("script:*"): its PC body, or None to leave
                                          the record to the builder's other rules (the PC release's record when the PC
                                          archive holds the key, otherwise the Wii bytes)
    convert_entry(key, name, data, ctx)   a loose bigfile entry (not a bin, not a shadow shortcut, not default.cfg): its
                                          PC bytes, or None to keep the Wii bytes

The development option --script-overrides stays in front of the hooks: a record or entry override wins.
"""
from __future__ import annotations

import importlib
import importlib.util

SCRIPTS_PACKAGE = "rghport.scripts"
SCRIPT_KIND_PREFIX = "script:"
HOOKS = ("add_arguments", "prepare", "convert", "convert_entry")


class ScriptHooks:
    """The hook functions of a scripts package (None for each function it does not define)."""

    def __init__(self, module=None):
        self.module = module
        for name in HOOKS:
            setattr(self, name, getattr(module, name, None) if module is not None else None)

    def present(self) -> list[str]:
        return [name for name in HOOKS if getattr(self, name) is not None]


def script_hooks(package: str = SCRIPTS_PACKAGE) -> ScriptHooks:
    """The hooks of the scripts package when it is installed.  An error while importing it is raised, not hidden: a build
    that silently lost its script conversion would look complete."""
    if importlib.util.find_spec(package) is None:
        return ScriptHooks()
    return ScriptHooks(importlib.import_module(package))
