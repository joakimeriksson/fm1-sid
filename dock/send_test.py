#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A tune from the computer into FM-1 SID's own firmware, in fm1-emu (fm1-live --shot --usb): fm1-dock's
MODULE frames over the emulated USB, as tools/dock.py send and the page's "Play on my FM-1" send them.

  dock/send_test.py [PACKAGE.fwsc]     (default build/solo/fm1-sid.fwsc: dock/build_solo.sh)

Needs fm1-dock's tree (FM1_DOCK: tools/hbdeploy.py) and fm1-live (FM1_LIVE, default
~/work/fm1-emu/target/release/fm1-live). Checks: every frame answered and taken, the tune playing
afterwards (PSID, RSID, a subtune), and a file that is not a tune refused with the tune before it
still playing."""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCK = Path(os.environ.get("FM1_DOCK", ROOT.parent / "fm1-dock"))
LIVE = os.environ.get("FM1_LIVE", str(Path.home() / "work/fm1-emu/target/release/fm1-live"))
sys.path.insert(0, str(DOCK / "tools"))
import hbdeploy as hd  # noqa: E402

PKG = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build/solo/fm1-sid.fwsc")
failed = []


def send(*files, at=3000, gap=250, after=3000):
    """each (bytes, song) in turn from `at` ms, a step every `gap` ms; -> per file, per step whether
    it was taken, and the audio peak at the end. Every frame goes twice, 100 ms apart, as a sender
    that gets no answer sends it again (tools/dock.py and the page wait 1 s): here the frames go out
    unanswered, and now and then one is lost after the FM-1 took its packets (fm1-emu, 2026-10-25:
    about 1 in 80; not found yet). A step is taken when either copy is."""
    items, plan, seq, t = [], [], 0, at
    for blob, song in files:
        steps = [b"B" + struct.pack("<II", len(blob), song)]
        steps += [b"D" + struct.pack("<I", a) + blob[a:a + 256] for a in range(0, len(blob), 256)]
        steps.append(b"E")
        mine = []
        for st in steps:
            copies = []
            for k in (0, 100):
                seq = (seq + 1) & 0x7F
                items.append(f"{t + k}:{hd.frame(hd.MODULE, seq, st).hex()}")
                copies.append(seq)
            mine.append(copies)
            t += gap
        plan.append(mine)
        t += 1000
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "work").mkdir()
        r = subprocess.run([LIVE, "--fwsc", PKG, "--usb", "--shot", str(t + after)], cwd=d, capture_output=True,
                           text=True, env=dict(os.environ, FM1_USB_SEND=";".join(items)), timeout=600)
    order = []                                  # (the replies in order: seq wraps at 128)
    for line in r.stdout.splitlines():
        if "usb in:" in line:
            p = hd.parse(list(bytes.fromhex(line.split("usb in:")[1].replace(" ", ""))))
            if p and p[0] == hd.MODULE:
                order.append((p[1], bytes(p[2][:2])))
    out, k = [], 0
    for mine in plan:
        res = []
        for copies in mine:
            st = []
            while k < len(order) and order[k][0] in copies and len(st) < 2:
                st.append(order[k][1])
                k += 1
            res.append(st)
        out.append(res)
    peak = next((int(ln.rsplit(":", 1)[1]) for ln in r.stdout.splitlines() if "audio peak" in ln), -1)
    return out, peak


def check(name, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}: {what}")
    if not ok:
        failed.append(name)


def tune(name):
    """a tune of our own for the test: house.sid (FM-1 SID's own, in web/)"""
    return (ROOT / "web" / name).read_bytes()


def taken(r):
    return all(any(x[0] == 0 for x in st) for st in r)


def main():
    house = tune("house.sid")
    (r,), peak = send((house, 0))
    check("psid", taken(r) and peak > 500, f"house.sid in {len(r)} steps, all taken, playing (peak {peak})")
    (r,), peak = send((house, 1))
    check("subtune", taken(r) and peak > 500, f"with song 1 asked for (peak {peak})")
    rsid = bytearray(house)                     # (the same tune as an RSID: taken and started; made from a PSID it need not sound)
    rsid[0] = ord("R")
    rsid[0x77] &= ~2 & 0xFF
    (r,), peak = send((bytes(rsid), 0))
    check("rsid", taken(r), f"as an RSID, all taken (peak {peak})")
    (a, b), peak = send((house, 0), (b"GTS5" + bytes(400), 0))   # a GoatTracker song: not sent (yet)
    check("refused", taken(a) and bytes([11, 3]) in b[1] and peak > 500,
          f"a GoatTracker song after house.sid refused at its header (FM-1 SID's 3), house.sid playing on (peak {peak})")
    print("all passed" if not failed else f"FAILED: {', '.join(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
