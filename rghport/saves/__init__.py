"""Wii save import: converts a Rabbids Go Home Wii save into the PC save format of the port.

    from rghport.saves import import_save
    import_save(wii_save_dir, out_dir, bigfile)          # returns a process exit code

Command line: `rghport import-save` (rghport.saves.cli.register) or `python -m rghport.saves import-save`.
"""
from .wii_import import SaveImportError, import_save

__all__ = ["import_save", "SaveImportError"]
