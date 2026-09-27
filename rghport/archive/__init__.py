"""Bigfile access: reading (bigfile), LZO1X (lzo), streaming writer (writer), in-place patching (patch), packages and
the world-list view (packages), record indexes (index)."""
from .bigfile import Big, Entry, Ref


def open_bigfile(path: str) -> Big:
    """A read-only bigfile: `read_key(key) -> bytes`, `find(key)`, `entries()`, `close()`."""
    return Big(path)


__all__ = ["Big", "Entry", "Ref", "open_bigfile"]
