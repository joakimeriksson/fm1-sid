# fm1-sid — a SID synth for the M-VAVE FM-1?

A feasibility study, with a working prototype: can the **M-VAVE FM-1** pocket synth
(JieLi AC791N, pi32v2 CPU at 240 MHz, see [the emulator](https://github.com/joakimeriksson/fm1-emu)) be turned into a synth built
on the **MOS 6581 / 8580 SID**, the Commodore 64's sound chip?

**Just want to use it?** Read the [user guide](#user-guide-fm-1-sid): playing C64 tunes and
GoatTracker songs on the FM-1, putting your own on it, and writing music with the GoatTracker
editor. The rest of this file is how it was built and measured.

**Where it lives:** this repository is also [fm1-emu](https://github.com/joakimeriksson/fm1-emu)'s
`fm1-sid/` (a submodule): its web page and some builds use the emulator next to it (`../web`, `../target`,
`../tools`). Clone fm1-emu with `--recurse-submodules` for all of it; the host tests (`make test`) and
the fm1-dock module (`dock/build.sh`) build from this repository alone. Licences: [LICENSING.md](LICENSING.md).

**On a real FM-1 it runs on [fm1-dock](https://github.com/joakimeriksson/fm1-dock)**, which owns USB,
the installers and the rescue: as FM-1 SID's own firmware (`web/sidplayer.fwsc`, `dock/build_solo.sh`:
fm1-dock with FM-1 SID and the sine, starting in FM-1 SID), or as a module deployed into an fm1-dock
with other synths (`dock/build.sh`). PSID and RSID tunes, GoatTracker songs, the editor, songs saved
in fm1-dock's flash banks, subtunes on ALGORITHM. Any .sid can go to a real FM-1 over USB without an install: "Play on my FM-1" on the page, or
fm1-dock's `tools/dock.py send tune.sid [--arg SONG]`, writes it into FM-1 SID's C64 memory as it arrives
and plays it (nothing in flash; `dock/send_test.py` checks it in fm1-emu). It has played on a real FM-1 as a module (Arkanoid at
full speed, 2026-10-24), and as its own firmware (installed, USB answering as it plays, the heaviest
audio block at 34 %, no overruns, 2026-10-25). The standalone builds (Felucca's and Dinghy's) are not for a real FM-1: their
USB replies never reach a computer.

## Summary

**Yes, it can be done.** Two things were studied:

1. **An own SID model** (`src/sid.c`, integer C) plus a synth layer that plays the chip the way
   C64 music does (`src/sidsynth.c`: 50 Hz frames, waveform tables, PWM and filter sweeps,
   sync / ring, hard restart). ~320 instructions per sample and chip on the FM-1, ~6 % of a core.
2. **A comparison with the ready-made engines** reSID 0.16, reSID 1.0 and reSIDfp: the same
   register stream through all of them, the cost measured on the emulated FM-1, the sound
   level-matched for listening.

Conclusion: **reSID 0.16 in "fast" mode is the right chip engine for the FM-1.** It costs 440
instructions per sample (8 % of a core per chip), sounds bit for bit the same on the FM-1 as
on the computer, and has waveform tables and filter curves measured on real chips. The own
model is cheaper but worse. reSID's high-quality modes (resampling, alias-free) clock the chip
cycle by cycle and cost 1-7 cores; reSID 1.0 and reSIDfp do not fit the FM-1 at all (8-21 MB
of filter tables per chip model, against 1 MB of flash in total). The synth layer (`sidsynth`)
is what is new here, and it stays on top of reSID. On the way, four bugs were found in the
FM-1 emulator (64-bit carry, flags, multiply-accumulate and push / pop masks) that made all
`double` arithmetic wrong; the fixes are in PR #31.

There is also a **SID player firmware** (`firmware/`): real C64 tunes (PSID and RSID) run their
own 6502 code on the FM-1's CPU, GoatTracker 2 songs (.sng) play by GoatTracker's own
playroutine straight from the file, the sound comes from reSID 0.16 (or the own model), and the
LCD shows the three voices, sync / ring and the filter. It runs in `fm1-emu` and as a web page
(`web/`, GitHub Pages) where the whole FM-1 is emulated and all of HVSC can be searched and
played; the tunes are fetched straight from hvsc.c64.org. The sound matches reSID on the
computer. As a synth (which its update package starts as) it has a step sequencer and stereo
effects after Felucca's: chorus, delay and reverb.

New: RSID tunes (Arkanoid, Great Giana Sisters, Game Over and others) run on a whole emulated
C64, GoatTracker 2 songs play exactly as in GoatTracker (the same SID registers, frame for
frame), and there is a **GoatTracker editor** on the FM-1 itself (SEQ in the player): patterns,
order lists, instruments, tables and the song's name, edited with the FM-1's knobs and keys (the
keys C4-D#5 type hex 0-F) while the song plays. Songs are saved in 16 flash slots and show up in
the player's list. How to use it is in the [user guide](#user-guide-fm-1-sid).

> **On a real FM-1 (2026-10-24, once)**: this version started, took MIDI over USB and showed
> the tunes, but they did not play, and the installers could not reach it; the FM-1 went back to
> M-VAVE's firmware with its own recovery (MvaveFM1Unbricker, from Linux). The cause is not
> found yet (in the emulator, now with USB, both work). Since then: **USB mode** (HOME held
> 5 s: everything off but USB, for the installers and to tell a busy CPU from a dead USB),
> MASTER as the volume and the buttons' lights. Do not give it out until it has played on a
> real FM-1.

## User guide: FM-1 SID

FM-1 SID turns the M-VAVE FM-1 into a Commodore 64. It plays C64 music (.sid files) and
GoatTracker 2 songs (.sng) through an emulated SID chip, and it lets you write your own
GoatTracker songs on the FM-1 itself and save them. It is also a SID synth with a step
sequencer and effects.

> The RSID tunes, the GoatTracker player and the editor are new. On the one real FM-1 they
> have been tried on, the tunes did not play ([above](#summary)); have a way back ready
> (step 4 below) before you install it.

### Try it without an FM-1

- **In the browser**: [joakimeriksson.github.io/fm1-emu/fm1-sid](https://joakimeriksson.github.io/fm1-emu/fm1-sid/)
  runs the whole FM-1 in the page, with FM-1 SID's own firmware (fm1-dock with FM-1 SID). Pick a
  tune, search all of HVSC, or drop your own .sid files and GoatTracker .sng songs on the page.
- **On a Mac with this repository**: build the emulator (`cargo build --release` in the
  repository's root) and open a package in its window:

  ```sh
  target/release/fm1-live --fwsc fm1-sid/web/sidplayer.fwsc --flash my_fm1_flash.bin
  ```

  `--flash` keeps the FM-1's flash in a file, so the songs you save are still there the next
  time. Esc quits. The computer keyboard plays the FM-1 ([the keys](#the-emulators-keyboard)).

### Install it on an FM-1: as an fm1-dock module

1. Install [fm1-dock](https://github.com/joakimeriksson/fm1-dock/releases/latest) (its own
   installer, over USB; the way back to any firmware stays open).
2. Get `fm1sid.dock`: from fm1-dock's release (FM-1 SID with its own tune), or build one with
   your tunes: `dock/build.sh TUNE.sid[:SONG] SONG.sng ...` ([dock/](dock/README.md)).
3. Deploy it: `uv run --with python-rtmidi tools/dock.py deploy fm1sid.dock` in fm1-dock's tree,
   or drop it on [the console](https://joakimeriksson.github.io/fm1-dock/#console) (Chrome, Edge).
4. Hold **HOME** for 1.5 s and pick **FM-1 SID**. fm1-dock starts it at power-on from then on.

### The standalone package (not yet for a real FM-1)

1. Get the update package: `sidplayer.fwsc` from the page above (or `fm1-sid/web/` here), or
   make one with your own tunes ([below](#your-own-tunes-on-the-fm-1)).
2. Install it like any community firmware package (.fwsc), with the installer you use for
   those.
3. **To install something else later**, run its installer: FM-1 SID answers the installers as
   Felucca does (its update code), so they find it and install through it. If one does not,
   hold **HOME** for 5 seconds (or while switching on): **USB MODE**, the sound and the player
   off and only USB running; then run the installer again. **HOME** leaves USB mode. If FM-1
   SID ever fails to start twice in a row, the third start is in USB mode. **OCT−** and
   **OCT+** held for 3 seconds go to the chip's own update mode ("UBOOT"), which only the
   recovery tools of step 4 can write to (from Windows or Linux, not a Mac).
4. **If nothing else works**, the FM-1 has its own recovery, outside any firmware: hold OCT−
   and OCT+ while switching it on, with the USB cable plugged in, and use M-flasher, the
   Firmware Hub or [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) with the
   official firmware (issues #22 and #23). FM-1 SID never writes the flash this needs.

The package from the page starts as the **SID synth**: it is silent until you play a key.
Press **HOME** for the **tune player**.

### The tune player

The screen shows the tune's name and author, the tune number, the time, the chip (6581 or
8580) and the CPU load; under that the three SID voices (waveform, gate, sync / ring, filter,
note, envelope, pulse width), the filter and an oscilloscope.

| Control | What it does |
|---|---|
| **PRESETS** or **SELECT** (turn), **OCT−** / **OCT+** | previous / next tune |
| **ALGORITHM** (turn) | the tune's subtune (a SID or GoatTracker song with several: "Song 2/11") |
| **PLAY** | pause / play |
| **EDIT** | the SID engine: reSID 0.16 (default) or fm1-sid's own model |
| **FX** | the effects page (below); **FX** again goes back |
| **HOME** | the SID synth (and back); held 5 s: USB mode (for installers) |
| **MASTER** | the volume |
| **SEQ** | the GoatTracker editor on the song that plays ([below](#the-goattracker-editor)) |

**The effects page** (FX): **KNOB1** chorus, **KNOB2** delay, **KNOB3** the delay time
(1/16, 1/8, dotted 1/8, 1/4), **KNOB4** reverb, **SELECT** the delay's feedback, **PRESETS**
the chip (6581 / 8580). The player starts dry (the tunes were mixed for the bare chip); turn
the knobs up for space.

What plays:

- **.sid tunes**, PSID and RSID: almost all of the
  [High Voltage SID Collection](https://www.hvsc.c64.org/). RSID tunes (Arkanoid, Great Giana
  Sisters, Game Over, ...) run on a whole emulated C64. Not: the few tunes written in C64 BASIC,
  and tunes for two or three SID chips.
- **GoatTracker 2 songs** (.sng), played by GoatTracker's own playroutine, exactly as
  GoatTracker plays them. GoatTracker's release has example songs in its `examples` folder;
  make your own in [GoatTracker](https://sourceforge.net/projects/goattracker2/),
  [sid-tracker](https://github.com/joakimeriksson/sid-tracker) or on the FM-1. Songs made for
  2x speed play at normal speed (the speed setting is not in a .sng).
- **Your saved songs**: the songs you save in the editor come after the package's tunes.

### USB-MIDI

Plugged into a computer, the FM-1 shows up as a USB-MIDI device called **FM-1 SID** (class
compliant: no driver on macOS, Windows, Linux, iOS). A `USB` in green on the player's top line
says a computer has it.

- **Notes** play the SID synth (in the synth mode: the polyphonic synth, or with the sequencer's
  page up the selected track; with REC on they are recorded as the keys are) and the editor
  (test notes on the cursor's channel; on PATTERNS with REC on they are written, as the keys
  write). A MIDI note plays at its own pitch, whatever the octave.
- **Program change** N picks tune N+1 in the player.
- **Installers** find it and install through it (Felucca's update code: their handshake and
  upgrade command `F0 22 24 35 7F F7`), the web installer and Felucca's `tools/fm1_install.py`.
- **The chip's update mode**: Felucca's "soft key" SysEx `F0 22 24 35 7D F7` restarts the FM-1
  into it, as holding OCT− and OCT+ does. From Python with mido:
  `python3 -c "import mido; mido.open_output('FM-1 SID').send(mido.Message('sysex', data=[0x22, 0x24, 0x35, 0x7D]))"`
  (the port's name can differ a little, e.g. `FM-1 SID MIDI 1`: `mido.get_output_names()`).

Not yet: songs in and out over SysEx, USB audio, MIDI out, and the TRS MIDI jack. The USB code
is Felucca's own driver and update code, which run on real FM-1s; in FM-1 SID they work in the
emulator (`fm1-live --usb`), but on the one real FM-1 tried so far the installers got no answer
(see [above](#summary)): if that happens, hold HOME 5 s for USB mode.

### Your own tunes on the FM-1

The tunes are built into the update package. With this repository (after
`tools/fetch_ext.sh` and `firmware/build.sh`, see [Ways to build it](#ways-to-build-it)):

```sh
tools/fwsc.py my.fwsc Commando.sid Arkanoid.sid:1 Dojo.sng Dojo.sng:3
```

- `:N` picks the song in a .sid (from 1), or the subtune in a .sng (from 1). Without it: the
  tune's own start song, the song's first subtune. List a file twice for two of its songs.
- `--first N` starts on entry N (from 0), `--chip 6581` or `--chip 8580` sets the chip for all,
  `--own` starts on the own SID model, `--synth` starts in the synth.
- About 360 KB of tunes fit (a .sid is 2-20 KB, a GoatTracker song 1-20 KB). Each file is
  checked first; one the FM-1 would not play is named, with the reason.

Then install `my.fwsc` as above. A new set of tunes is a new package to install.

### The SID synth

**HOME** switches between the player and the synth. The 27 keys play 12 SID instruments.
**PRESETS** / **SELECT**: the instrument; **OCT−** / **OCT+**: the octave; **KNOB1-4**:
cutoff, resonance, attack, release. **SEQ** opens the step sequencer: three tracks of 16 steps,
**PLAY** runs it, **REC** records (running: onto the nearest step; stopped: at the cursor,
which **SELECT** moves), **SCL** / **ENV** / **LFO** pick track 1 / 2 / 3, **PRESETS** its
instrument (track 3 starts as a drum kit), **ALGORITHM** the tempo, **GLO** clears a step,
**ARP** a track.

### The GoatTracker editor

A GoatTracker song is built from four things:

- **Patterns**: rows of notes, one pattern per channel at a time. A row has a note, an
  instrument number and a command with its data. Patterns are 1-128 rows long.
- **Order lists**: one per channel (the SID has three), the patterns in the order they play,
  with repeats and transposes. The list ends by jumping back to a restart position. A song can
  have up to 32 **subtunes**, each with its own three order lists.
- **Instruments**: an envelope (attack / decay, sustain / release) and where in the tables the
  sound starts.
- **Tables**: the **wave** table makes the sound (the waveform per frame, arpeggios), the
  **pulse** table sweeps the pulse width, the **filter** table the filter, and the **speed**
  table holds the vibrato and portamento speeds.

GoatTracker's own manual (`readme.txt` in its release) explains all of it in depth; the
editor here edits the same song, and it plays exactly as in GoatTracker.

**Open it**: press **SEQ** in the player. If the tune playing is a GoatTracker song, it opens
in the editor; if it is a .sid tune, you get a new, empty song. **HOME** leaves the editor
(press it twice if there are changes you have not saved: they are lost when you leave).

#### Controls on every page

| Control | What it does |
|---|---|
| **SEQ** | next page: PATTERNS → ORDERS → INSTRUMENT → TABLES → SONG |
| **REC** | edit on / off. Off, nothing changes and the keys just play notes. On, the red **REC** shows |
| **PRESETS** (turn) | move up / down |
| **SELECT** (turn) | move left / right |
| **ALGORITHM** (turn) | change the value under the cursor (with REC on) |
| note keys | play notes; with REC on, write them (patterns) or type hex digits (below) |
| **PLAY** | play / stop |
| **OCT−** / **OCT+** | the octave of the keys |
| **KNOB1** (turn) | the instrument the keys play and write |
| **LFO** | mute / unmute the cursor's channel |
| **SAVE** | save the song (into its slot, or the first free one) |
| **HOME** | leave the editor |
| **FX**, **EDIT** | the effects page, the SID engine, as in the player |

**Typing hex digits**: with **REC** on and the cursor on a hex number, the 16 keys from **C4**
(middle C) to **D#5** type **0** to **F**, in order, black keys included:

| Key | C4 | C#4 | D4 | D#4 | E4 | F4 | F#4 | G4 | G#4 | A4 | A#4 | B4 | C5 | C#5 | D5 | D#5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Digit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
| Emulator key | A | W | S | E | D | F | T | G | Y | H | U | J | K | O | L | P |

The top line shows the page, the instrument (`I01`), the octave (`O4`), the step (`S1`, on
the patterns page), **REC**, `PLY` while it plays, and `*` when there are unsaved changes. The
bottom line says which keys do what on the page, and shows messages.

#### PATTERNS

The three channels side by side, with each channel's pattern number and length above it
(`P05:40`: pattern 05, 40 hex = 64 rows). A row reads `C-4 01 305`: note C-4, instrument 01,
command 3 with data 05. `...` is an empty note, `---` a key off.

| Control | What it does |
|---|---|
| note key (REC on, cursor on a note) | write the note with the current instrument, move on by the step |
| hex keys (REC on, cursor on a number) | type the instrument (2 digits), the command (1) and its data (2) |
| **GLO** | clear the note (or the instrument, or the command and data) |
| **ARP** | key off (the note is released) |
| **SCL** / **ENV** | insert / delete a row |
| **KNOB2** | which pattern the cursor's channel shows |
| **KNOB3** | the pattern's length |
| **KNOB4** | the step: how many rows a note moves the cursor (0-16) |
| **PLAY** | play the three patterns over and over, from their first row |

While it plays, the cursor follows the music (up / down does nothing then; stop with **PLAY**).
You can still write: with REC on, a key writes at the row that plays.

The commands (data in hex):

| | Command | | Command |
|---|---|---|---|
| 1 | portamento up (speed table entry) | 9 | pulse table position |
| 2 | portamento down (speed table entry) | A | filter table position |
| 3 | slide to the note (speed table entry; 00 at once) | B | filter control (resonance and channels; 00 filter off) |
| 4 | vibrato (speed table entry) | C | filter cutoff |
| 5 | attack / decay | D | master volume (00-0F) |
| 6 | sustain / release | E | funk tempo (speed table entry: two alternating tempos) |
| 7 | waveform | F | tempo (03-7F, about the frames per row; 80 added: this channel only) |
| 8 | wave table position | 0 | nothing |

#### ORDERS

The subtune's three order lists, one column per channel, the position numbers on the left.
An entry is a pattern number (00-CF), a repeat (D0-DF: the next pattern 1-16 more times) or a
transpose (E0-EF down 16-1 semitones, F0-FE up 0-14). After the last entry, `Rxx` is the
restart: where the list jumps back to.

| Control | What it does |
|---|---|
| hex keys (REC on) | type an entry, two digits; on the `R` line, the restart (or a new entry at the end) |
| **SCL** / **ENV** | insert (a copy of the entry) / delete an entry |
| **GLO** | the restart here |
| **KNOB2** | the subtune (00-1F; a new one starts empty: give each of the three channels an entry with **SCL**, then it plays) |
| **PLAY** | play from the cursor's position |

Moving the cursor here also changes the patterns the PATTERNS page shows: those at this
position.

#### INSTRUMENT

**KNOB1** picks the instrument (01-3F). Its fields, in hex:

| Field | What it is |
|---|---|
| Attack/Decay, Sustain/Rel. | the SID envelope: one hex digit each (attack, decay, sustain level, release) |
| Wavetable | where the sound starts in the wave table (its row; 00: none, then the note gets no pitch) |
| Pulsetable, Filtertable | where its pulse sweep / filter program starts (00: none) |
| Vibrato par. | a speed table entry for the vibrato (00: none) |
| Vibrato del. | frames before the vibrato starts |
| HR/Gatetimer | frames before the next note that the gate goes off (hard restart); 80 added: no hard restart, 40 added: no gate off |
| 1st wave | the waveform of a note's first frame (09: the usual hard restart; 00: none) |
| Name | 16 letters: **SELECT** moves along it, **ALGORITHM** changes the letter |

With REC on, two hex digits set a field and move to the next. With REC off the keys play the
instrument, so you hear each change.

#### TABLES

The four tables side by side, rows 01-FF, each row two hex numbers (left, right). **SELECT**
moves along the digits, typing moves on by itself, and after the last digit of a row to the
next row. Rows a playing channel is on show green.

| Table | Left | Right |
|---|---|---|
| WAVE | the waveform: 11 triangle, 21 sawtooth, 41 pulse, 81 noise (these with the gate on); 01-0F wait that many frames; FF jump | the note: 00-7F up from the played note, 80 the same, 81-DF a fixed note; after FF: the row to jump to (00: stop) |
| PULSE | 8x-Fx: set the width to xyy; 01-7F: sweep for that many frames; FF jump | the low byte (set), or the speed (sweep) |
| FILT | 90 / A0 / C0: filter type low / band / high pass; 00: set the cutoff; 01-7F: sweep; FF jump | resonance and channels (type), the cutoff, or the speed |
| SPEED | vibrato: depth; portamento: high byte | vibrato: speed; portamento: low byte |

| Control | What it does |
|---|---|
| **SCL** / **ENV** | insert / delete a row (instruments, commands and jumps that point past it follow, as in GoatTracker) |
| **GLO** | clear the row |

#### SONG

The song's name, author and year (**SELECT** moves along, **ALGORITHM** changes the letter,
with REC on), its size, the editor's free memory, and the 16 **flash slots** with what is in
each (`>` marks the song's own slot).

| Control | What it does (on a slot) |
|---|---|
| **SAVE** | save the song into this slot (press twice to save over another song) |
| **GLO** | load the song in this slot (twice if there are unsaved changes) |
| **ARP** | delete the slot (twice) |
| **SCL** | a new, empty song (twice) |

**Saving**: **SAVE** on any other page saves into the song's own slot, or the first free one.
The sound stops for a moment while it is written. Saved songs stay when the FM-1 is off and
appear in the player after the package's tunes, where **SEQ** opens them again. The 16 slots
share about 290 KB of flash. **Installing a package again can wipe the saved songs**, and
there is no way yet to copy them to a computer.

#### Your first song, step by step

A new song's instruments have no wave table yet, so they make no sound. Make one first:

1. In the player, go to a .sid tune (for example Wizball) and press **SEQ**: a new song opens
   on the PATTERNS page.
2. Press **SEQ** twice for INSTRUMENT, then **REC**. Instrument 01 is selected. Type **0 9**
   (attack 0, decay 9), **0 0** (sustain 0, release 0) and **0 1** (the sound starts in wave
   table row 01). Each pair moves to the next field.
3. Press **SEQ** for TABLES. The cursor is on row 01 of WAVE. Type **2 1 0 0** (row 01:
   sawtooth with the gate on, the played note) and then **F F 0 0** (row 02: stop here).
4. Press **SEQ** twice to get round to PATTERNS. Still with REC on, play some keys: each writes
   a note with instrument 01 and moves down a row. Turn **KNOB4** to 2 or 4 to leave space
   between the notes, use **ARP** for a key off and **GLO** to clear.
5. Press **PLAY**: the patterns loop from the top. A pattern is 64 rows (40 hex), about 8
   seconds; turn **KNOB3** to make it shorter, for example 10 (16 rows). **PLAY** again stops.
6. For a longer song, go to ORDERS: channel 1 plays pattern 00, channel 2 pattern 01,
   channel 3 pattern 02, then starts again. Insert entries with **SCL** and type pattern
   numbers; turn **KNOB2** on PATTERNS to fill the new patterns.
7. Go to SONG, give it a name, and press **SAVE**. Press **HOME**: it is in the player's list.

For other sounds: a pulse wave is `41 00` in the wave table, and it needs a pulse width. Set the
instrument's Pulsetable to 01 and in the PULSE table write row 01 `88 00` (width 800), row 02
`20 20` (sweep up for 32 frames) and row 03 `FF 01` (back to row 01). A sustain level
(Sustain/Rel. `A0`) makes notes hold until their key off.

#### The emulator's keyboard

| Emulator key | FM-1 control |
|---|---|
| A W S E D F T G Y H U J K O L P ; ' | the note keys C4 to F5 (and the hex digits 0-F, A to P) |
| Z / X | OCT− / OCT+ |
| ← / → | SELECT (in the editor: left / right) |
| ↑ / ↓ | PRESETS (in the editor: up / down) |
| , / . | ALGORITHM |
| Space | PLAY |
| R | REC |
| 1 2 3 4 5 6 7 8 9 0 | FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ |
| mouse (drag or scroll) | the knobs, KNOB1-4 among them; click a button or key |
| Esc | quit (with `--flash`, the flash is saved to its file) |

#### When something is not right

- **A note makes no sound**: its instrument needs a wave table position, and the wave table a
  waveform there (see the first song). An instrument's 1st wave of 00 and no wave table is
  silent.
- **"the song's memory is full"**: the editor has about 60 KB for the patterns and order
  lists. Shorten patterns you do not use (patterns never written take no room).
- **"too big for the editor"**: the song does not fit into those 60 KB; it still plays in the
  player.
- **"the flash store is full"**: delete a slot on the SONG page.
- **Up / down does nothing on PATTERNS**: the song plays and the cursor follows it; stop it
  with **PLAY**.
- **Back to another firmware**: run its installer. If it does not find FM-1 SID, hold HOME for
  5 seconds (USB mode) and run it again.

## Short answer

Yes. A SID chip costs **~8 % of one 240 MHz core with reSID 0.16 (fast mode)** and ~6 % with
the model written here, measured on the emulated FM-1 and bit-identical to the host builds.
**Use reSID 0.16 for the chip** (real combined-waveform tables, measured filter curves, years
of use in VICE / sidplay); keep `sidsynth` (the instrument driver) on top. The high-quality,
alias-free modes of reSID and the newer engines do not fit the FM-1 (see the comparison).
The player firmware built on it is also a synth (a step sequencer, stereo chorus, delay and
reverb after Felucca's), which its update package starts in.

## Engine comparison: own model, reSID 0.16, reSID 1.0, reSIDfp

The demo pieces' register streams are recorded (`host/demo.c record`: every write with its
sample position) and replayed on every engine, each clocked to the exact cycle of each write
(`host/replay.cc`). On the FM-1 the bench workload's stream replays through reSID 0.16 built
for pi32v2 (`fm1/bench_resid.cc`, `fm1/run_resid.py`).

| engine | FM-1 instr / sample | of a 240 MHz core | RAM | host ms per s of audio | verdict |
|---|---:|---:|---|---:|---|
| own (`src/sid.c`) | 316 | 5.8 % | ~1 KB RAM + 32 KB tables | 2.4 | fits, lowest fidelity |
| reSID 0.16 fast | 440 | 8.1 % | 17 KB RAM + 128 KB tables | 3.1 | **fits: use this** |
| reSID 0.16 interpolate | 5,562 | 102 % | 17 KB RAM + 128 KB tables | 27.9 | too heavy |
| reSID 0.16 resample | 39,226 | 721 % | 167 KB RAM + 128 KB tables | 133 | far too heavy |
| reSID 1.0 resample | – | – | 8.3 MB of filter tables per chip model | 117 | does not fit |
| reSIDfp resample | – | – | ~21 MB of filter tables per chip model, `std::thread` | 72 | does not fit |

Per SID chip (3 voices + filter), 44.1 kHz. Host: one core of an Intel Xeon at 2.1 GHz.
FM-1 outputs equal the host's bit for bit for every row with a number. Tables are constant
after init, so they can live in flash (XIP) instead of RAM: reSID 0.16's 128 KB of wave
tables and the own model's 32 KB fit easily. The FM-1 has 1 MB of flash in all (~568 KB for
the app), so the 8-21 MB filter tables of reSID 1.0 and reSIDfp do not fit there either.

What the listening shows (`tools/compare.sh`, `tools/listen.py`, level-matched to -20 dBFS):

- **Aliasing**: the own model and reSID fast both sample the waveforms at 44.1 kHz; high notes
  get folded partials (spectrograms of the high chord: a dense floor between the harmonics).
  The resample modes are clean, at 15–90x the cost.
- **6581 filter**: reSID 1.0 / reSIDfp model its non-linearity; reSID 0.16 has the measured
  cutoff curve without distortion; the own model has an approximate curve.
- **Combined waveforms**: reSID plays tables sampled from real chips; the own model guesses.

Next quality step that fits: reSID 0.16 fast at 2x the output rate with a half-band
decimator (about 2x the cost, ~16 % of a core per chip) to push most aliasing out of the
audible band; to be measured.

### Real C64 tunes

`host/sidlogger.cpp` runs a tune on libsidplayfp's C64 emulation (PSID and RSID, multi-SID)
and logs every SID write with its C64 cycle (sidlog v2); `host/replay.cc` then plays that log
on every engine, each clocked to the cycle of each write. libsidplayfp 2.6.0 is built in
`ext/` with a 24-line patch (`tools/libsidplayfp-2.6.0-sidlog.patch`) that calls a hook on
each SID write. `tools/scan_tunes.py` ranks HVSC tunes by how much they use hard sync, ring
modulation and the filter (from their register streams); `tools/tunes.sh` fetches the chosen
ones (they and their renders stay in `build/`, not in the repo) and renders each with
`sidplayfp` as a reference. `tools/refcheck.py` aligns ours with the reference: the tempo holds
and the log-spectra correlate ~0.8 with our reSIDfp replay; the rest is engine settings
(filter curve) and where in the frame the player runs.

### Emulator fixes found on the way (ai-smarthome PR #31)

reSID's `double` setup came out wrong on the emulator; tracing it found four gaps in
`fm1-core`: no arithmetic flags (64-bit add lost its carry, 64-bit subtract always
borrowed, 64-bit compares read psr = 0), the add / subtract-with-carry classes fitted to the
corpus registers only, the 64-bit multiply-accumulate `fc e1`, and rets + register-mask
push / pop for masks the corpora never had. The fixes, their probes and notes are in
[PR #31](https://github.com/joakimeriksson/ai-smarthome/pull/31) (branch
`fm-1-flags-carry`). Until it is merged, `fm1/run_resid.py` needs an `fm1-emu` built from
that branch; `fm1/run.py` (the own model, integer only) runs on either.

## The SID player firmware (`firmware/`, `player/`)

Path C below, built: C64 tunes played by their own 6502 code on the FM-1's CPU, with the
chip drawn on the LCD.

- `player/psid.c` is the C64 side of a PSID tune around its own NMOS 6510, `player/cpu6510.h`:
  an instruction per dispatch, every access at its cycle in the instruction (all 256 opcodes,
  the undocumented ones and NMOS decimal mode included): 64 KB RAM, `$01` banking, the CIA
  timer latch (`$DC04/05`) for the play rate, `$D012` raster reads, OSC3 / ENV3 reads. It runs
  `init` and `play` as subroutines and hands each SID write to a callback with its C64 cycle.
- **RSID tunes** (Arkanoid, Game Over, Great Giana Sisters: about 7 % of HVSC) need the C64
  around the 6502, and for them the same file keeps a whole one running: both CIAs' timers
  (CIA 1 -> IRQ, CIA 2 -> NMI, powering up at `$FFFF` as the chips do), the VIC's raster
  counter, raster interrupt and bad lines (the CPU stopped at its first read from cycle 11 to
  cycle 54, as libsidplayfp's VIC drops BA; its writes go on), the `$01` banking and, in the KERNAL's place, a stub with its interrupt entries and exits
  (`$FF48`, `$FE43`, `$EA31` / `$EA81` / `$FEBC`; RTS elsewhere). Init is entered as
  libsidplayfp's driver enters it (CIA 1 timer A at 50 Hz, interrupts on, an idle loop to return
  to) and may never return; each `psid_play` runs the machine on for a PAL frame. A `JMP` to
  itself is skipped a whole `JMP` at a time up to the next change (a bad line's stop runs it), and
  the chips run behind the CPU: they catch up when it touches the VIC or a CIA and at the cycles a
  line changes or a timer underflows (cycle for cycle the same as stepping them every cycle), the
  interrupts polled between instructions as the 6502 does (a line up two cycles before). Not
  covered: RSID tunes in C64 BASIC (refused: no BASIC ROM), tunes that need cycle-exact CIA
  quirks (Jammer's *Foregone Destruction* runs code out of CIA 1's timer registers), keys.
  The core replaced floooh's cycle-stepped `m6502.h` (2026-10-24: a dispatch per clock was ~80 %
  of an RSID tune's time on the FM-1, and Arkanoid fell behind): 2.6x faster on the host, RSID
  tunes keep up on the FM-1 (fm1-emu strict: Arkanoid, Giana, Game Over, *Vicious SID 2*'s 8 kHz
  NMI samples). Every PSID tune's register stream is bit for bit the old one's; against
  libsidplayfp the RSID tunes score as before or better (*Vicious SID 2* 0.29 -> 0.55,
  *Foregone Destruction* 0.26 -> 0.52). Checked: every opcode's cycles (the NMOS table), and
  each opcode from random states against `m6502.h` (the same but for SHA / SHX / SHY / TAS, whose
  behaviour differs from chip to chip, and BRK / PHP pushing B set, as the 6510 does).
- Checked against libsidplayfp given the same KERNAL stub (`build/sidlogger` with
  `FM1SID_KERNAL=`, the image from `build/psidlog --kernal`; without a KERNAL its `$EA31` is a
  BRK): over 20 s, log-spectrum correlation of the reSID renders 0.91 for Arkanoid's title tune
  (its sampled drums: the write intervals come out identical), 0.94 Giana's title tune (digi
  drums from CIA 2 NMIs), 0.91 Game Over, 0.95 / 0.99 Arkanoid's alternative drums / song 3;
  0.84-0.99 for 15 of 22 RSID tunes drawn at random from HVSC (4 in BASIC, 2 silent in both, 1
  the CIA trick above). PSID tunes play as before (60 random tunes: the same register stream,
  byte for byte).
- Cost: a busy RSID tune (Arkanoid's title, whose main loop polls a flag) keeps the 6502 running
  every cycle: in `fm1-live` the FM-1's core is 62 % busy with it (38 % idle) against 20 % for a
  PSID tune; tunes whose init returns, or that loop in a `JMP` to themselves, cost less
  (Arkanoid song 3: 58 % idle). The web page's emulator runs it at 1.8x real time in node on an
  M1 (Wizball: 4.4x).
- **GoatTracker 2 songs** (`.sng`, GTS3-GTS5): `player/gt2.c` is GoatTracker v2.76's
  playroutine (`gplay.c`, Lasse Öörni and contributors, GPL-2.0-or-later) kept statement for
  statement, without the editor's parts. GoatTracker holds a song in fixed arrays (208 patterns
  of 128 rows, 32 order lists: about 135 KB, where the firmware has 7 KB free); here the song
  stays in flash as the file it is. Loading checks it and makes an index (where each order list
  and pattern starts, how long it is), copies the instruments and the four tables (with the
  older versions' conversions), and the playroutine reads patterns and order lists through the
  index: 7.4 KB for all of it (with the editor's play state), in the PSID player's memory (one or the other plays). What the
  file leaves out reads as GoatTracker's cleared song does (an empty pattern is 64 rests). It
  plays as GoatTracker's editor does at its default settings (1x speed, hard restart `$0F00`,
  the pulse and realtime optimizations), the 25 registers every frame from `$18` down to `$00`,
  14 cycles apart, as its sound code writes them; a song's own .sid export settings are not in
  the .sng (2x-speed songs play at 1x). The subtunes are its songs.
- Checked against GoatTracker itself: `tools/gt2check.sh` (`make gt2check`) runs sid-tracker's
  `gt2dump` (GoatTracker's `gplay.c` and `gsong.c` compiled unmodified) and `build/gt2log` on
  every subtune and compares the registers frame by frame: the 14 example songs of
  GoatTracker 2.76's release and sid-tracker's 3, 30 subtunes, the same for 10 minutes each (or
  to the frame where the song stops itself). The loader takes damaged files without reading
  outside them (4500 cut or scrambled files under AddressSanitizer / UBSan, 2000 frames each);
  `tools/sidpack.py` and `web/pack.js` check a file the way it loads it (the same verdict on
  1000 damaged files). Cost on the FM-1: 83 % idle in `fm1-live` with *Unleash the Fury*
  (the reSID rendering is most of the rest); the firmware grew 6 KB.
- `player/psidlog.c` (`make build/psidlog`) runs the same code on the host and writes a
  sidlog v2, so `host/replay.cc` and `tools/refcheck.py` can hold it against libsidplayfp's
  stream: Sanxion 0.979, Roland's Rat Race 0.888, Contaxia 0.915 log-spectrum correlation
  against the `sidplayfp` render.
- `firmware/main.c` (GPL-3.0-only: it is built on Felucca's HAL, startup and ISR code in
  `ext/Felucca`) plays them. The main loop runs the play routine up to three frames ahead
  and queues the writes with their cycle; the audio interrupt (ALNK0, 128-frame half buffers
  at 44117.6 Hz) clocks reSID 0.16 fast (`firmware/resid_glue.cc`) to each write's cycle, or
  renders the own model to the sample. The LCD shows each voice (waveform, gate, note,
  envelope, pulse width; SYNC / RING badges), the filter (mode, routing, cutoff, resonance),
  an oscilloscope and the CPU load.
- Keys: PRESETS / SELECT encoder or OCT− / OCT+ = tune, ALGORITHM = its subtune, PLAY = pause, EDIT = engine (reSID /
  own model), FX = the effects page (below), SEQ = the GoatTracker editor (below), MASTER = the
  volume (the pot read from the main loop, its square the level, as Felucca's). Keys and
  buttons light while held; PLAY, REC, FX and SEQ also while playing, recording, on the FX page,
  in the editor; SCL / ENV / LFO the sequencer's track, LFO a muted channel in the editor. Every
  other button and key that does something on the page glows dim (Felucca's glow,
  `fm1_led_dim`, ~1/30 of lit), so the black FM-1's panel can be found in the dark. HOME held 5 s (or at power-on): USB mode (below). OCT− and OCT+ held 3 s: the
  chip's update mode.
- **Effects** (`firmware/fx.c`, after Felucca's `src/fx.c`, GPL-3.0-only): a stereo chorus (two
  taps swinging against each other), a tempo delay whose right side answers 1.5 times later
  (1/16, 1/8, dotted 1/8 or 1/4 of the tempo, at most 0.49 s; low-passed repeats) and Felucca's
  ROOM reverb (4 damped combs, 2 allpasses). The chip is mono, the FM-1's output stereo: the
  chorus and the delay give it width. FX opens their page: KNOB1 chorus, KNOB2 delay, KNOB3 its
  time, KNOB4 reverb, SELECT the delay's feedback, PRESETS 6581 / 8580; FX again leaves it. One
  setting per mode: the synth starts with a little of each, the player dry (its output is then
  the chip's samples, bit for bit as without the effects). They always run, like sends, and
  take the audio interrupt from ~10 % to ~14 % of the core; their buffers are 78 KB of RAM.
- **Synth mode** (HOME switches): the 27 note keys play `src/sidsynth.c`'s 12 instruments on
  the same chip and engine. The driver's register writes go into the same cycle-stamped
  queue as a tune's: `sidsynth` has an optional write / envelope hook for another engine
  (`sidsyn_t.write`, `.env`, `sidsyn_tick`; without it nothing changes, same bench hash). Its
  50 Hz frames run in the audio interrupt, each at its own cycle, and key presses reach it
  through a small command queue at the start of the next half buffer (2.9 ms). PRESETS /
  SELECT pick the instrument, OCT- / OCT+ the octave, KNOB1-4 cutoff, resonance, attack and
  release; PLAY sends all notes off. Pack flag bit 3 boots straight into it.
- **Step sequencer** in the synth mode: three tracks of 16 steps, each a `sidsynth` part on
  its own SID voice with its own patch (`sidsyn_set_voice`; track 3 starts as a drum kit
  where a note's pitch class picks kick, snare or hat). The tracks share the chip's one filter
  the way C64 music does: routing bits merge, cutoff, resonance and mode come from the first
  track whose patch has a filter; sync and ring take the voice before as their source. Steps
  fire in the audio interrupt at their own cycle, between the driver's 50 Hz frames. SEQ shows
  the pattern on the LCD, PLAY runs it, REC records (running: to the nearest step; stopped: at
  the cursor, which SELECT moves), SCL / ENV / LFO pick track 1-3, PRESETS its patch,
  ALGORITHM the tempo, GLO clears a step, ARP a track. It starts with a demo pattern; nothing
  is saved yet.
- **GoatTracker editor** (`firmware/tracker.c`; SEQ in the player): the GoatTracker song that
  plays opens in it (a .sid: a new song), moved into RAM by `player/gt2edit.c` (60 KB of the
  PSID player's memory: the order lists and patterns each as long as they are, one after the
  other; a song that has not got a pattern takes no room for it), and GoatTracker's own
  playroutine plays it while it is edited, with GoatTracker's editor ways to play: a pattern
  over and over, from an order list position, test notes from the keys, channels muted. Five
  pages as GoatTracker's, SEQ goes round them: PATTERNS (the three channels, the row the song
  plays followed), ORDERS (a subtune's lists, repeats and transposes, the restart),
  INSTRUMENT (its nine bytes and name), TABLES (wave, pulse, filter, speed side by side) and
  SONG (the texts, the flash slots). The FM-1's controls: PRESETS up / down, SELECT left /
  right (the emulator's arrow keys), ALGORITHM the value at the cursor, KNOB1 the instrument, KNOB2 the pattern (on ORDERS
  the subtune), KNOB3 its length, KNOB4 the step; REC edit on / off (off, the keys only play);
  with REC on, on a hex digit, the 16 keys from C4 to D#5 type 0-F; GLO clear, ARP key off,
  SCL / ENV insert / delete (rows, entries, table rows), LFO mute, PLAY play / stop, SAVE save,
  HOME leave. The changes follow GoatTracker's editor: rows in and out of a pattern as
  `gpattern.c`, order list entries and the restart as `gorder.c`, table rows as `gtable.c`
  (the pointers into the table from instruments, wave table commands, patterns and its own
  jumps follow), and the .sng it writes as `gsong.c`'s.
- Checked on the host: `build/gt2edtest` (`make gt2edtest`) saves each song as loaded (all 17:
  the file again, byte for byte), moves it into RAM (it saves and plays the same), then makes
  2000 random edits while it plays (rows, lengths, order lists, tables, instruments, new
  subtunes) and saves it: the .sng loads back to the same song, saved again the same bytes,
  playing the same registers as the edited one (5 seeds x 3000 edits, AddressSanitizer /
  UBSan). The editor's ways to play against GoatTracker's own `initsongpos` (a copy of
  sid-tracker's gt2dump started that way): 42 of 42 runs the same, frame by frame. In
  fm1-emu (`fm1-live --shot` with scripted keys): notes, hex digits, order list entries typed,
  every page drawn, a song saved in a slot, found again after a power cycle and played from
  the player's list; fm1-dock's `tests/trio_test.py` with the boot slot build.
- **Songs in flash** (`firmware/store.c`): 16 slots in the free end of the firmware's own app
  slot (from the first 4 KB sector after the image and its pack to 0x92000; about 290 KB with
  few built-in tunes), flash no other firmware keeps anything in: Felucca's store and fm1-dock's
  table and modules are above 0x93000. A song is a record of whole sectors, a head (name,
  length, CRC-32, slot, save number) and the .sng; the head is programmed last, and a slot's
  older record is erased only once the new one reads back right, so a save cut off keeps the
  song before it. The store is read with the flash's own read command, a page at a time, not
  through the XIP window: FM-1 SID leaves the SFC's settings (its plain window) as the boot set
  them, so no firmware installed after it can inherit a changed one. A saved song is copied
  into RAM (the end of the PSID player's memory) to play or to edit, and joins the player's
  list after the pack's tunes. The audio stops while a song is written (erase and program run
  from RAM with the flash unmapped and interrupts off, Felucca's `hal/fm1_flash.h`). Installing
  a package rewrites the slot: the songs go with it. The boot slot build (`DOCK_BOOT`) has no
  store: the slot's flash is fm1-dock's to give.
- **Not bricking an FM-1** (the published FM-1 SID had no USB: installed, it played, but no
  installer could reach the FM-1 again until people found the FM-1's own recovery, #22, #23):
  - USB-MIDI and Felucca's update code (`ext/Felucca/firmware/src/ota.c`, copied unchanged by
    `firmware/build.sh`, with `src/ota_hw.c`'s hooks in `firmware/main.c`): the installers'
    handshake gets the identity FM-1_900, their upgrade command a session that stages the
    package's update loader in Felucca's OTA area (0xE0000..0xE5000) and restarts into it once
    the installer says "success"; anything else leaves FM-1 SID as it was. The soft key (7D):
    the chip's update mode. A staged loader left over is erased at boot, as Felucca does.
  - **USB mode** (`usb_mode`): HOME held 5 s, HOME held at power-on, or the third of three
    starts in a row that die in their first 30 s: the audio interrupt stopped, no player, only
    USB (the TIMER5 interrupt) and its requests in the main loop; the screen counts USB packets
    and handshakes. HOME leaves (where it was). If USB mode dies early too, the next start is
    the chip's update mode. OCT− and OCT+ held 3 s: the chip's update mode (only the recovery
    tools write there, from Windows or Linux).
  - Flash writes only inside the store and the OTA area, checked in the RAM code that writes
    (`FL_RANGE_OK`; none at all in the boot slot build): the bootloader, the image and every
    other firmware's flash are out of reach, so the FM-1's own recovery (OCT− and OCT+ held while
    it is switched on, USB plugged in: M-flasher, the Firmware Hub, MvaveFM1Unbricker) stays.
  - No global setting changed that outlives a reset: the SFC's plain window is left alone.
  - On a real FM-1 (2026-10-24): installed (through Scintilla's installer), it enumerated as
    "FM-1 SID" and took MIDI (a program change switched the tune), but played no tunes and
    answered no handshake; holding OCT− and OCT+ took it to the chip's update mode, and
    MvaveFM1Unbricker (Linux) put M-VAVE's V15 back. In fm1-emu with USB both work, also with
    the SIE 50x slower; at half the clock Arkanoid stops as it did there, Dojo plays on. Not
    found yet. Try both ways back on one FM-1 before giving it out.
- The tunes ride after the firmware in flash as a **tune pack** (`firmware/main.c` reads it at
  the image's end, `_pack` in `firmware/app.ld`): "SIDPACK1", a count, the first entry and two
  flags (own model; chip 6581 / 8580 for every tune), then offset, length and song per entry.
  `tools/sidpack.py` makes one from .sid and .sng files, the web page builds the same in
  JavaScript (`web/pack.js`). Without a pack the firmware plays its own tune.
- `tools/housetune.py` writes that tune, *Pocket Sync* (31 s, PSID, BSD-2): the music is
  computed in Python frame by frame as SID register writes (bass and drums through a resonant
  low-pass, arpeggios, a lead that goes plain, hard-synced, then ring-modulated) and a 60-byte
  6502 routine plays one frame of writes per call.
- **USB-MIDI**: Felucca's USB driver (`ext/Felucca/firmware/src/usb.c`, the one that runs on
  real FM-1s in Felucca, the update loader and SLOOP / Dinghy's family; `firmware/build.sh`
  copies it with one change, the product name "FM-1 SID", and checks the change took), included
  by `firmware/main.c` without its CDC console and USB audio, with the update code (above). Its
  SIE is polled from the TIMER5 ISR at 2 kHz (Felucca defers the poll while its audio ISR runs;
  FM-1 SID does not); MIDI in waits in its ring for the main loop, where notes go to the synth
  or the editor and a program change picks the tune. In the emulator: `fm1-live --usb` (the
  FM-1's USB controller and a host, fm1-emu's `crates/fm1-soc/src/usb.rs`): it enumerates as
  "FM-1 SID (emu)" and Felucca's `fm1_install.py --info` finds it; headless,
  `FM1_USB_SEND='3000:F0 00 32 45 00 00 00 40 7F F7'` with `--shot` sends the handshake and
  prints the answer.
- `firmware/build.sh` makes `build/fw/sidplayer.bin` (242 KB of the 568 KB app slot: players, the editor,
  reSID with its wave tables in flash, the own model, a font and the own tune; 426 KB RAM of
  432 KB with the effects' 78 KB),
  `sidplayer_xip.bin` for `fm1-emu --bin`, and copies the image without a pack to
  `web/sidplayer.bin` (committed, since CI has no JieLi toolchain).
- The main loop sleeps (`idle`) until the next interrupt. On the FM-1 that saves power; in
  the emulator it lets idle skipping jump ahead, which made the web build 6x faster.
- Measured in `fm1-emu`: the audio interrupt takes ~9.5 % of the 240 MHz core with reSID, and
  its output matches host reSID fed the same stream (correlation 0.984). It needs PR #31's
  emulator fixes; on master the CPU halts.

- `tools/fwsc.py` packs it as an FM-1 update package, `web/sidplayer.fwsc`: Felucca's update
  loader (built here) and `tools/fm1pkg_make.py`, with three Apache-2.0 files of JieLi's AC79
  SDK (`tools/fetch_ext.sh` fetches them into `ext/ac79sdk` and checks Felucca's hashes; the
  page ships `web/LICENSES/Apache-2.0.txt`). The app is the image with a pack of its own tune that starts it in the synth mode (silent
  until a key is played; HOME switches to the player). `../web/fwsc.js` unpacks exactly that app from it, and the
  emulator boots it like the community packages.
- **Your own tunes on an FM-1**: `tools/fwsc.py OUT.fwsc A.sid B.sid:3 C.sng ...` makes the
  package with those tunes and GoatTracker songs instead (same options as `sidpack.py`, plus
  `--synth` to start in the synth mode). They live in the firmware's flash, after the image: up
  to about 360 KB of them (a GoatTracker song is 1-20 KB). The firmware has no way to take tunes
  while it runs, so a new set is a new package to install. Each file is checked the way the
  firmware loads it (`psid_load`: a PSID header, the data inside the file; `gt2_load`: every
  part inside the file and inside GoatTracker's limits).
- **In fm1-dock's boot slot**: `DOCK_BOOT=1 firmware/build.sh` builds the same firmware to
  run from fm1-dock's boot slot (flash 0x15000..0x93000, XIP 0x02011000: 504 KiB, about 298 KB of
  it for tunes), `tools/sidpack.py --boot` adds the tunes, and fm1-dock's
  `tools/hbboot.py make OUT.bin --name FM1SID --no-data` makes the boot file. It writes no flash,
  so fm1-dock's deployed modules (CS-1, Scintilla, ...) stay beside it; power-on starts whichever
  was picked last, and HOME held at power-on is always fm1-dock, with its USB. Tested in fm1-emu
  with fm1-dock's `tests/trio_test.py`.
- **On real FM-1s**: people installed the first version (no USB) and it started and played
  (issues #22, #24), but no installer could reach it again; they got back with M-flasher and
  OCT− + OCT+ held at power-on (#22). This version answers the installers itself, and has USB
  mode (HOME held 5 s); on the one FM-1 it has been on so far it did not (above). In fm1-dock's
  boot slot, HOME at power-on is fm1-dock's: USB mode is HOME held 5 s there.

```sh
tools/fetch_ext.sh                            # engines, m6502.h, Felucca
firmware/build.sh                             # build/fw/sidplayer*.bin, web/sidplayer.bin
tools/fwsc.py                                 # web/sidplayer.fwsc, the update package
tools/fwsc.py my.fwsc A.sid B.sid:3 C.sng     # the update package with your tunes and songs
DOCK_BOOT=1 firmware/build.sh && tools/sidpack.py my_boot.bin --boot A.sid B.sid:3   # for fm1-dock
tools/sidpack.py build/fw/my_xip.bin --xip --first 1 A.sid B.sid:3   # a firmware with tunes
../target/release/fm1-emu --bin build/fw/my_xip.bin --entry 0x02000120 \
    --ms 3000 --wav build/fw/out.wav --keys "t:PRESET:+1,t:EDIT" --snap 2500  # LCD -> work/
tools/webpage.sh [..]                    # the web page with the emulator -> build/pages
```

## The web page (`web/`, GitHub Pages)

`web/` is the player as a page: the FM-1's front panel (the emulator's own drawing code, as on
`../web`), the emulated machine in an AudioWorklet booting `sidplayer.bin` plus a tune
pack the page builds from its playlist, and around it a player:

- **Playlist**: picks chosen for sync, ring modulation and filter work (`web/tunes.json`, from
  `tools/picks.py`: the HVSC path, header fields, the song's length from HVSC's
  `Songlengths.md5`, and tags measured from each tune's own register stream with
  `build/psidlog`), then *Pocket Sync*.
- **All of HVSC**: the page fetches `Songlengths.md5` (5 MB, all 61,157 paths) from
  hvsc.c64.org when asked and searches it; a tune is fetched when picked, added to the
  playlist, and the machine reboots on the new pack. "Surprise me" draws a random tune (not
  one in C64 BASIC).
  hvsc.c64.org sends `Access-Control-Allow-Origin: *`; no tune is hosted with the page.
- **Your files**: .sid files dropped on the page or picked join the playlist, locally.
- **SID synth**: the page's mode switch (HOME on the panel) turns the FM-1 into the synth; the
  Instruments tab lists the 12 patches, the computer keyboard (A W S E D ... ; ') and a MIDI
  keyboard over Web MIDI play the note keys (MIDI notes fold into the keys' range), and the
  octave and the four knobs have buttons. `?mode=synth` starts there.
- The page mirrors the firmware's state from the keys it sends (tune, engine, chip, pause), so
  its buttons and the panel's knobs stay in step; it moves to the next tune at the end of the
  HVSC song length. The address follows the tune: `?t=MUSICIANS/H/Hubbard_Rob/Sanxion.sid&s=2`.
- `../tools/build_site.sh` puts the page and the emulator build's
  `fm1_web.wasm` together; `.github/workflows/pages.yml` runs `web/smoke.mjs` on the result:
  the wasm must boot the firmware with a pack of the own tune (no network) to sound and a
  screen, and in the synth mode stay silent until a key goes down, then sound. With an fm1-core that lacks PR #31 this check fails (the CPU halts), so merge PR #31
  first.
- Headless Chromium on this cloud machine runs it at 0.9-1.0x real time; a desktop browser has
  room to spare. The emulator skips idle time, so the work per second does not depend on the
  emulated clock.

## Measured (own model)

`fm1/run.py` builds the bench (`fm1/bench.c` + `fm1/workload.h`: a held chord on every
voice, SAW LEAD patch with the filter and vibrato, a chord change halfway, rendered in
128-sample blocks like Felucca's I2S half buffers) with the JieLi clang for pi32v2, runs it
in the emulator, counts the instructions between two markers and compares the
output with the host build:

| SID chips | voices | instr / sample (-O2) | of a 240 MHz core | -Os (Felucca's flag) | output = host |
|---:|---:|---:|---:|---:|:---:|
| 1 | 3  | 316   | 5.8 %  | 334 (6.1 %)    | yes |
| 2 | 6  | 642   | 11.8 % |                | yes |
| 3 | 9  | 979   | 18.0 % |                | yes |
| 4 | 12 | 1,348 | 24.8 % | 1,418 (26.1 %) | yes |

- Instructions are not cycles: the percentages assume one instruction per cycle. pi32v2
  issues one most of the time, but loads from XIP flash and divides take longer, so read them
  as a lower bound. Even at 2x the FM-1 has room for four SIDs plus effects.
- Init (building the combined-waveform tables) costs ~9.9 M instructions per chip model
  (14.7 M at -Os), once at boot (~40–60 ms). In a firmware these should become `const` tables in flash.
- The code is small: the whole bench, SID model + synth driver + 12 patches, is 6 KB of
  pi32v2 code.
- `make test`: 14 checks against the datasheet / known chip behaviour pass (pitch, attack
  times, decay / release with the exponential divider, sustain, the ADSR delay bug, noise,
  hard sync, a retrigger at full level, filter stability at full resonance, 6581
  volume-register digis).

## What is in here

```
src/sid.h, sid.c          the chip: register-level 6581 / 8580 model (integer only)
src/sidsynth.h, .c        the instrument driver + 12 example patches
host/demo.c               self tests, demo WAVs, the host side of the bench, host timing
fm1/workload.h            the bench workload (shared by host and FM-1 builds)
fm1/bench.c, crt0.S,      bare-metal pi32v2 program for the emulator
  bench.ld, build.sh
fm1/run.py                build + run on the emulated FM-1 + compare + report
fm1/bench_resid.cc,       reSID 0.16 replaying the workload's register stream on the FM-1
  bench_resid.ld, build_resid.sh, run_resid.py, sidlog2h.py
host/replay.cc,           one register stream on every engine (own, reSID 0.16 / 1.0, reSIDfp)
  engines.h, eng_*.cc
tools/fetch_ext.sh        fetch reSID 0.16, reSID 1.0, reSIDfp into ext/ (GPL, not in the repo)
tools/compare.sh          record every demo piece, replay on every engine -> build/cmp/
tools/listen.py,          level-matched MP3s, spectrograms and the A/B page -> build/listen/
  listen.html
host/sidlogger.cpp        a tune's SID writes, cycle-exact, from libsidplayfp's C64 (sidlog v2)
tools/scan_tunes.py       rank HVSC tunes by sync / ring / filter use
tools/tunes.sh,           HVSC tunes on every engine + sidplayfp reference; alignment check
  refcheck.py
player/psid.h, psid.c     PSID and RSID tunes on cpu6510.h (the C64 around it: RAM, banking,
                          both CIAs, the VIC's raster, a KERNAL stub)
player/cpu6510.h          an NMOS 6510 an instruction at a time, each access at its cycle
player/psidlog.c          the same on the host -> sidlog v2, to check against libsidplayfp
player/gt2.h, gt2.c       GoatTracker 2 songs: GoatTracker's playroutine on the .sng in place
                          (GPL-2.0-or-later)
player/gt2log.c           the same on the host: registers per frame (as gt2dump), or sidlog v2
tools/gt2check.sh         gt2log against GoatTracker's own gplay.c (sid-tracker's gt2dump)
player/gt2edit.c          a GoatTracker song to edit: order lists and patterns in a pool of RAM,
                          GoatTracker's editor changes, the .sng writer (GPL-2.0-or-later)
player/gt2edtest.c        on the host: save as loaded, into RAM, random edits, saved and back
firmware/tracker.c        the GoatTracker editor on the FM-1's LCD and controls
firmware/store.c          songs saved in flash, in the app slot's free end
firmware/main.c, fx.c,    the FM-1 SID player: 6502 + reSID 0.16 / own model, LCD visualiser,
  resid_glue.cc, app.ld,  effects (GPL-3.0-only, on Felucca's HAL); tunes from a pack after the image
  gen_assets.py, build.sh
tools/sidpack.py          a firmware image with a tune pack
tools/fwsc.py             the firmware as an FM-1 update package (web/sidplayer.fwsc)
tools/housetune.py        the firmware's own tune, Pocket Sync (PSID)
web/                      the player page (GitHub Pages): panel, playlist, HVSC search, files
tools/picks.py            web/tunes.json: the picks with HVSC lengths and measured tags
tools/webpage.sh          the page with the emulator's wasm in build/pages, for a local look
```

```sh
make test                 # self tests
make wav                  # build/patch_*.wav (every patch, 8580) and build/song_{6581,8580}.wav
make bench                # host timing
fm1/run.py [chips...]     # the FM-1 measurement (needs the toolchain and the emulator)
tools/fetch_ext.sh && make build/sid_record build/sid_replay && tools/compare.sh && tools/listen.py
fm1/run_resid.py [0 1 2]  # reSID 0.16 fast / interpolate / resample on the FM-1 (after compare.sh)
```

`fm1/run.py` needs `../target/release/fm1-emu` (`cargo build --release -p fm1-emu` in the
repository root) and the JieLi Linux toolchain, which runs natively on Linux x86-64
(`../docs/recon.md` has the URL; `JIELI_TOOLCHAIN` points at it, the default is
`../reference/toolchain/jieli-linux-toolchains-*`). On macOS run the toolchain through
Docker as `../tools/probe_build.sh` does.

### The chip model (`src/sid.c`)

Written from public descriptions of the chip — no code from reSID / reSIDfp — so it can be
BSD like the rest of this repo. Integer-only, no 64-bit arithmetic and no division per
sample, so the FM-1 and the host produce identical samples. It runs at the output rate and
advances the chip by the ~22.3 SID cycles that fit in each sample, solving the parts that
need cycle precision analytically:

- **Oscillators**: 24-bit phase accumulators, triangle / saw / pulse / noise; hard sync
  restarts the synced voice where it would be after the source's MSB rose inside the batch;
  ring modulation flips the triangle with the source's MSB; the 23-bit noise LFSR is clocked
  once per rising edge of accumulator bit 19 (several times per sample at high pitches); the
  test bit; "no waveform" holds the last DAC value.
- **Envelope**: the real 15-bit rate counter, the 16-entry period table, the exponential
  decay / release divider, the sustain comparison, the freeze at zero — and the **ADSR delay
  bug** (lower the rate below the counter and it wraps through 0x7FFF first), which matters
  for how real C64 music sounds and why drivers use a hard restart.
- **Combined waveforms**: AND of the components plus a neighbour-bit pull-down, harder on the
  6581 than on the 8580. A model of my own, *not* sampled from real chips (see below).
- **Filter**: 12 dB/oct state-variable filter in the TPT form (stable up to the top of the
  range), LP / BP / HP and their sums (notch), resonance Q 0.7–1.7 (8580) / 0.7–2.0 (6581);
  the 8580's linear cutoff curve and an S-shaped 6581 curve (approximate — real 6581s differ
  from chip to chip).
- **Mixer**: 3OFF, the master volume, the 6581's DC offsets (so writing the volume
  register plays samples, the classic "digi" trick) and the output's AC coupling.

### The synth driver (`src/sidsynth.c`)

The SID sound people know is as much the music drivers as the chip: registers written once
per video frame from per-instrument tables. `sidsynth` does that, for 1–4 chips:

- **wavetable** per instrument: waveform + note offset per frame, with a loop (drums = a noise
  frame then a falling pulse/triangle; the C64 "chord" = a 0-4-7 arpeggio looped at 50 Hz)
- **pulse width sweep** (ping-pong), **filter**: mode, resonance, cutoff start and sweep
- **vibrato** with delay, **sync / ring** patches (the note then uses voice 1 as the sounding
  voice and voice 3 as the silent modulator at its own interval, with a pitch sweep),
  **hard restart**
- polyphony: 3 notes per chip (1 per chip with sync / ring patches); the notes on a chip
  share its filter, as on the real thing; 50 Hz (PAL) frames, settable for "multispeed"

Patches: SAW LEAD, PWM BASS, MAJ / MIN CHORD, SYNC LEAD, RING BELL, KICK, SNARE, HAT, RESO
PAD, P&S ORGAN, BP SWEEP. `make wav` renders each, plus a short four-chip song (bass,
chords, sync lead, drums) on both chip models.

## The FM-1 as a SID synth

What the hardware gives (from `../docs/recon.md` and Felucca's HAL):

| | |
|---|---|
| CPU | pi32v2, 240 MHz, **two cores**, FPU (single) |
| Memory | ~512 KB SRAM, 1 MB flash (XIP), ~568 KB of it for the app |
| Audio | I2S codec, 44.1 kHz, 24-bit, 128-frame half buffers (2.9 ms) |
| Panel | 27 keys, 14 buttons with LEDs, 7 encoders, MASTER pot, 240×240 LCD |
| I/O | USB-C (MIDI, audio, the SysEx firmware update), TRS MIDI in, BLE |

Everything a SID instrument needs is there. A possible panel layout:

- **keys**: notes (or, in a "tracker" mode, pattern steps)
- **encoders**: K1 cutoff, K2 resonance, K3 pulse width, K4 waveform / wavetable;
  SELECT / ALGO / PRESET for patch browsing and the ADSR page
- **buttons**: 6581 / 8580, filter mode LP / BP / HP, SYNC, RING, ARP, 1–4 SIDs
- **screen**: the 29 registers live, the envelope, the oscilloscope, the wavetable editor

Polyphony: 12 voices with four chips is ~25 % of a core. The second core is free in
Felucca-style firmwares (Melodee and X0X hand audio to it), which could run more chips or a
`.sid` player.

## Ways to build it

**A. A SID engine in Felucca (recommended first step).** Felucca
(`hugelton/Felucca`, GPL-3.0) already has the UI, presets, the sequencer, USB / TRS MIDI,
flash storage, the update package and a browser emulator, and it runs in the emulator's
emulator. Its engines are plug-ins (`engine_t` in `firmware/src/core.h`): `note_on`,
`render`, `block` (once per block — the place for the 50 Hz driver tick), `ownenv` + `done`
(the engine's own envelope is the amplitude: the SID ADSR), 8 EDIT parameters and a part
memory (12.9 KB per part; a 1-chip `sidsynth_t` is well under 1 KB). One wrinkle: Felucca
renders per voice, a SID shares one filter between three voices — run the part as one chip
(poly cap 3, the filter in `block`) rather than one chip per voice. The combined-waveform
tables become `const` (flash). The result is GPL-3.0 (this code is BSD-2, which is
compatible).

**B. A dedicated firmware** on Felucca's HAL: full freedom for a SID-specific UI (register
view, wavetable / pulse / filter table editors, a small tracker), multi-SID, and room for C.
More work: USB, MIDI, flash and the update path have to come along.

**C. A `.sid` player** (built: see "The SID player firmware" above). A 6502 core plus the PSID format plays the C64 music library with the
same chip model. Cost is small: a typical play routine runs a few thousand 6502 cycles per
frame, i.e. a few million pi32v2 instructions per second. Digi-heavy tunes that write
registers at kHz rates need the chip rendered in smaller chunks (supported: `sid_render`
takes any length).

## Limits and open questions

- **Aliasing** (own model and reSID fast alike): the waveforms are sampled at 44.1 kHz,
  while the real chip runs at ~1 MHz into an analogue filter. The alias-free reSID modes cost
  1-7 cores. In between: reSID fast at 2x rate + a half-band decimator, or polyBLEP edges
  in the own model; to be measured.
- **Licence**: reSID is GPL-2.0-or-later, so a firmware using it is GPL (as Felucca is).
  The own model stays BSD.
- **6581 filter distortion** is only in reSID 1.0 / reSIDfp, which do not fit.
- **Cycles, not instructions**: confirm on hardware once the FM-1 arrives (the emulator does
  not model cycle timing; reSID's integer code and the own model run mostly single-cycle ops).
- The FM-1 itself has not run this code, only its emulator (which needed four fixes for it).

## Next steps

1. reSID 0.16 (fast) as the chip under `sidsynth`; its wave tables as `const` in flash.
2. Measure reSID fast at 88.2 kHz + half-band decimation against the alias-free modes.
3. Felucca engine (path A): `eng_sid.c`, build Felucca with the toolchain, play it in
   `fm1-live` and the web emulator.
4. On the real FM-1 when it arrives: cycle cost, sound check against a real C64.
5. The player: multi-SID tunes, tunes loaded over USB instead of built in; for RSID tunes an
   instruction-stepped 6502 (the cycle-stepped one is most of their cost).
6. USB: installing through the running firmware (Felucca's M-UPGRADE handshake, `ota.c`), USB
   audio (Felucca's UAC1 input), MIDI out and the TRS MIDI jack.
7. The GoatTracker editor, next: songs out and in over MIDI (SysEx: the .sng to the page, to
   sid-tracker); copy / paste of rows and patterns; a flash store in fm1-dock's boot slot (its
   flash is fm1-dock's to give); the page following the editor (it mirrors the player and the
   synth only); faster entry (a computer's keys through the page).
