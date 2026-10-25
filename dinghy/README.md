# Dinghy 0.3, the platform FM-1 SID runs on

[Dinghy](https://github.com/zvenson/dinghy) v0.3 (commit 9297300), `firmware/` as it is, for
`firmware/fm1sid.c`: boot, boot guard, the USB rescue (OCT- held at power-on), the installers' update path,
USB-MIDI and USB audio, the keys, knobs and LEDs, MASTER, the screen's driver, the crash screen. It is the
base [Scintilla](https://github.com/joakimeriksson/synthex-fm1) runs on, on real FM-1s. GPL-3.0-only
(LICENSE, LICENSING.md: Felucca's and SLOOP's code inside).

Changed here (nothing else):

- `hal/fm1_sys.h`: Scintilla's, which adds `fm1_cpu1_halt()` (hold a second core the firmware before may
  have left running).
- `src/platform.c`: `fm1_cstart` calls `fm1_cpu1_halt()` first (as Scintilla's); `FL_RANGE_OK` only when the
  app has not defined it (FM-1 SID adds its song store's flash, firmware/store.c).
- `src/usb.c`: every USB DMA buffer `aligned(64)` (with a 4-aligned `ep1tx` no reply reached the computer: the
  FM-1's transmit DMA appears to round its address down to 8; see the comment there).
- `src/dinghy.c` (the demo app) left out.
