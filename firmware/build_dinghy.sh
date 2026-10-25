#!/bin/sh
# Build FM-1 SID on Dinghy 0.3 (dinghy/: its platform; firmware/fm1sid.c: the app) for the FM-1:
#   firmware/build_dinghy.sh -> build/dfw/sidplayer.bin (app.bin for XIP 0x02000120, an empty tune pack)
#                               build/dfw/sidplayer_xip.bin (+0x120 pad, for fm1-emu --bin)
#                               build/dfw/sidplayer_base.bin (no tune pack: tools/fwsc.py --dinghy appends one)
#   DOCK_BOOT=1 firmware/build_dinghy.sh -> build/dfw_boot/: the same for fm1-dock's boot slot (runs at
#                               0x02011000, no song store, its NOINIT clear of fm1-dock's start counter);
#                               fm1-dock's tools/hbboot.py make build/dfw_boot/sidplayer.bin --no-data makes
#                               the .dock (a standalone FM-1 SID on a real FM-1 with fm1-dock to come back to)
# Needs ext/ (tools/fetch_ext.sh: reSID 0.16). The tunes come after the image as a tune
# pack (tools/sidpack.py); without one it plays its own tune.
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
TC="${JIELI_TOOLCHAIN:-$(ls -d ../reference/toolchain/jieli-linux-toolchains-* 2>/dev/null | tail -1)}"
O=build/dfw
BASE=0x02000120
SLOT=0x8DFBC
LD=firmware/app_dinghy.ld
D=dinghy
HAL="-I$D/hal"
DEFS=
if [ "${DOCK_BOOT:-0}" = 1 ]; then          # fm1-dock's boot slot: flash 0x15000..0x93000 (tools/hbboot.py)
    O=build/dfw_boot
    BASE=0x02011000
    SLOT=0x7E000
    mkdir -p "$O/hal"
    sed -e "s/XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x8DFBC/XIP    (rx)  : ORIGIN = $BASE, LENGTH = $SLOT/" \
        -e "s/NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50/NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D40/" \
        firmware/app_dinghy.ld > "$O/app.ld"
    grep -q "ORIGIN = $BASE" "$O/app.ld" && grep -q "LENGTH = 0x3D40" "$O/app.ld" || { echo "app_dinghy.ld: not the layout this knows"; exit 1; }
    LD="$O/app.ld"
    # the PC limits from where the image starts (as tools/hbboot.py relink does to a Felucca tree)
    sed -e "s/_sstack_top\[\], _etext\[\];/_sstack_top[], _etext[], _start[];/" \
        -e "s/FM1_PC_LIMIT0_L = 0x02000120u;/FM1_PC_LIMIT0_L = (uint32_t)(uintptr_t)_start;/" \
        -e "s/re > rs ? rs : 0x02000120u;/re > rs ? rs : (uint32_t)(uintptr_t)_start;/" $D/hal/fm1_guard.h > "$O/hal/fm1_guard.h"
    [ "$(grep -c '_start' "$O/hal/fm1_guard.h")" -ge 3 ] || { echo "fm1_guard.h: not the layout this knows"; exit 1; }
    HAL="-I$O/hal -I$D/hal"
    DEFS=-DDOCK_BOOT=1
fi
L="$TC/pi32v2/lib/r3"
mkdir -p "$O"
python3 firmware/gen_assets.py "$O/assets.h"
CC="$TC/pi32v2/bin/clang -target pi32v2 -mcpu=r3 -mfprev1 -ffp-contract=off"
CXX="$TC/pi32v2/bin/clang++ -target pi32v2 -mcpu=r3 -mfprev1 -ffp-contract=off"
CF="-O2 -ffunction-sections -fno-builtin -ffreestanding -Wall -Wno-unused-function -I$TC/pi32v2/include"
$CC -c $D/crt0.S -o $O/crt0.o
$CC -c $D/hal/fm1_vec.S -o $O/fm1_vec.o
$CC -c $D/hal/fm1_isr.S -o $O/fm1_isr.o
$CC $CF $DEFS -DFELUCCA_ID='"FM-1_900"' -DAPP_VER='"1.0"' $HAL -I$D/src -I$D/gen -Isrc -Iplayer -I$O \
    -c firmware/fm1sid.c -o $O/fm1sid.o
