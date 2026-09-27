"""RGHS - the setup's looping music: a 32-byte header with the loop points, then an Ogg Vorbis stream.

    python tools/rghs.py from-wav <in.wav> <out.rghs> --loop-start N --loop-end N [--quality Q]
    python tools/rghs.py wrap <in.ogg> <out.rghs> --loop-start N --loop-end N [--rate HZ --channels C --frames N]
    python tools/rghs.py info <file.rghs>
    python tools/rghs.py unwrap <file.rghs> <out.ogg>

Header (little-endian, 32 bytes):
    0x00  "RGHS"
    0x04  u32  version (1)
    0x08  u32  sample rate (Hz)
    0x0C  u32  channels
    0x10  u32  loop start: the first PCM frame of the loop
    0x14  u32  loop end: the frame after the loop's last (0: the piece plays through and starts over)
    0x18  u32  total PCM frames (0: unknown)
    0x1C  u32  size of the Ogg Vorbis stream that follows
    0x20  the Ogg Vorbis stream

The launcher plays Assets/Audio/setup.rghs in setup mode (launcher/music.cpp: stb_vorbis + waveOut), looping
between the two frames, and fades it out when the setup hands over to the launcher.  `from-wav` encodes with ffmpeg
(libvorbis; quality 5 is about 160 kbps) and checks the stream decodes to the WAV's frame count when the soundfile
module is available.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

MAGIC = b"RGHS"
VERSION = 1
HEADER = struct.Struct("<4sIIIIIII")
assert HEADER.size == 32


def pack(ogg: bytes, rate: int, channels: int, loop_start: int, loop_end: int, frames: int) -> bytes:
    if loop_end and loop_end <= loop_start:
        raise SystemExit("the loop end must come after the loop start (or be 0 for no loop)")
    return HEADER.pack(MAGIC, VERSION, rate, channels, loop_start, loop_end, frames, len(ogg)) + ogg


def unpack(data: bytes) -> tuple[dict, bytes]:
    if len(data) < HEADER.size or data[:4] != MAGIC:
        raise SystemExit("not an RGHS file")
    magic, version, rate, channels, loop_start, loop_end, frames, size = HEADER.unpack_from(data)
    ogg = data[HEADER.size:HEADER.size + size]
    return {"version": version, "sample_rate": rate, "channels": channels, "loop_start": loop_start,
            "loop_end": loop_end, "frames": frames, "ogg_bytes": size}, ogg


def ogg_frames(path: str) -> int | None:
    """The PCM frames an Ogg Vorbis file decodes to, or None when the soundfile module is not there."""
    try:
        import soundfile
    except ImportError:
        return None
    return int(soundfile.info(path).frames)


def cmd_from_wav(a) -> int:
    with wave.open(a.wav) as w:
        rate, channels, frames = w.getframerate(), w.getnchannels(), w.getnframes()
    ffmpeg = a.ffmpeg or shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg not found: give --ffmpeg <path>")
    tmp = tempfile.NamedTemporaryFile(suffix=".ogg", delete=False)
    tmp.close()
    try:
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", a.wav, "-c:a", "libvorbis", "-q:a", str(a.quality),
                        tmp.name], check=True)
        decoded = ogg_frames(tmp.name)
        if decoded is not None and decoded != frames:
            print("note: the Ogg stream decodes to %d frames, the WAV had %d" % (decoded, frames), file=sys.stderr)
        with open(tmp.name, "rb") as f:
            ogg = f.read()
    finally:
        os.unlink(tmp.name)
    with open(a.out, "wb") as f:
        f.write(pack(ogg, rate, channels, a.loop_start, a.loop_end, frames))
    print("%s: %d Hz, %d channel(s), %d frames, loop %d-%d, %d bytes of Ogg Vorbis" % (
        a.out, rate, channels, frames, a.loop_start, a.loop_end, len(ogg)))
    return 0


def cmd_wrap(a) -> int:
    with open(a.ogg, "rb") as f:
        ogg = f.read()
    frames = a.frames
    rate, channels = a.rate, a.channels
    try:
        import soundfile
        i = soundfile.info(a.ogg)
        rate, channels, frames = rate or i.samplerate, channels or i.channels, frames or int(i.frames)
    except ImportError:
        pass
    if not rate or not channels:
        raise SystemExit("give --rate and --channels (the soundfile module is not there to read them)")
    with open(a.out, "wb") as f:
        f.write(pack(ogg, rate, channels, a.loop_start, a.loop_end, frames or 0))
    print("%s: %d Hz, %d channel(s), %d frames, loop %d-%d" % (a.out, rate, channels, frames or 0, a.loop_start, a.loop_end))
    return 0


def cmd_info(a) -> int:
    with open(a.file, "rb") as f:
        info, ogg = unpack(f.read())
    for k, v in info.items():
        print("%-12s %s" % (k, v))
    if info["sample_rate"]:
        print("%-12s %.3f s" % ("loop", (info["loop_end"] - info["loop_start"]) / info["sample_rate"]))
    return 0


def cmd_unwrap(a) -> int:
    with open(a.file, "rb") as f:
        _info, ogg = unpack(f.read())
    with open(a.out, "wb") as f:
        f.write(ogg)
    print("%s: %d bytes" % (a.out, len(ogg)))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="rghs", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("from-wav", help="encode a WAV with ffmpeg and wrap it")
    p.add_argument("wav")
    p.add_argument("out")
    p.add_argument("--loop-start", type=int, default=0)
    p.add_argument("--loop-end", type=int, default=0)
    p.add_argument("--quality", type=float, default=5, help="libvorbis quality (default 5)")
    p.add_argument("--ffmpeg", help="the ffmpeg program (default: the one on PATH)")
    p.set_defaults(func=cmd_from_wav)
    p = sub.add_parser("wrap", help="wrap an Ogg Vorbis file")
    p.add_argument("ogg")
    p.add_argument("out")
    p.add_argument("--loop-start", type=int, default=0)
    p.add_argument("--loop-end", type=int, default=0)
    p.add_argument("--rate", type=int, default=0)
    p.add_argument("--channels", type=int, default=0)
    p.add_argument("--frames", type=int, default=0)
    p.set_defaults(func=cmd_wrap)
    p = sub.add_parser("info", help="print the header")
    p.add_argument("file")
    p.set_defaults(func=cmd_info)
    p = sub.add_parser("unwrap", help="write the Ogg Vorbis stream out")
    p.add_argument("file")
    p.add_argument("out")
    p.set_defaults(func=cmd_unwrap)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
