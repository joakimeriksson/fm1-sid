# FM-1 SID as an fm1-dock module

[fm1-dock](https://github.com/joakimeriksson/fm1-dock) is one firmware for the M-VAVE FM-1 that holds
synths as modules in flash. Here FM-1 SID (`firmware/fm1sid.c`, all of it: the C64 tune player, the
GoatTracker player and editor, the SID synth) is built as one of them:

| | |
| --- | --- |
| `fm1sid_module.c` | the module's table: `render()` is the app's audio interrupt, `draw()` its main loop body |
| `dock_platform.h` | Dinghy's platform calls on fm1-dock's host table (the panel, LEDs, screen strips, time, MIDI) |
| `resid_glue.cc` | reSID 0.16 without an allocator or static constructors (a module has neither) |
| `mkpack.py` | the tunes as a C array in the module's own flash (`tools/sidpack.py`'s pack, the same bytes) |
| `module.cfg` | what fm1-dock's `tools/hbmod.py` builds: the sources, the own model's tables as constants |

```sh
tools/fetch_ext.sh                                   # reSID 0.16 and m6502.h into ext/
FM1_DOCK=../../fm1-dock dock/build.sh Commando.sid Wizball.sid:7 song.sng   # -> build/dock/fm1sid.dock
```

`FM1_DOCK` is fm1-dock's tree (its toolchain settings, `tools/hbmod.py` and `firmware/dock/hb_module.h`);
`DEJAVU_MONO` points at DejaVu Sans Mono where it is not in Debian's place. With no tunes the module plays
its own. A module has 272 KiB of flash and 272 KiB of RAM: about 64 KiB of tunes fit beside the code.
Songs saved in the editor go to fm1-dock's flash banks (`firmware/store.c`). Deploy with fm1-dock's
`tools/dock.py deploy build/dock/fm1sid.dock` or its page's console.
