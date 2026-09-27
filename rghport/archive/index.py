"""Record indexes: record key -> the package holding it, built from a bigfile and kept in the cache folder.

Resources are shared between worlds, and a world's own package does not always carry a record it uses, so readers
fall back to the index when a key is missing locally.  The first package in bigfile entry order wins for a key
several packages hold.  A cached index names the bigfile it was built from (a fingerprint of its header and file
table) and is rebuilt when that does not match.
"""
from __future__ import annotations

import hashlib
import json
import os
import struct

from .bigfile import Big
from .packages import walk

FORMAT = 1


def fingerprint(big: Big) -> str:
    """SHA-1 of the header, the file size and every live file record's key, position, lengths and checksum."""
    h = hashlib.sha1()
    h.update(struct.pack("<Q", big.size))
    h.update(big.header)
    for e in big.entries():
        h.update(struct.pack("<IQIII", e.key, e.pos, e.length_disk, e.length_user, e.checksum))
        h.update(e.name.encode("latin-1", "replace"))
    return h.hexdigest()


def build_records(big: Big, log=None) -> dict[int, int]:
    """record key -> the first package (0xFFF.....) holding it."""
    recs: dict[int, int] = {}
    for e in big.entries():
        if e.key >> 20 != 0xFFF:
            continue
        data = big.read(e)
        try:
            rl, _end = walk(data)
        except Exception as ex:          # noqa: BLE001
            if log:
                log("package %08X: %s" % (e.key, ex))
            continue
        for o, k, ln in rl:
            recs.setdefault(k, e.key)
    return recs


def load_records(big: Big, path: str, log=None) -> dict[int, int]:
    """The record index of `big`, read from the JSON file at `path`; built and written there when the file is missing
    or belongs to another bigfile."""
    fp = fingerprint(big)
    if os.path.exists(path):
        try:
            with open(path) as f:
                doc = json.load(f)
            if doc.get("format") == FORMAT and doc.get("source") == fp:
                return {int(k, 16): int(v, 16) for k, v in doc["records"].items()}
        except (OSError, ValueError, KeyError):
            pass
    if log:
        log("indexing the records of %s" % os.path.basename(big.path))
    recs = build_records(big, log)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"format": FORMAT, "source": fp, "records": {"%08X" % k: "%08X" % v for k, v in recs.items()}}, f)
    os.replace(tmp, path)
    return recs
