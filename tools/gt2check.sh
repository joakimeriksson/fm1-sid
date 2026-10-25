#!/bin/bash
# player/gt2.c against GoatTracker's own playroutine: every subtune of every song, its 25 SID
# registers frame by frame (build/gt2log against sid-tracker's gt2dump, which compiles
# GoatTracker 2.76's gplay.c and gsong.c unmodified), until the song stops or FRAMES are up.
#   tools/gt2check.sh [SONG.sng ...]       (default: sid-tracker's songs and GoatTracker's examples)
# SID_TRACKER: a sid-tracker checkout (default ../../sid-tracker); GT2_EXAMPLES: a directory of
# .sng files (GoatTracker's release has examples/); FRAMES (default 30000: 10 minutes).
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
cd "$(dirname "$0")/.."
ST="${SID_TRACKER:-../../sid-tracker}"
FRAMES="${FRAMES:-30000}"
R="$ST/tools/gt2-refdump"
G="$ST/gt2-src"
[ -f "$R/main.c" ] || { echo "no sid-tracker at $ST (set SID_TRACKER)"; exit 1; }
mkdir -p build/gt2check
make -s build/gt2log
cc -O2 -w -I"$R/shim" -I"$G" -I"$G/bme" -o build/gt2check/gt2dump "$R/main.c" "$G/gplay.c" "$G/gsong.c" "$G/bme/bme_end.c"
if [ $# = 0 ]; then
    set -- "$ST"/sids/*.sng "$ST"/tests/songs/*.sng
    [ -n "${GT2_EXAMPLES:-}" ] && set -- "$@" "$GT2_EXAMPLES"/*.sng
fi
all=0 bad=0
for f in "$@"; do
    # (gt2dump takes paths shorter than 256 characters, so it runs where the song is)
    d="$(cd "$(dirname "$f")" && pwd)" b="$(basename "$f")"
    n="$(build/gt2log "$f" --frames 1 2>&1 >/dev/null | sed -n 's/.*, \([0-9]*\) subtune.*/\1/p')"
    [ -n "$n" ] || { echo "FAIL  $f: not loaded"; bad=$((bad + 1)); all=$((all + 1)); continue; }
    for ((s = 0; s < n; s++)); do
        (cd "$d" && "$OLDPWD/build/gt2check/gt2dump" "$b" --frames "$FRAMES" --subtune $s 2>/dev/null) |
            grep '^{"f"' > build/gt2check/ref.txt || true
        build/gt2log "$f" --frames "$FRAMES" --subtune $s 2>/dev/null | grep '^{"f"' > build/gt2check/ours.txt || true
        all=$((all + 1))
        if cmp -s build/gt2check/ref.txt build/gt2check/ours.txt; then
            echo "same  $b:$s  $(wc -l < build/gt2check/ref.txt | tr -d ' ') frames"
        else
            bad=$((bad + 1))
            echo "DIFF  $b:$s  $(cmp build/gt2check/ref.txt build/gt2check/ours.txt 2>&1 | head -1)"
        fi
    done
done
echo "$((all - bad)) of $all subtunes the same"
[ $bad = 0 ]
