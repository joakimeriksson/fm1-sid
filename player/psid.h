/* player/psid.h - run a C64 tune's 6502 code and report its SID writes, cycle-exact.
 *
 * The 6502 is player/cpu6510.h: an instruction at a time, each access at its cycle, decimal mode,
 * the undocumented opcodes. Two ways to run it:
 *
 * PSID tunes: init, then a play routine per frame, called as subroutines. Around them 64 KB
 * RAM with the $01 banking of the I/O area, CIA 1 timer A as the speed source of CIA-timed
 * tunes, the SID registers ($D400-$D7FF, every 32 bytes); no ROMs, no VIC beyond the raster
 * counter, no NMI.
 *
 * RSID tunes: the C64 keeps running. Init is entered as libsidplayfp's driver enters it (CIA 1
 * timer A interrupting at 50 Hz, interrupts on, an idle loop to return to) and may never
 * return; the tune's own interrupt handlers do the rest. Here: both CIAs' timers (CIA 1 -> IRQ,
 * CIA 2 -> NMI), the VIC's raster counter, raster interrupt and bad lines (the CPU stopped 40
 * cycles of every 8th visible line while the screen is on), the $01 banking of the I/O area
 * and the KERNAL, whose place a stub takes: the IRQ / NMI entries through $0314 / $0318, the
 * $EA31 / $EA81 / $FEBC exits, RTS elsewhere. No BASIC or character ROM (RAM shows through),
 * no sprites, no keys pressed. Tunes that need BASIC (the header's BASIC flag) are refused.
 *
 * Portable C: the host (player/psidlog.c, checked against libsidplayfp's register stream)
 * and the FM-1 firmware (firmware/) run the same code.
 * SPDX-License-Identifier: BSD-2-Clause */
#ifndef PSID_H
#define PSID_H
#include <stdint.h>

#define PSID_PAL_CLOCK 985248u
#define PSID_PAL_FRAME 19656u            /* 312 lines x 63 cycles */

/* a SID write: cycle = cycles since the start of this init / play call */
typedef void (*psid_write_fn)(void *user, uint32_t cycle, uint32_t reg, uint32_t val);

typedef struct psid psid_t;

/* the tune's header fields (strings NUL-terminated, Latin-1) */
typedef struct {
    char name[33], author[33], released[33];
    uint16_t load, init, play;
    uint16_t songs, start;
    uint32_t speed;                       /* bit n set: song n+1 is CIA-timed */
    uint8_t model;                        /* 0 6581, 1 8580 (header flags; unknown -> 6581) */
    uint8_t rsid;                         /* 1: an RSID tune (the whole C64 runs) */
} psid_info_t;

/* the player state is large (64 KB RAM): static storage on the FM-1 */
uint32_t psid_size(void);
int psid_load(psid_t *p, const uint8_t *data, uint32_t len, psid_write_fn wr, void *user);   /* 0: ok */
/* a tune arriving in pieces (over USB): psid_begin takes the file's first `have` bytes (the header, and
 * the load address when the header's is 0: off + 2) of a file of n, sets the player up as psid_load and
 * returns the file offset of the byte that goes to the load address (-1: not a tune we play); the caller
 * then puts file byte off + i at psid_ram(p)[info.load + i] (below 65536) before psid_init */
int psid_begin(psid_t *p, const uint8_t *head, uint32_t have, uint32_t n, psid_write_fn wr, void *user);
uint8_t *psid_ram(psid_t *p);
const psid_info_t *psid_info(const psid_t *p);
/* run the init routine for song (1-based); returns the cycles it took (RSID: sets the C64 up
 * to enter init and returns 0, the first psid_play runs it) */
uint32_t psid_init(psid_t *p, uint32_t song);
/* cycles from one play call to the next (a PAL frame, or the CIA timer A latch; RSID a frame) */
uint32_t psid_period(const psid_t *p);
/* run one play call (or the tune's own interrupt handler); returns the cycles it took. RSID:
 * the C64 runs on for one PAL frame */
uint32_t psid_play(psid_t *p);
/* the KERNAL stub RSID tunes run on, as an 8 KB ROM image (to give libsidplayfp the same one:
 * build/sidlogger with FM1SID_KERNAL) */
void psid_kernal(uint8_t rom[8192]);

#endif
