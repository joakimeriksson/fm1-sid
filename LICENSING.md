# Licensing

FM-1 SID is free software. Each file says its licence in its `SPDX-License-Identifier` line; a file without
one is under the MIT licence, as it was in [fm1-emu](https://github.com/joakimeriksson/fm1-emu), where FM-1
SID grew up (Copyright (c) 2026 Joakim Eriksson).

| What | Licence |
| --- | --- |
| The firmware (`firmware/`, the Felucca and Dinghy builds), the fm1-dock module (`dock/`), the web page's app (`web/`) | GPL-3.0-only (`LICENSE`): whoever gets a firmware built from it gets its source |
| The PSID / RSID player and the 6510 (`player/psid.c`, `player/cpu6510.h`), the own SID model (`src/`), most of `tools/` | BSD-2-Clause |
| The GoatTracker 2 player and editor (`player/gt2*.c`): GoatTracker's playroutine by Lasse Öörni (Cadaver) and contributors | GPL-2.0-or-later |
| `dinghy/`: Dinghy 0.3 (zvenson) on the platform of Felucca (Leo Kuroshita, Hügelton Instruments) as SLOOP (isod89) carries it | GPL-3.0-only (`dinghy/LICENSE`, `dinghy/LICENSING.md`) |
| Fetched by `tools/fetch_ext.sh` into `ext/`, not in this tree: reSID 0.16 (Dag Lem), m6502.h (Andre Weissflog), Felucca, libsidplayfp (for the host checks) | GPL-2.0-or-later, zlib, GPL-3.0, GPL-2.0-or-later |

A firmware or module built from these parts is GPL-3.0 as a whole. C64 tunes are their composers' and are
not in this tree; the web page fetches them from [HVSC](https://www.hvsc.c64.org/) when asked.
