#!/bin/sh
# dock/build_solo.sh [TUNE[:SONG] ...]: FM-1 SID as a firmware of its own: fm1-dock with FM-1 SID as its only
# module (and the sine), starting in it, its tunes after the modules (dock/solo: PACK_AFTER_IMAGE).
#   build/solo/fm1-sid.fwsc        the package (installable: fm1-dock's installer, USB and rescue)
#   build/solo/fm1-sid_xip.bin     the same app for fm1-emu (fm1-live --bin)
#   build/solo/fm1sid-base.bin     the app without tunes: the web page appends a pack (web/pack.js)
# No tunes: GoatTracker 2's example songs (tunes/goattracker), Dojo first. RELEASE=X.Y names the
# package fm1-sid-X.Y.fwsc and sets its version. Needs fm1-dock's tree (FM1_DOCK, default ../fm1-dock
# next to this repository's root), its toolchain (JIELI_TOOLCHAIN) and AC79 SDK (AC79_SDK), ext/
# (tools/fetch_ext.sh), DEJAVU_MONO where DejaVu Sans Mono is not in Debian's place.
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
HB="${FM1_DOCK:-${FM1_HARBOR:-$(cd .. && pwd)/../fm1-dock}}"
O=build/solo
mkdir -p "$O" build/dock
python3 firmware/gen_assets.py build/dock/assets.h
cc -O2 -Isrc -o build/dock/combtab tools/combtab.c
build/dock/combtab > build/dock/sid_comb.h
if [ $# -eq 0 ]; then
    set -- tunes/goattracker/dojo.sng tunes/goattracker/unleash.sng tunes/goattracker/sanction.sng \
           tunes/goattracker/consultant.sng tunes/goattracker/transylvanian.sng tunes/goattracker/hyperspace.sng \
           tunes/goattracker/sixpack.sng tunes/goattracker/ghosttrackers.sng tunes/goattracker/everlasting.sng \
           tunes/goattracker/cabrinigreen.sng
fi
python3 - "$O/pack.bin" "$@" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "tools")
import sidpack
tunes = []
for t in sys.argv[2:]:
    path, _, song = t.partition(":")
    tunes.append((Path(path).read_bytes(), int(song) if song else 0))
Path(sys.argv[1]).write_bytes(sidpack.pack(tunes, 0, 0))
print(f"{sys.argv[1]}: {len(tunes)} tunes, {Path(sys.argv[1]).stat().st_size} bytes")
PY
HERE=$PWD
solo() {    # fm1-dock's build of FM-1 SID's own firmware, with what else is given
    (cd "$HB" && HB_MOUNT="$HERE" python3 tools/build.py --modules "$HERE/dock/solo" --name fm1-sid \
        --app-name "FM-1 SID" --identity FM-1_922 ${RELEASE:+--release "$RELEASE"} "$@")
}
solo --no-package | tail -3                     # the app without tunes, for the web page (no SDK needed)
cp "$HB/build/fm1-sid.bin" "$O/fm1sid-base.bin"
solo --pack "$HERE/$O/pack.bin" | tee "$O/build.log" | tail -4   # the package with the tunes
cp "$HB/build/fm1-sid.bin" "$HB/build/fm1-sid_xip.bin" "$HB/build/fm1-sid.syms" "$O/"
cp "$HB"/build/fm1-sid*.fwsc "$O/"
ls "$O"/*.fwsc
