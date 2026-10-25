#!/bin/sh
# dock/build.sh [TUNE[:SONG] ...]: FM-1 SID as an fm1-dock module -> build/dock/fm1sid.dock, for
# fm1-dock's tools/dock.py deploy (over USB-MIDI) or its image (tools/build.py --modules). Needs
# fm1-dock's tree (FM1_DOCK, default ../fm1-dock next to this repository's root) for
# tools/hbmod.py, its module interface and toolchain settings; ext/ (tools/fetch_ext.sh); a host cc.
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
HB="${FM1_DOCK:-${FM1_HARBOR:-$(cd .. && pwd)/../fm1-dock}}"   # (FM1_HARBOR: the old name)
O=build/dock
mkdir -p "$O"
python3 firmware/gen_assets.py "$O/assets.h"
cc -O2 -Isrc -o "$O/combtab" tools/combtab.c
"$O/combtab" > "$O/sid_comb.h"
python3 dock/mkpack.py "$O/pack.c" "$@"
(cd "$HB" && HB_MOUNT="$OLDPWD" python3 tools/hbmod.py "$OLDPWD/dock") | tee "$O/hbmod.log"
cp "$HB/build/modules/dock.dock" "$O/fm1sid.dock"
echo "$O/fm1sid.dock"
