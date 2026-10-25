#!/usr/bin/env python3
"""A firmware image with a tune pack (firmware/main.c reads it at the image's end):
    tools/sidpack.py OUT.bin [--xip | --boot] [--first N] [--own] [--chip 6581|8580] TUNE[:SONG] ...
from build/fw/sidplayer_base.bin (firmware/build.sh). --xip prefixes the 0x120 bytes that
fm1-emu --bin and the web emulator's raw boot expect; --boot takes build/fw_boot's instead
(DOCK_BOOT=1 firmware/build.sh), for fm1-dock's boot slot (its tools/hbboot.py make OUT.bin
--name FM1SID --no-data makes the .dock); --first picks the entry it starts on,
--own starts on fm1-sid's model instead of reSID, --chip sets the chip for every tune.
A TUNE is a PSID / RSID file (.sid) or a GoatTracker 2 song (.sng, player/gt2.c). Song 0 (the
default) is the tune's own start song (a .sng's first subtune). The web page builds the same pack
in JavaScript (web/sid.js).
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
    """Why player/psid.c's psid_load would turn the file down (None: it takes it): a PSID or
    RSID header and the C64 data inside the file, after the header (#63); not an RSID tune in
    C64 BASIC (no BASIC ROM)."""
    if len(b) < 0x76 or b[:4] not in (b"PSID", b"RSID"):
        return "not a PSID / RSID file"
    if b[:4] == b"RSID" and b[5] >= 2 and len(b) >= 0x7c and b[0x77] & 2:
        return "an RSID tune in C64 BASIC (needs the BASIC ROM)"
    off, load = struct.unpack(">HH", b[6:10])
    if off < 0x76 or off > len(b):
        return f"the data offset {off:#x} is not inside the file"
    if not load and len(b) - off < 2:
        return "no load address"
    return None


def gt2_songs(b):
    """A GoatTracker 2 song as player/gt2.c's gt2_load reads it: its subtunes, or why it turns
    the file down (a str). GTS3..GTS5: the texts, the order lists, instruments, the four tables
    and the patterns, each inside the file and inside GoatTracker's limits."""
    if len(b) < 101 or b[:3] != b"GTS" or not b"3"[0] <= b[3] <= b"5"[0]:
        return "not a GoatTracker 2 song (GTS3..GTS5)"
    pos = 100

    def byte():
        nonlocal pos
        if pos >= len(b):
            raise ValueError("cut short")
        pos += 1
        return b[pos - 1]

    def skip(n):
        nonlocal pos
        if n > len(b) - pos:
            raise ValueError("cut short")
        pos += n
    try:
        subtunes = byte()
        if subtunes > 32:
            return f"{subtunes} subtunes (GoatTracker has 32)"
        lens = []
        for _ in range(subtunes):
            for _ in range(3):
                n = byte() + 1
                skip(n)
                lens.append(next((i for i in range(min(n, 254)) if b[pos - n + i] == 0xFF), min(n, 254)))
        ins = byte()
        if ins >= 64:
            return f"{ins} instruments (GoatTracker has 63)"
        skip(25 * ins)
        for _ in range(4):
            n = byte()
            if n > 255:
                return "a table longer than 255"
            skip(2 * n)
        patts = byte()
        if patts > 208:
            return f"{patts} patterns (GoatTracker has 208)"
        for _ in range(patts):
            n = byte()
            if n > 129:
                return f"a pattern of {n - 1} rows (GoatTracker has 128)"
            skip(4 * n)
    except ValueError as e:
        return str(e)
    if not subtunes or 0 in lens[:3]:
        return "no subtune, or the first one empty"
    return subtunes


def tune_problem(b):
    """Why the firmware would turn the file down (None: it plays it)."""
    if b[:3] == b"GTS":
        n = gt2_songs(b)
        return n if isinstance(n, str) else None
    return psid_problem(b)


def tune_songs(b):
    return gt2_songs(b) if b[:3] == b"GTS" else struct.unpack(">H", b[14:16])[0]


def tune_args(ap, required=True):
    """The tune options sidpack and fwsc.py share."""
    ap.add_argument("tunes", nargs="+" if required else "*", metavar="TUNE[:SONG]",
                    help="a .sid (PSID / RSID) or a GoatTracker 2 .sng")
    ap.add_argument("--first", type=int, default=0, help="the entry it starts on")
    ap.add_argument("--own", action="store_true", help="start on fm1-sid's own model, not reSID")
    ap.add_argument("--chip", choices=("6581", "8580"), help="the chip for every tune")


def tunes_of(specs):
    """[(bytes, song)] from TUNE[:SONG]; a file named twice is stored once."""
    files, tunes = {}, []
    for t in specs:
        path, _, song = t.partition(":")
        if path not in files:
            files[path] = Path(path).read_bytes()
            why = tune_problem(files[path])
            if why:
                raise SystemExit(f"{path}: {why}")
        songs = tune_songs(files[path])
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
    ap.add_argument("--boot", action="store_true", help="for fm1-dock's boot slot (build/fw_boot)")
    a = ap.parse_args()
    if a.xip and a.boot:
        raise SystemExit("--xip or --boot, not both")
    tunes = tunes_of(a.tunes)
    if not 0 <= a.first < len(tunes):
        raise SystemExit(f"--first {a.first}: there are {len(tunes)} entries")
    fw, slot = ("fw_boot", 0x7E000) if a.boot else ("fw", 0x8DFBC)
    img = (ROOT / "build" / fw / "sidplayer_base.bin").read_bytes() + pack(tunes, a.first, flags_of(a))
    if len(img) > slot:
        raise SystemExit(f"{len(img)} bytes: more than the slot ({slot}); fewer or shorter tunes")
    Path(a.out).write_bytes((b"\0" * 0x120 if a.xip else b"") + img)
    print(f"{a.out}: {len(tunes)} entries, {len(img)} bytes of {slot}")


if __name__ == "__main__":
    main()
