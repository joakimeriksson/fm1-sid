#!/usr/bin/env python3
"""The SID player as an FM-1 update package (.fwsc), installable the way community firmwares
are and bootable by the emulator's page (../web):
    tools/fwsc.py [OUT.fwsc]       (default web/sidplayer.fwsc)
    tools/fwsc.py OUT.fwsc [--first N] [--own] [--chip 6581|8580] [--synth] TUNE.sid[:SONG] ...
The app is web/sidplayer.bin (firmware/build.sh: the firmware without a tune pack) plus a tune
pack (tools/sidpack.py). Without tunes the pack is its own tune (web/house.sid) and starts it in
the synth mode: an FM-1 that boots it is a synth, silent until a key is played; HOME switches to
the player and the tune. With tunes it starts in the player on the first (--synth: the synth),
and they are the tunes it has: there is no way to send it others while it runs, a new set is a
new package to install. Each tune is checked the way the firmware's psid_load reads it. The
package is Felucca's: its update loader
(ext/Felucca/firmware/loader, built here with the JieLi toolchain) and tools/fm1pkg_make.py,
which takes three files of JieLi's AC79 SDK (Apache-2.0; tools/fetch_ext.sh fetches them into
ext/ac79sdk and checks them against Felucca's hashes). Ship LICENSES/Apache-2.0.txt with it.
SPDX-License-Identifier: GPL-3.0-only"""
import argparse
import glob
import os
import sys
from pathlib import Path

CWD = Path.cwd()                                # (the paths given are from here, not from Felucca's)
ROOT = Path(__file__).resolve().parent.parent
FEL = ROOT / "ext" / "Felucca"
os.environ.setdefault("JIELI_TOOLCHAIN", (sorted(glob.glob(str(ROOT.parent / "reference" / "toolchain" / "jieli-linux-toolchains-*"))) or [""])[-1])
os.environ.setdefault("AC79_SDK", str(ROOT / "ext" / "ac79sdk"))
sys.path.insert(0, str(FEL / "tools"))
os.chdir(FEL)                                   # (Felucca's build helpers use repo-relative paths)
import build as felucca                         # noqa: E402
import fm1pkg_make as pkg                       # noqa: E402
sys.path.insert(0, str(ROOT / "tools"))
import sidpack                                  # noqa: E402

SYNTH = 8                                       # pack flag: start in the synth mode

PRODUCT = "FM-1_900"                            # the identity Felucca's own development builds carry


def main():
    ap = argparse.ArgumentParser(description="FM-1 SID as an update package, with its own tune or yours")
    sidpack.tune_args(ap, required=False)
    ap.add_argument("--synth", action="store_true", help="start in the synth mode (the default without tunes)")
    a = ap.parse_args()
    specs = [str(CWD / t) for t in a.tunes]     # (absolute: the cwd is Felucca's from here on)
    out = ROOT / "web" / "sidplayer.fwsc"
    if specs and specs[0].endswith(".fwsc"):
        out = Path(specs.pop(0))
    if specs:
        tunes, flags = sidpack.tunes_of(specs), sidpack.flags_of(a) | (SYNTH if a.synth else 0)
        if not 0 <= a.first < len(tunes):
            raise SystemExit(f"--first {a.first}: there are {len(tunes)} entries")
    else:
        tunes, flags = [((ROOT / "web" / "house.sid").read_bytes(), 0)], SYNTH
    app = (ROOT / "web" / "sidplayer.bin").read_bytes() + sidpack.pack(tunes, a.first, flags)
    if len(app) > 0x8DFBC:
        raise SystemExit(f"{len(app)} bytes: more than the app slot (581564); fewer or shorter tunes")
    for rel, sha in felucca.SDK_SHA256.items():
        if __import__("hashlib").sha256(pkg.sdk_file(rel)).hexdigest() != sha:
            raise SystemExit(f"{rel}: not the AC79NN_SDK_V1.2.1 file Felucca's packages are made with")
    ota = felucca.build_loader()
    data = pkg.ufw(pkg.flash_image(app, pkg.KEY), ota, PRODUCT)
    Path(out).write_bytes(data)
    print(f"{out}: {len(data)} bytes, app {len(app)} bytes ({len(tunes)} tune entries), identity {PRODUCT}")


if __name__ == "__main__":
    main()
