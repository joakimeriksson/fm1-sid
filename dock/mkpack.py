#!/usr/bin/env python3
"""dock/mkpack.py OUT.c [--first N] [--own] [--chip 6581|8580] TUNE[:SONG] ...: FM-1 SID's tune pack
(tools/sidpack.py's, the same bytes) as a C array, `_pack`, for the fm1-dock module (in the Dinghy
firmware it follows the image; a module carries it in its own flash). No tunes: an empty pack (the
firmware's own tune).
SPDX-License-Identifier: BSD-2-Clause"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import sidpack  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("tunes", nargs="*")
ap.add_argument("--first", type=int, default=0)
ap.add_argument("--own", action="store_true")
ap.add_argument("--chip", choices=["6581", "8580"])
a = ap.parse_args()
tunes = []
for t in a.tunes:
    path, _, song = t.partition(":")
    tunes.append((Path(path).read_bytes(), int(song) if song else 0))
flags = sidpack.flags_of(a)
blob = sidpack.pack(tunes, a.first, flags) if tunes else b"SIDPACK1" + bytes(8)
rows = [", ".join(str(b) for b in blob[i:i + 24]) for i in range(0, len(blob), 24)]
Path(a.out).write_text("/* made by dock/mkpack.py: do not edit */\n#include <stdint.h>\n"
                       f"const uint8_t _pack[{len(blob)}] __attribute__((aligned(4))) = {{\n" +
                       ",\n".join(rows) + "\n};\n")
print(f"{a.out}: {len(tunes)} tunes, {len(blob)} bytes")
