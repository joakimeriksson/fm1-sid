#!/usr/bin/env python3
"""A firmware image with a tune pack (firmware/main.c reads it at the image's end):
    tools/sidpack.py OUT.bin [--xip] [--first N] [--own] [--chip 6581|8580] TUNE.sid[:SONG] ...
from build/fw/sidplayer_base.bin (firmware/build.sh). --xip prefixes the 0x120 bytes that
fm1-emu --bin and the web emulator's raw boot expect; --first picks the entry it starts on,
--own starts on fm1-sid's model instead of reSID, --chip sets the chip for every tune.
Song 0 (the default) is the tune's own start song. The web page builds the same pack in
JavaScript (web/sid.js).
SPDX-License-Identifier: BSD-2-Clause"""
import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def pack(tunes, first=0, flags=0):
    """tunes: [(bytes, song)]; the same bytes object is stored once."""
    blobs, where = [], {}
    head = 16 + 12 * len(tunes)
    off = head
    ents = []
    for data, song in tunes:
        if id(data) not in where:
            where[id(data)] = off
            blobs.append(data + b"\0" * (-len(data) % 4))
            off += len(blobs[-1])
        ents.append(struct.pack("<III", where[id(data)], len(data), song))
    return b"SIDPACK1" + struct.pack("<II", len(tunes), first | flags << 16) + b"".join(ents) + b"".join(blobs)


def psid_problem(b):
    """Why player/psid.c's psid_load would turn the file down (None: it takes it): a PSID header
    and the C64 data inside the file, after the header (#63)."""
    if len(b) < 0x76 or b[:4] != b"PSID":
        return "not a PSID file (RSID tunes need a whole C64)"
    off, load = struct.unpack(">HH", b[6:10])
    if off < 0x76 or off > len(b):
        return f"the data offset {off:#x} is not inside the file"
    if not load and len(b) - off < 2:
        return "no load address"
    return None


def tune_args(ap, required=True):
    """The tune options sidpack and fwsc.py share."""
    ap.add_argument("tunes", nargs="+" if required else "*", metavar="TUNE.sid[:SONG]")
    ap.add_argument("--first", type=int, default=0, help="the entry it starts on")
    ap.add_argument("--own", action="store_true", help="start on fm1-sid's own model, not reSID")
    ap.add_argument("--chip", choices=("6581", "8580"), help="the chip for every tune")


def tunes_of(specs):
    """[(bytes, song)] from TUNE.sid[:SONG]; a file named twice is stored once."""
    files, tunes = {}, []
    for t in specs:
        path, _, song = t.partition(":")
        if path not in files:
            files[path] = Path(path).read_bytes()
            why = psid_problem(files[path])
            if why:
                raise SystemExit(f"{path}: {why}")
        songs = struct.unpack(">H", files[path][14:16])[0]
        if int(song or 0) > songs:
            raise SystemExit(f"{t}: the tune has {songs} song(s)")
        tunes.append((files[path], int(song or 0)))
    return tunes


def flags_of(a):
    return int(a.own) | {None: 0, "6581": 2, "8580": 4}[a.chip]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    tune_args(ap)
    ap.add_argument("--xip", action="store_true")
    a = ap.parse_args()
    tunes = tunes_of(a.tunes)
    if not 0 <= a.first < len(tunes):
        raise SystemExit(f"--first {a.first}: there are {len(tunes)} entries")
    img = (ROOT / "build" / "fw" / "sidplayer_base.bin").read_bytes() + pack(tunes, a.first, flags_of(a))
    assert len(img) <= 0x8DFBC, f"{len(img)} bytes: more than the app slot"
    Path(a.out).write_bytes((b"\0" * 0x120 if a.xip else b"") + img)
    print(f"{a.out}: {len(tunes)} entries, {len(img)} bytes")


if __name__ == "__main__":
    main()