$CC $CF -c src/sid.c -o $O/own_sid.o
$CC $CF -Isrc -c src/sidsynth.c -o $O/sidsynth.o
$CC $CF -Iext/chips -c player/psid.c -o $O/psid.o
$CC $CF -c player/gt2.c -o $O/gt2.o
$CC $CF -c player/gt2edit.c -o $O/gt2edit.o
OBJS="$O/crt0.o $O/fm1_vec.o $O/fm1_isr.o $O/fm1sid.o $O/own_sid.o $O/sidsynth.o $O/psid.o $O/gt2.o $O/gt2edit.o"
for f in sid voice wave envelope filter extfilt pot wave6581_PST wave6581_PS_ wave6581_P_T wave6581__ST \
         wave8580_PST wave8580_PS_ wave8580_P_T wave8580__ST; do
    $CXX $CF -fno-exceptions -fno-rtti -w -Iext/resid-0.16 -c ext/resid-0.16/$f.cc -o $O/$f.o
    OBJS="$OBJS $O/$f.o"
done
$CXX $CF -fno-exceptions -fno-rtti -Iext/resid-0.16 -c firmware/resid_glue.cc -o $O/resid_glue.o
"$TC/pi32v2/bin/ld" --gc-sections -T "$LD" -e _start $OBJS $O/resid_glue.o \
    --start-group "$L/libm.a" "$L/libc.a" "$L/libcompiler_rt.a" --end-group -o $O/sidplayer.elf
"$TC/common/bin/objcopy" -O binary -j .text $O/sidplayer.elf $O/text.bin
"$TC/common/bin/objcopy" -O binary -j .data $O/sidplayer.elf $O/data.bin
"$TC/common/bin/objcopy" -O binary -j .ram_text $O/sidplayer.elf $O/ram_text.bin
"$TC/common/bin/objdump" -d $O/sidplayer.elf > $O/sidplayer.dis
"$TC/pi32v2/bin/nm" $O/sidplayer.elf > $O/sidplayer.nm 2>/dev/null || "$TC/common/bin/nm" $O/sidplayer.elf > $O/sidplayer.nm
python3 - "$O" "$BASE" "$SLOT" <<'PY'
import sys
o, base, slot = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
nm = {l.split()[-1]: int(l.split()[0], 16) for l in open(f"{o}/sidplayer.nm") if len(l.split()) == 3}
text, data = open(f"{o}/text.bin", "rb").read(), open(f"{o}/data.bin", "rb").read()
rt = open(f"{o}/ram_text.bin", "rb").read()  # (the flash code, copied to RAM at boot)
img = text + b"\xff" * (nm["_data_load"] - base - len(text)) + data
img += b"\xff" * (nm["_rt_load"] - base - len(img)) + rt
img += b"\xff" * (-len(img) % 4)
assert nm["_start"] == base, hex(nm["_start"])
assert len(img) == nm["_pack"] - base, (len(img), hex(nm["_pack"]))
assert len(img) <= slot, f"image {len(img)} > its slot ({slot})"
ram = nm["_pool_end"] - 0x01C08000
assert nm["_pool_end"] <= 0x01C74000, f"RAM {ram}: into the stacks"
open(f"{o}/sidplayer_base.bin", "wb").write(img)
img += b"SIDPACK1" + bytes(8)              # an empty pack: the firmware's own tune
open(f"{o}/sidplayer.bin", "wb").write(img)
open(f"{o}/sidplayer_xip.bin", "wb").write(b"\0" * 0x120 + img)
print(f"{o}/sidplayer.bin: {len(img)} bytes of the {slot} slot at {base:#010x}; RAM {ram} of 442368 bytes")
PY
