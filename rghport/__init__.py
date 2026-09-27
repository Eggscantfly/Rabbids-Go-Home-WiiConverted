"""rghport: builds a Windows port of the Wii version of Rabbids Go Home from files the user owns.

    python -m rghport convert --wii <Wii game data folder> --pc <PC release folder> --out <port folder>
    python -m rghport check --out <port folder>

Packages:
    archive    bigfile reading, writing and patching; packages; record indexes
    formats    binary stream framework and record formats
    walk       world and list walker that assigns a kind to every package record
    convert    Wii -> PC converters, package builder, whole-archive build, port folder assembly and check
    scripts    the scripts phase (plugs into the build through convert/hooks.py)
    saves      Wii save import
"""
__version__ = "0.1.0"
