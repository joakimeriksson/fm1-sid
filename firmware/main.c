/* firmware/main.c - a SID player for the M-VAVE FM-1: C64 tunes (PSID, RSID) played by their own
 * 6502 code (player/psid.c, player/cpu6510.h) on the FM-1's CPU, and GoatTracker 2 songs (.sng)
 * by GoatTracker's playroutine (player/gt2.c, from the file in flash); the SID rendered by reSID 0.16
 * (fast) or by fm1-sid's own model (src/sid.c), the three voices, sync / ring and the filter
 * drawn on the 240x240 LCD.
 *
 * Built on Felucca's hardware layer (ext/Felucca/firmware/hal, crt0.S, fm1_vec.S, fm1_isr.S;
 * GPL-3.0-only), so this firmware is GPL-3.0-only too.
 *
 * Two modes, HOME switches. PLAYER: PRESETS / SELECT encoder or OCT- / OCT+ = tune, PLAY =
 * pause. SYNTH: the note keys play fm1-sid's instrument driver (src/sidsynth.c) on the same
 * chip, PRESETS / SELECT = patch, OCT- / OCT+ = octave, KNOB1..4 = cutoff, resonance, attack,
 * release. Its step sequencer: SEQ = its page, PLAY = run / stop, REC = record, SCL / ENV / LFO =
 * track 1 / 2 / 3 (PRESETS then picks the track's patch), ALGORITHM = tempo, GLO = clear the step,
 * ARP = clear the track. Both: EDIT = engine (reSID / own); FX = the effects page (firmware/fx.c:
 * KNOB1 chorus, KNOB2 delay, KNOB3 its time, KNOB4 reverb, SELECT the delay's feedback, PRESETS
 * 6581 / 8580; FX again leaves it). Hold OCT- and OCT+ for 3 s: the USB update mode (UBOOT).
 * Hold HOME for 5 s (or while switching on): USB MODE, everything off but USB (usb_mode).
 * MASTER is the volume (master_poll); the keys and buttons light while held, PLAY / REC / FX /
 * SEQ also while playing, recording, on the FX page, in the editor or the sequencer, and the
 * others that do something on the page glow dim (leds).
 * SEQ in the player: the GoatTracker editor (firmware/tracker.c) on the song playing; its songs
 * are saved in flash (firmware/store.c) and join the player's list.
 * USB: a class-compliant USB-MIDI device, "FM-1 SID" (Felucca's driver): notes play the synth and
 * the editor, a program change picks the player's tune, Felucca's soft key SysEx restarts into
 * the USB update mode; and the installers (Felucca's web installer, tools/fm1_install.py) find it
 * and install another firmware through it as through Felucca (its src/ota.c).
 *
 * Timing: the main loop runs the play routine up to three frames ahead and queues its SID
 * writes with their C64 cycle; the audio interrupt (ALNK0, 128 frames a half buffer) clocks
 * the SID engine to each write's cycle and applies it, so the writes land cycle-exact for
 * reSID and to the sample for the own model.
 * SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>
#include "fm1_cc.h"
#include "fm1_time.h"
#include "fm1_irq.h"
#include "fm1_sys.h"
#include "fm1_gpio.h"
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_audio.h"
#include "fm1_input.h"
#include "fm1_adc.h"
#include "sid.h"
#include "psid.h"
#include "gt2.h"
#ifndef DOCK_BOOT
#define DOCK_BOOT 0                      /* 1: built for fm1-dock's boot slot (firmware/build.sh) */
#endif
#define FELUCCA_OTA (!DOCK_BOOT)          /* installers' updates through FM-1 SID (Felucca's ota.c) */
#define FELUCCA_ID "FM-1_922"               /* the identity it answers (= the package's, tools/fwsc.py) */
#include "sidsynth.h"

void *memset(void *d, int c, unsigned long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

#include "assets.h"

/* ------------------------------------------------------------------------- the tunes --- */
/* A tune pack appended to the image (tools/sidpack.py, or the web page): "SIDPACK1", u32
 * count, u32 start (bits 0-15 the first entry, bit 16 the own model instead of reSID, bits
 * 17-18 the chip for every tune: 0 the tune's header, 1 6581, 2 8580, bit 19 start in the
 * synth mode), then per entry u32
 * offset (from the pack), length, song (0: the tune's start song); little-endian. An entry is a
 * PSID / RSID file or a GoatTracker 2 song (.sng: song = its subtune, from 1). Without one: the
 * firmware's own tune (tools/housetune.py). */
extern const uint8_t _pack[];
#define MAXTUNES 256u
#define PACK_MAX 0x80000u
static const uint8_t *tune_ptr[MAXTUNES];
static uint32_t tune_len[MAXTUNES], tune_song[MAXTUNES], ntunes, first_tune, pack_flags;
static int8_t tune_slot[MAXTUNES];          /* a song saved in flash: its slot (the pack's: -1) */
static uint32_t npack, pack_end;            /* the pack's entries; where the image and pack end */

static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static void load_pack(void)
{
    uint32_t n, i;
    pack_end = (uint32_t)_pack;
    if (!memcmp(_pack, "SIDPACK1", 8)) {
        n = rd32(_pack + 8);
        first_tune = rd32(_pack + 12) & 0xFFFFu;
        pack_flags = rd32(_pack + 12) >> 16;
        for (i = 0; i < n && i < MAXTUNES; i++) {
            const uint8_t *e = _pack + 16u + 12u * i;
            uint32_t off = rd32(e), len = rd32(e + 4);
            if (len < 0x7Cu || off > PACK_MAX || len > PACK_MAX - off)
                continue;
            tune_ptr[ntunes] = _pack + off;
            tune_len[ntunes] = len;
            tune_song[ntunes] = rd32(e + 8);
            tune_slot[ntunes] = -1;
            if ((uint32_t)_pack + off + len > pack_end)
                pack_end = (uint32_t)_pack + off + len;
            ntunes++;
        }
        if ((uint32_t)_pack + 16u + 12u * n > pack_end)
            pack_end = (uint32_t)_pack + 16u + 12u * n;
    }
    if (!ntunes) {
        tune_ptr[0] = HOUSE;
        tune_len[0] = sizeof HOUSE;
        tune_song[0] = 0;
        tune_slot[0] = -1;
        ntunes = 1;
    }
    npack = ntunes;
    if (first_tune >= ntunes)
        first_tune = 0;
}

/* reSID (firmware/resid_glue.cc) */
void rs_init(int model, double clock, double fs);
void rs_write(int reg, int val);
int rs_clock(int *cycles, short *buf, int n);
void rs_env(unsigned char env[3]);

#define HALF_FRAMES 128u
#define HALF_WORDS (HALF_FRAMES * 2u)
#define FS_HZ 44118u                       /* 24 MHz / 544 = 44117.6 */
#define CPS_Q16 1463569u                   /* SID cycles per output sample, Q16: 985248 * 544 / 24e6 */
#define OUT_SHIFT 8                        /* 16-bit -> 24-bit (the SID engines peak well below full scale) */

/* ------------------------------------------------------------------------------- state --- */
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static uint8_t psid_mem[70000] __attribute__((aligned(8)));   /* psid_t (64 KB RAM inside) */
#define P ((psid_t *)psid_mem)
#define G ((gt2_t *)psid_mem)               /* or a GoatTracker song's index and player (7.4 KB), and
                                             * in the editor the song's pool after it */
static uint32_t song_tail;                  /* bytes at psid_mem's end holding a saved song (song_ram) */

static uint8_t gt;                          /* the tune playing is a GoatTracker song */
static uint8_t trk;                         /* the GoatTracker editor is up (firmware/tracker.c) */
typedef char gt2_fits[GT2_SIZE_MAX <= sizeof psid_mem ? 1 : -1];
static sid_t own;

typedef struct { uint32_t cyc; uint8_t reg, val; } ev_t;
#define NEV 4096u
static ev_t evq[NEV];
static volatile uint32_t ev_head, ev_tail;  /* main loop writes head, the audio ISR tail */
static uint32_t gen_cyc, call_base;         /* main loop: the next play call; this call's start */
static volatile uint32_t play_cyc;          /* audio ISR: the SID's time */
static uint32_t cyc_frac;
static volatile uint8_t running, engine, model;    /* engine 0 reSID, 1 own */
static volatile uint8_t regs[32];           /* the registers as written, for the display */
static int16_t scope[512];
static volatile uint32_t scope_w;
static volatile uint32_t isr_ticks, isr_halves;
static uint32_t play_idx, played_ms;
static volatile uint32_t ms;

/* the synth mode: fm1-sid's driver writes into the same event queue, from the audio ISR. Four
 * driver instances: 0..2 the sequencer's tracks, each a part on its own SID voice with its own
 * patch, and 3 the polyphonic synth on all three voices (the keys while the tracks are idle). */
static volatile uint8_t mode;               /* 0 the tune player, 1 the synth */
static sidsyn_t inst[4];
#define POLY 3u
static uint32_t patch;                      /* the polyphonic synth's SID_PATCHES index */
static int32_t octave;                      /* -2..2 */
static int32_t knob[4];                     /* its knobs: cutoff offset, resonance, attack, release */
static uint8_t key_note[27];                /* the note each held key started */
enum { CMD_ON, CMD_OFF, CMD_PATCH, CMD_ALLOFF, CMD_PARTS, CMD_SEQ };
typedef struct { uint8_t op, arg, tgt; } cmd_t;
#define NCMD 64u
static cmd_t cmdq[NCMD];
static volatile uint32_t cmd_head, cmd_tail;   /* main loop writes head, the audio ISR tail */
static uint32_t syn_next;                   /* the cycle of the driver's next frame */
static uint32_t syn_held;                   /* the note keys down at the last look */
#define HALF_CYC 2860u                      /* SID cycles in a 128-frame half buffer, rounded up */

/* the step sequencer: three tracks of 16 steps, a MIDI note or 0 (rest) each. A track plays its
 * own patch on its own voice; KIT is a drum kit: a note's pitch class picks the drum (C..D#
 * kick, E..G snare, G#..B hat). Steps fire in the audio ISR at their own cycle. */
#define NTRK 3u
#define NSTEP 16u
#define KIT 12u                             /* (SID_NPATCHES: the patch after the last) */
static uint8_t pat[NTRK][NSTEP] = {
    {45, 0, 45, 57, 0, 45, 0, 55, 45, 0, 57, 0, 43, 0, 45, 48},     /* PWM BASS */
    {69, 0, 0, 0, 72, 0, 71, 0, 69, 0, 0, 0, 64, 0, 67, 0},        /* SAW LEAD */
    {48, 0, 57, 0, 52, 0, 57, 0, 48, 48, 57, 0, 52, 0, 57, 57},    /* KIT: kick, hat, snare */
};
static uint8_t trk_patch[NTRK] = {1, 0, KIT};
static int32_t trk_knob[NTRK][4];
static uint32_t sel;                        /* the track the keys and knobs work on */
static volatile uint8_t seq_on, rec, seq_view, parts;   /* parts: the tracks own the voices */
static volatile uint32_t seq_pos;           /* the step sounding */
static volatile uint32_t seq_next;          /* ISR: the cycle of the next step */
static uint32_t seq_frac, cursor;
static volatile uint32_t bpm = 120;
static uint8_t trk_on[NTRK], live[NTRK], kit_cur[NTRK];   /* ISR: each track's note, keys held, kit drum */
static uint8_t f_route[4], f_res[4];        /* each instance's filter routing and resonance */
static uint8_t parts_req, seq_req;          /* main loop: what it asked the ISR for */
static uint8_t key_tgt[27];                 /* the instance each held key plays */

#include "store.c"

/* the player's list: the pack's tunes, then the songs saved in flash (read into RAM to play:
 * song_ram) */
static void list_store(void)
{
    uint32_t i, len;
    ntunes = npack;
    for (i = 0; i < ST_SLOTS && ntunes < MAXTUNES; i++) {
        if (!(len = st_song(i, 0, 0)))
            continue;
        tune_ptr[ntunes] = 0;
        tune_len[ntunes] = len;
        tune_song[ntunes] = 0;
        tune_slot[ntunes] = (int8_t)i;
        ntunes++;
    }
    if (play_idx >= ntunes)
        play_idx = 0;
}

/* a song saved in flash into RAM, at psid_mem's end (after G's index and the pool's start) */
static const uint8_t *song_ram(uint32_t slot, uint32_t *len)
{
    uint32_t n = st_song(slot, 0, 0), room = sizeof psid_mem - GT2_SIZE_MAX, at;
    song_tail = 0;
    if (!n || n > room)
        return 0;
    at = (uint32_t)(sizeof psid_mem - n) & ~3u;
    if (st_song(slot, psid_mem + at, n) != n)
        return 0;
    song_tail = (uint32_t)sizeof psid_mem - at;
    *len = n;
    return psid_mem + at;
}

/* ------------------------------------------------------------------------- USB-MIDI --- */
/* Felucca's USB driver (ext/Felucca/firmware/src/usb.c; firmware/build.sh makes it
 * build/fw/usb_fm1sid.c, named FM-1 SID): a class-compliant USB-MIDI device, its SIE polled from
 * the TIMER5 ISR at 2 kHz. MIDI in waits in its ring for the main loop (midi_in); its soft key
 * SysEx F0 22 24 35 7D F7 restarts the FM-1 into the USB update mode (the main loop). */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "usb_fm1sid.c"

#if FELUCCA_OTA
/* Updates as Felucca takes them (ext/Felucca's src/ota.c, build/fw/ota_fm1sid.c, unchanged): an
 * installer's handshake gets the identity FM-1_922, its upgrade command (F0 22 24 35 7F F7) a
 * session in which FM-1 SID reads parts of the package, checks it, stages the package's update
 * loader at 0xE0000 (Felucca's OTA area) and, once the installer says "success", restarts into
 * it; the loader writes the package. Anything else leaves FM-1 SID as it was. These are the
 * hooks Felucca's src/ota_hw.c gives it, with FM-1 SID's screen. */
#include "ota_fm1sid.c"
static uint32_t ota_now_ms(void) { return ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code);
static void ota_commit(const uint8_t *parm);
#endif

/* ------------------------------------------------------------------- the play routine --- */
static void on_write(void *user, uint32_t cycle, uint32_t reg, uint32_t val)
{
    uint32_t h = ev_head;
    (void)user;
    if (h - ev_tail >= NEV)                 /* full: drop (should not happen with the look-ahead) */
        return;
    evq[h % NEV].cyc = call_base + cycle;
    evq[h % NEV].reg = (uint8_t)reg;
    evq[h % NEV].val = (uint8_t)val;
    ev_head = h + 1;
}

static void generate(void)
{
    while (running && !mode && gen_cyc - play_cyc < (trk ? 2u : 3u) * PSID_PAL_FRAME &&
           ev_head - ev_tail < NEV - 600u) {   /* (the editor: 40 ms ahead, its keys heard sooner) */
        call_base = gen_cyc;
        if (gt) {
            gt2_play(G);
            gen_cyc += GT2_FRAME;
        } else {
            psid_play(P);
            gen_cyc += psid_period(P);
        }
    }
}

static const psid_info_t *tune_info(void) { return gt ? gt2_info(G) : psid_info(P); }

/* ------------------------------------------------------------------------ the synth --- */
/* the track whose patch has the filter: the chip has one, the first such track sets it */
static uint32_t filter_owner(void)
{
    uint32_t t;
    for (t = 0; t < NTRK; t++)
        if (inst[t].p.filter)
            return t;
    return POLY;
}

/* an instance's register write into the queue (at call_base). The tracks share the chip's one
 * filter: their routing bits merge, cutoff, resonance and mode are the owner's. */
static void syn_write(void *user, uint32_t chip, uint32_t reg, uint32_t val)
{
    uint32_t i = (uint32_t)(uintptr_t)user, own_;
    (void)chip;
    if (i < NTRK && reg >= 0x15u && reg <= 0x18u) {
        own_ = filter_owner();
        if (reg == 0x17u) {
            f_route[i] = (uint8_t)(val & 7u);
            f_res[i] = (uint8_t)(val >> 4);
            val = (own_ < NTRK ? (uint32_t)f_res[own_] << 4 : 0u) | f_route[0] | f_route[1] | f_route[2];
        } else if (i != own_ && !(own_ == POLY && reg == 0x18u)) {
            return;
        }
    }
    on_write(0, 0, reg, val);
}

static uint32_t syn_env(void *user, uint32_t chip, uint32_t voice)
{
    uint8_t e[3];
    (void)user;
    (void)chip;
    if (engine)
        return own.v[voice].env;
    rs_env(e);
    return e[voice];
}

/* knobs k over patch b, into instance y */
static void apply_knobs(sidsyn_t *y, const sid_patch_t *b, const int32_t *k)
{
    int32_t r = (int32_t)b->res + k[1], a = (int32_t)(b->ad >> 4) + k[2], rl = (int32_t)(b->sr & 15u) + k[3];
    y->cut_ofs = k[0] * 16;
    y->p.res = (uint8_t)(r < 0 ? 0 : r > 15 ? 15 : r);
    y->p.ad = (uint8_t)((a < 0 ? 0 : a > 15 ? 15 : a) << 4 | (b->ad & 15u));
    y->p.sr = (uint8_t)((b->sr & 0xF0u) | (uint32_t)(rl < 0 ? 0 : rl > 15 ? 15 : rl));
    y->transpose = 0;
}

/* the drum of a kit note (6 KICK, 7 SNARE, 8 HAT) */
static uint32_t kit_drum(uint32_t note) { uint32_t c = note % 12u; return c < 4u ? 6u : c < 8u ? 7u : 8u; }

static const sid_patch_t *base_patch(uint32_t i)
{
    if (i == POLY)
        return &SID_PATCHES[patch];
    return &SID_PATCHES[trk_patch[i] == KIT ? kit_cur[i] : trk_patch[i]];
}

static void syn_knobs(uint32_t i) { apply_knobs(&inst[i], base_patch(i), i == POLY ? knob : trk_knob[i]); }

/* (ISR) an instance's patch from the settings */
static void load_patch(uint32_t i)
{
    if (i < NTRK && trk_patch[i] == KIT && kit_cur[i] < 6u)
        kit_cur[i] = 6;
    sidsyn_set_patch(&inst[i], base_patch(i));
    syn_knobs(i);
    if (i < NTRK)
        trk_on[i] = 0;
}

/* (ISR) track t plays note (0: stops) */
static void trk_note(uint32_t t, uint32_t note)
{
    if (trk_on[t]) {
        sidsyn_note_off(&inst[t], trk_on[t]);
        trk_on[t] = 0;
    }
    if (!note)
        return;
    if (trk_patch[t] == KIT) {
        uint32_t d = kit_drum(note);
        if (d != kit_cur[t]) {
            kit_cur[t] = (uint8_t)d;
            sidsyn_set_patch(&inst[t], &SID_PATCHES[d]);
            syn_knobs(t);
        }
        note = 48;                          /* the drums are tuned for C3 */
    }
    sidsyn_note_on(&inst[t], note);
    trk_on[t] = (uint8_t)note;
}

/* (ISR) the next step */
static void seq_fire(void)
{
    uint32_t t, s = (seq_pos + 1u) % NSTEP;
    seq_pos = s;
    for (t = 0; t < NTRK; t++) {
        if (pat[t][s])
            trk_note(t, pat[t][s]);
        else if (!live[t])
            trk_note(t, 0);
    }
}

static uint32_t step_q8(void) { return (uint32_t)((14778720ull << 8) / bpm); }   /* PAL cycles a 16th, Q8 */

static void syn_cmd(uint32_t op, uint32_t arg, uint32_t tgt)
{
    uint32_t h = cmd_head;
    if (h - cmd_tail >= NCMD)
        return;
    cmdq[h % NCMD].op = (uint8_t)op;
    cmdq[h % NCMD].arg = (uint8_t)arg;
    cmdq[h % NCMD].tgt = (uint8_t)tgt;
    cmd_head = h + 1;
}

/* in the audio ISR, before a half is rendered: the keys now, then the sequencer steps and the
 * driver frames that fall in this half, in time order, each at its own cycle */
static void syn_step(void)
{
    uint32_t t, i;
    call_base = play_cyc;
    while ((t = cmd_tail) != cmd_head) {
        const cmd_t *c = &cmdq[t % NCMD];
        uint32_t g = c->tgt;
        if (c->op == CMD_ON) {
            if (g == POLY)
                sidsyn_note_on(&inst[POLY], c->arg);
            else {
                live[g] = 1;
                trk_note(g, c->arg);
            }
        } else if (c->op == CMD_OFF) {
            if (g == POLY)
                sidsyn_note_off(&inst[POLY], c->arg);
            else {
                live[g] = 0;
                if (trk_on[g] && (trk_patch[g] == KIT || trk_on[g] == c->arg))
                    trk_note(g, 0);
            }
        } else if (c->op == CMD_ALLOFF) {   /* (MIDI in broke off: no note left hanging) */
            sidsyn_all_off(&inst[POLY]);
            for (i = 0; i < NTRK; i++)
                if (live[i]) {
                    live[i] = 0;
                    trk_note(i, 0);
                }
        } else if (c->op == CMD_PATCH) {
            load_patch(g);
        } else if (c->op == CMD_PARTS) {    /* the voices to the tracks, or back to the poly synth */
            if (c->arg) {
                sidsyn_all_off(&inst[POLY]);
                for (i = 0; i < NTRK; i++)
                    load_patch(i);
            } else {
                for (i = 0; i < NTRK; i++)
                    trk_note(i, 0);
                load_patch(POLY);
            }
            parts = c->arg;
        } else if (c->op == CMD_SEQ) {
            seq_on = c->arg;
            if (c->arg) {
                seq_pos = NSTEP - 1u;
                seq_next = play_cyc;
                seq_frac = 0;
            } else {
                for (i = 0; i < NTRK; i++)
                    if (!live[i])
                        trk_note(i, 0);
            }
        } else {
            sidsyn_all_off(&inst[POLY]);
            for (i = 0; i < NTRK; i++)
                trk_note(i, 0);
        }
        cmd_tail = t + 1;
    }
    if ((int32_t)(syn_next - play_cyc) < -(int32_t)PSID_PAL_FRAME)
        syn_next = play_cyc;                /* (after a pause) */
    if (seq_on && (int32_t)(seq_next - play_cyc) < -(int32_t)PSID_PAL_FRAME)
        seq_next = play_cyc;
    for (;;) {
        uint32_t lim = play_cyc + HALF_CYC;
        int step = seq_on && (int32_t)(seq_next - lim) < 0;
        int frame = (int32_t)(syn_next - lim) < 0;
        if (step && (!frame || (int32_t)(seq_next - syn_next) <= 0)) {
            uint32_t q = step_q8() + seq_frac;
            call_base = seq_next;
            seq_fire();
            seq_next += q >> 8;
            seq_frac = q & 255u;
        } else if (frame) {
            call_base = syn_next;
            if (parts)
                for (i = 0; i < NTRK; i++)
                    sidsyn_tick(&inst[i]);
            else
                sidsyn_tick(&inst[POLY]);
            syn_next += PSID_PAL_FRAME;
        } else {
            break;
        }
    }
}

/* ------------------------------------------------------------------------ audio ISR --- */
static void fx_block(const short *in, int32_t *o, uint32_t n);   /* firmware/fx.c */

static void apply(const ev_t *e)
{
    regs[e->reg & 31] = e->val;
    if (engine)
        sid_write(&own, e->reg, e->val);
    else
        rs_write(e->reg, e->val);
}

static void render_resid(short *out)
{
    uint32_t got = 0;
    while (got < HALF_FRAMES) {
        int dt;
        uint32_t t = ev_tail;
        if (t != ev_head && (int32_t)(evq[t % NEV].cyc - play_cyc) <= 0) {
            apply(&evq[t % NEV]);
            ev_tail = t + 1;
            continue;
        }
        dt = t != ev_head ? (int)(evq[t % NEV].cyc - play_cyc) : 100000;
        {
            int left = dt;
            got += (uint32_t)rs_clock(&left, out + got, (int)(HALF_FRAMES - got));
            play_cyc += (uint32_t)(dt - left);
        }
    }
}

static void render_own(short *out)
{
    static int32_t tmp[HALF_FRAMES];
    uint32_t got = 0, i;
    while (got < HALF_FRAMES) {
        uint32_t t = ev_tail, k = HALF_FRAMES - got;
        if (t != ev_head) {
            int32_t d = (int32_t)(evq[t % NEV].cyc - play_cyc);
            if (d <= 0) {
                apply(&evq[t % NEV]);
                ev_tail = t + 1;
                continue;
            }
            if ((uint32_t)d < k * 23u)       /* samples until the write (CPS ~ 22.33) */
                k = (((uint32_t)d << 16) + CPS_Q16 - 1u) / CPS_Q16;
            if (!k)
                k = 1;
            if (k > HALF_FRAMES - got)
                k = HALF_FRAMES - got;
        }
        sid_render(&own, tmp, k);
        for (i = 0; i < k; i++) {
            int32_t v = tmp[i];
            out[got + i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            cyc_frac += CPS_Q16;
            play_cyc += cyc_frac >> 16;
            cyc_frac &= 0xffffu;
        }
        got += k;
    }
}

void fm1_alnk0_irq(void)
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t t0 = fm1_ticks(), i;
        int32_t *o = &abuf[fm1_audio_free_half() * HALF_WORDS];
        static short s[HALF_FRAMES];
        if (running) {
            if (mode)
                syn_step();
            if (engine)
                render_own(s);
            else
                render_resid(s);
        } else {
            for (i = 0; i < HALF_FRAMES; i++)
                s[i] = 0;
        }
        fx_block(s, o, HALF_FRAMES);
        for (i = 1; i < HALF_FRAMES; i += 2u)
            scope[scope_w++ & 511u] = s[i];
        fm1_audio_ack_half();
        isr_ticks += fm1_ticks() - t0;
        isr_halves++;
    }
}

void fm1_timer5_irq(void)
{
    static uint32_t last, acc;
    uint32_t now;
    fm1_timer5_ack();
    fm1_input_tick();
    now = fm1_ticks();
    acc += now - last;
    last = now;
    while (acc >= 24000u) {
        acc -= 24000u;
        ms++;
    }
    fm1_ms = ms;
    {
        static uint32_t div;               /* (10 kHz: the USB poll at 2 kHz, as Felucca's) */
        if (++div >= 5u) {
            div = 0;
            usb_poll();
        }
    }
}

/* Core 1: this firmware never starts it, but the one before it may have (Jangada, X0X, Felucca's 2ND CORE ON),
 * and not every reset stops it: a core 1 still running from before would execute whatever this firmware's
 * startup writes into the RAM under it. So the boot holds it first, as Felucca 1.6.1's does (its hal/fm1_sys.h,
 * fm1_cpu1_halt; the 1.5.1 hal built here has none): C1_CON bit 1 set, then bit 3 (enable) cleared; registers
 * only, harmless when it never ran. The bus-invalid traps are off around the hold: cutting core 1's read off
 * the bus raises an AXI read invalid a few instructions later */
#define C1_CON (*(volatile uint32_t *)0x1EEE004u)
#define DBG_WR_EN (*(volatile uint32_t *)0x1EEE240u)
#define DBG_MSG_CLR (*(volatile uint32_t *)0x1EEE248u)
#define DBG_EN (*(volatile uint32_t *)0x1EEE340u)
#define DBG_BUS 0x003F0030u                 /* AXI rd/wr (4, 5), C0/C1 fetch/rd/wr (16..21) */
static void cpu1_halt(void)
{
    uint32_t locked = !(DBG_WR_EN & 1u), en;
    volatile uint32_t n;
    if (locked)
        DBG_WR_EN = 0xE7u;                  /* unlock */
    en = DBG_EN;
    DBG_EN = en & ~DBG_BUS;
    __asm__ volatile("csync" ::: "memory");
    C1_CON |= 0x2u;
    C1_CON &= ~0x8u;
    __asm__ volatile("csync" ::: "memory");
    for (n = 0; n < 256u; n++)              /* the flag comes some instructions after the hold */
        ;
    DBG_MSG_CLR = DBG_BUS;
    __asm__ volatile("csync" ::: "memory");
    DBG_EN = en;
    if (locked && (DBG_WR_EN & 1u))
        DBG_WR_EN = 0xE7u;                  /* lock again */
    __asm__ volatile("csync" ::: "memory");
}

/* the ISR wrappers (Felucca's hal/fm1_isr.S) */
void isr_alnk0(void);
void isr_timer5(void);

/* ------------------------------------------------------------------------------ LCD --- */
static uint16_t fb[240 * 240];
static uint8_t lcd_up;                      /* lcd_init done: a crash can show itself */

static void lcd_cmd(uint8_t c, const uint8_t *d, uint32_t n)
{
    static uint8_t buf[8];
    uint32_t i;
    fm1_lcd_send_cmd(c);
    fm1_lcd_wait();
    if (n) {
        for (i = 0; i < n; i++)
            buf[i] = d[i];
        fm1_lcd_send_data(buf, n);
        fm1_lcd_wait();
    }
    fm1_lcd_deselect();
}

static void lcd_init(void)
{
    static const uint8_t c565 = 0x55, mad = 0x00;
    fm1_lcd_hw_init();
    fm1_delay_ms(80);
    lcd_cmd(0x01, 0, 0);
    fm1_delay_ms(150);
    lcd_cmd(0x11, 0, 0);
    fm1_delay_ms(120);
    lcd_cmd(0x3A, &c565, 1);
    lcd_cmd(0x36, &mad, 1);
    lcd_cmd(0x21, 0, 0);
    lcd_cmd(0x13, 0, 0);
    fm1_lcd_baud(4);
    lcd_cmd(0x29, 0, 0);
    lcd_up = 1;
}

static void lcd_frame(void)
{
    static const uint8_t win[4] = {0, 0, 0, 239};
    uint32_t y;
    lcd_cmd(0x2A, win, 4);
    lcd_cmd(0x2B, win, 4);
    fm1_lcd_send_cmd(0x2C);
    fm1_lcd_wait();
    for (y = 0; y < 240; y += 24) {          /* in strips: 11520 bytes a DMA */
        fm1_lcd_send_data(fb + y * 240, 240u * 24u * 2u);
        fm1_lcd_wait();
    }
    fm1_lcd_deselect();
}

/* RGB565, byte-swapped as the panel takes it */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3) >> 8 | \
                                 (((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3) << 8))
/* the C64's palette */
#define C_BLUE   RGB(0x35, 0x28, 0x79)
#define C_LBLUE  RGB(0x6C, 0x5E, 0xB5)
#define C_WHITE  RGB(0xFF, 0xFF, 0xFF)
#define C_CYAN   RGB(0x70, 0xA4, 0xB2)
#define C_YELLOW RGB(0xB8, 0xC7, 0x6F)
#define C_LGREEN RGB(0x9A, 0xD2, 0x84)
#define C_LRED   RGB(0x9A, 0x67, 0x59)
#define C_GREY   RGB(0x95, 0x95, 0x95)
#define C_DGREY  RGB(0x44, 0x44, 0x44)
#define C_DARK   RGB(0x20, 0x18, 0x4c)
#define C_ORANGE RGB(0xD8, 0x8C, 0x3C)
#define C_DARK2  RGB(0x30, 0x26, 0x6a)

static void fill(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if ((unsigned)i < 240u && (unsigned)j < 240u)
                fb[j * 240 + i] = c;
}

static int text(int x, int y, const char *s, uint16_t c)
{
    for (; *s; s++, x += 8) {
        unsigned ch = (unsigned char)*s;
        const uint8_t *g;
        int r, b;
        if (ch < 32 || ch > 126)
            ch = '?';
        g = FONT + (ch - 32u) * 12u;
        for (r = 0; r < 12; r++)
            for (b = 0; b < 8; b++)
                if (g[r] >> (7 - b) & 1)
                    if ((unsigned)(x + b) < 240u && (unsigned)(y + r) < 240u)
                        fb[(y + r) * 240 + x + b] = c;
    }
    return x;
}

static char *fmt_u(char *p, uint32_t v, int width)
{
    char t[10];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n < width) t[n++] = '0';
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

/* ---------------------------------------------------------------- crashes and boot loops --- */
/* A fault: the crash on the screen for 4 s, then a reset. A firmware that fails the same way at every boot
 * would reset forever, and the update mode (OCT- + OCT+ held, in the main loop) would never come: the boot
 * guard (as Felucca's) counts boots that die within their first 30 s, a crash or a hang (the watchdog), and
 * the third such boot goes to the USB update mode instead, where an installer can put another firmware on. */
static volatile struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
#define BOOTGUARD_MAGIC 0x53494447u         /* "SIDG" */

static void fm1_fault(const fm1_crash_t *c)
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t v[3] = {c->vec, c->pc, c->rets}, i, k, t0;
    char b[16];
    fm1_audio_stop();
    if (lcd_up) {
        fill(0, 0, 240, 240, C_BLUE);
        text(48, 40, "FM-1 SID CRASH", C_WHITE);
        for (i = 0; i < 3u; i++) {
            b[0] = "VPR"[i];
            b[1] = ' ';
            for (k = 0; k < 8u; k++)
                b[2 + k] = hex[(v[i] >> (28u - 4u * k)) & 15u];
            b[10] = 0;
            text(40, 80 + 20 * (int)i, b, C_LBLUE);
        }
        text(16, 160, "RESTARTING", C_WHITE);
        text(16, 180, "IF IT REPEATS: USB UPDATE", C_GREY);
        lcd_frame();
    }
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 24000u)
        fm1_wdt_feed();
    fm1_reboot();
}

/* the note nearest a frequency register (PAL), e.g. "C#4"; "---" when 0 */
static void note_name(uint32_t freq, char *out)
{
    static const char *const N[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    static const uint32_t Q8[12] = {4561991, 4833262, 5120662, 5425153, 5747749, 6089528,
                                    6451630, 6835264, 7241710, 7672325, 8128545, 8611893};   /* MIDI 84..95 */
    uint32_t v = freq << 8, best = 0, bd = 0xffffffffu, i;
    int oct = 6;
    if (!freq) { cat(out, "---"); return; }
    while (v < 4432000u) { v <<= 1; oct--; }           /* into C6 .. B6 (a quarter tone below C6) */
    while (v >= 8864000u) { v >>= 1; oct++; }
    for (i = 0; i < 12; i++) {
        uint32_t d = v > Q8[i] ? v - Q8[i] : Q8[i] - v;
        if (d < bd) { bd = d; best = i; }
    }
    if (best == 0 && v > 8300000u) { oct++; }          /* closer to the next C */
    out = cat(out, N[best]);
    if (oct < 0) oct = 0;
    *out++ = (char)('0' + (oct > 9 ? 9 : oct));
    *out = 0;
}

static void bar(int x, int y, int w, int h, uint32_t v, uint32_t max, uint16_t c)
{
    int f = (int)((uint32_t)w * (v > max ? max : v) / max);
    fill(x, y, w, h, C_DARK);
    fill(x, y, f, h, c);
}

static void badge(int x, int y, const char *s, int on, uint16_t c)
{
    int w = 0;
    const char *q = s;
    while (*q++) w += 8;
    fill(x - 2, y - 1, w + 4, 13, on ? c : C_DARK);
    text(x, y, s, on ? C_BLUE : C_DGREY);
}

#include "fx.c"

static const char *trk_name(uint32_t t)
{
    return trk_patch[t] == KIT ? "DRUM KIT" : SID_PATCHES[trk_patch[t]].name;
}

static char *midi_name(char *p, uint32_t n)
{
    static const char *const N[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    uint32_t o = n / 12u;
    p = cat(p, N[n % 12u]);
    *p++ = (char)('0' + (o ? (o - 1u > 9u ? 9u : o - 1u) : 0u));
    *p = 0;
    return p;
}

/* the sequencer's page: three tracks of 16 steps (a note's height in its cell is its pitch;
 * the kit shows K / S / H), the step playing or the cursor, that step's notes, the knobs */
static void draw_seq(void)
{
    static const uint16_t TC[3] = {C_LGREEN, C_CYAN, C_YELLOW};
    static const char *const DRUM[3] = {"K", "S", "H"};
    static const char *const KN[4] = {"CUT", "RES", "A", "R"};
    char line[64], *p;
    uint32_t t, s, at = seq_req ? seq_pos : cursor;
    int x;
    p = cat(line, "SEQ ");
    p = fmt_u(p, bpm, 1);
    cat(p, " BPM");
    x = text(2, 17, line, C_WHITE) + 8;
    x = text(x, 17, seq_req ? "PLAY" : "STOP", seq_req ? C_LGREEN : C_GREY) + 10;
    if (rec)
        badge(x, 17, "REC", 1, C_LRED);
    p = cat(line, octave < 0 ? "Oct-" : "Oct+");
    fmt_u(p, (uint32_t)(octave < 0 ? -octave : octave), 1);
    text(240 - 8 * 5 - 2, 17, line, C_GREY);
    for (t = 0; t < NTRK; t++) {
        int y = 32 + (int)t * 40;
        line[0] = t == sel ? '>' : ' ';
        line[1] = (char)('1' + t);
        line[2] = ' ';
        line[3] = 0;
        cat(line + 3, trk_name(t));
        text(2, y, line, t == sel ? C_WHITE : C_GREY);
        for (s = 0; s < NSTEP; s++) {
            int cx = (int)s * 15, cy = y + 14;
            uint32_t n = pat[t][s];
            fill(cx, cy, 14, 22, s % 4u ? C_DARK : C_DARK2);
            if (n && trk_patch[t] == KIT) {
                fill(cx + 1, cy + 3, 12, 16, TC[t]);
                text(cx + 3, cy + 5, DRUM[kit_drum(n) - 6u], C_BLUE);
            } else if (n) {
                int py = cy + 19 - (int)((n % 24u) * 15u / 24u);
                fill(cx + 2, py - 3, 10, 4, TC[t]);
            }
            if (seq_req && s == seq_pos) {
                fill(cx, cy, 14, 1, C_WHITE);
                fill(cx, cy + 21, 14, 1, C_WHITE);
            }
            if (!seq_req && t == sel && s == cursor)
                fill(cx, cy + 19, 14, 3, C_ORANGE);
        }
    }
    p = cat(line, "STEP ");
    p = fmt_u(p, at + 1u, 2);
    for (t = 0; t < NTRK; t++) {
        uint32_t n = pat[t][at];
        p = cat(p, "  ");
        if (!n)
            p = cat(p, "---");
        else if (trk_patch[t] == KIT)
            p = cat(p, kit_drum(n) == 6u ? "KCK" : kit_drum(n) == 7u ? "SNR" : "HAT");
        else
            p = midi_name(p, n);
    }
    text(2, 154, line, C_WHITE);
    x = 2;
    for (t = 0; t < 4; t++) {
        int32_t k = trk_knob[sel][t];
        p = cat(line, KN[t]);
        p = cat(p, k < 0 ? "-" : "+");
        fmt_u(p, (uint32_t)(k < 0 ? -k : k), 1);
        x = text(x, 168, line, C_CYAN) + 8;
    }
    text(2, 182, rec ? (seq_req ? "REC: keys land on the beat" : "REC: keys fill the cursor") :
                       "PLAY run  REC record  GLO clr", C_GREY);
}

static void trk_draw(void);

static void draw(void)
{
    static const char *const WAVES[4] = {"T", "S", "P", "N"};
    char line[64], *p;
    uint8_t env[3];
    uint32_t v, i, load;
    const psid_info_t *info = tune_info();
    if (trk && !fx_view) {
        trk_draw();
        return;
    }
    if (engine) {
        env[0] = own.v[0].env; env[1] = own.v[1].env; env[2] = own.v[2].env;
    } else {
        rs_env(env);
    }
    fill(0, 0, 240, 240, C_BLUE);
    /* header */
    fill(0, 0, 240, 14, C_LBLUE);
    text(2, 1, "FM-1 SID", C_WHITE);
    if (usb.config && !usb.suspended)       /* a computer has it as a MIDI device */
        text(2 + 8 * 9, 1, "USB", C_LGREEN);
    text(240 - 8 * 13 - 2, 1, engine ? "fm1-sid model" : "  reSID  0.16", C_YELLOW);
    if (mode && seq_view && !fx_view) {
        /* (the sequencer's page draws its own lines) */
    } else if (mode) {                     /* the patch, the knobs, the octave */
        static const char *const KN[4] = {"CUT", "RES", "A", "R"};
        int x = 2;
        const int32_t *kn = parts_req ? trk_knob[sel] : knob;
        if (parts_req) {
            line[0] = 'T'; line[1] = 'R'; line[2] = 'K'; line[3] = (char)('1' + sel); line[4] = ' '; line[5] = 0;
            text(text(2, 17, line, C_ORANGE), 17, trk_name(sel), C_WHITE);
        } else {
            text(text(2, 17, "SYNTH ", C_ORANGE), 17, SID_PATCHES[patch].name, C_WHITE);
        }
        for (i = 0; i < 4; i++) {
            int32_t k = kn[i];
            p = cat(line, KN[i]);
            p = cat(p, k < 0 ? "-" : "+");
            p = fmt_u(p, (uint32_t)(k < 0 ? -k : k), 1);
            x = text(x, 30, line, C_CYAN) + 8;
        }
        if (parts_req) {
            p = cat(line, seq_req ? "Seq " : "Stop ");
            p = fmt_u(p, bpm, 1);
        } else {
            p = cat(line, "Patch ");
            p = fmt_u(p, patch + 1u, 1);
            p = cat(p, "/");
            p = fmt_u(p, SID_NPATCHES, 1);
        }
        p = cat(p, octave < 0 ? " Oct-" : " Oct+");
        p = fmt_u(p, (uint32_t)(octave < 0 ? -octave : octave), 1);
        p = cat(p, model ? " 8580" : " 6581");
        text(2, 43, line, C_LGREEN);
    } else {                               /* the tune */
        text(2, 17, info->name, C_WHITE);
        p = cat(line, info->author);
        text(2, 30, line, C_CYAN);
        p = cat(line, "Tune ");
        p = fmt_u(p, play_idx + 1u, 1);
        p = cat(p, "/");
        p = fmt_u(p, ntunes, 1);
        p = cat(p, "  ");
        p = fmt_u(p, played_ms / 60000u, 1);
        p = cat(p, ":");
        p = fmt_u(p, played_ms / 1000u % 60u, 2);
        p = cat(p, model ? "  8580" : "  6581");
        p = cat(p, running ? "" : "  PAUSE");
        text(2, 43, line, C_LGREEN);
    }
    /* CPU: the audio ISR's share of its period */
    load = isr_halves ? isr_ticks / isr_halves * 100u / (HALF_FRAMES * 544u) : 0;
    isr_ticks = isr_halves = 0;            /* the next frame shows the last 40 ms */
    p = cat(line, "CPU ");
    p = fmt_u(p, load, 1);
    cat(p, "%");
    if (!(mode && seq_view && !fx_view))
        text(240 - 8 * 8, 43, line, C_GREY);
    if (fx_view) {
        draw_fx();
    } else if (mode && seq_view) {
        draw_seq();
    } else {
    /* voices */
    for (i = 0; i < 3; i++) {
        int y = 58 + (int)i * 36, w;
        uint8_t ctrl = regs[i * 7 + 4];
        uint32_t freq = regs[i * 7] | (uint32_t)regs[i * 7 + 1] << 8;
        uint32_t pw = (regs[i * 7 + 2] | (uint32_t)(regs[i * 7 + 3] & 15) << 8);
        char nn[8];
        line[0] = 'V'; line[1] = (char)('1' + i); line[2] = 0;
        text(2, y, line, C_WHITE);
        for (w = 0; w < 4; w++)
            badge(26 + w * 14, y, WAVES[w], ctrl >> (4 + w) & 1, C_YELLOW);
        badge(86, y, "SYNC", ctrl & 2, C_ORANGE);
        badge(126, y, "RING", ctrl & 4, C_ORANGE);
        badge(166, y, "G", ctrl & 1, C_LGREEN);
        badge(180, y, "F", regs[0x17] >> i & 1, C_CYAN);
        note_name(freq, nn);
        text(204, y, nn, C_WHITE);
        bar(2, y + 15, 170, 8, env[i], 255, C_LGREEN);
        bar(178, y + 15, 60, 8, (ctrl & 0x40) ? pw : 0, 4095, C_YELLOW);
    }
    /* filter */
    {
        int y = 168;
        uint32_t fc = (regs[0x15] & 7u) | (uint32_t)regs[0x16] << 3, mv = regs[0x18];
        text(2, y, "FILTER", C_WHITE);
        badge(58, y, "LP", mv & 0x10, C_CYAN);
        badge(82, y, "BP", mv & 0x20, C_CYAN);
        badge(106, y, "HP", mv & 0x40, C_CYAN);
        badge(130, y, "3OFF", mv & 0x80, C_LRED);
        p = cat(line, "VOL ");
        fmt_u(p, mv & 15u, 2);
        text(176, y, line, C_GREY);
        text(2, y + 14, "CUT", C_GREY);
        bar(30, y + 16, 140, 7, fc, 2047, C_CYAN);
        text(176, y + 14, "RES", C_GREY);
        bar(202, y + 16, 36, 7, regs[0x17] >> 4, 15, C_ORANGE);
    }
    }
    /* scope: the last 240 samples (every other one), scaled to the window's peak */
    {
        uint32_t w0 = scope_w - 240u, x;
        int prev = 220, peak = 512;
        fill(0, 198, 240, 42, C_DARK);
        for (x = 0; x < 240; x++) {
            int a = scope[(w0 + x) & 511u];
            if (a < 0) a = -a;
            if (a > peak) peak = a;
        }
        for (x = 0; x < 240; x++) {
            int yv = 219 - scope[(w0 + x) & 511u] * 19 / peak, a, b;
            if (yv < 199) yv = 199;
            if (yv > 239) yv = 239;
            a = prev < yv ? prev : yv;
            b = prev < yv ? yv : prev;
            fill((int)x, a, 1, b - a + 1, C_LGREEN);
            prev = yv;
        }
    }
    (void)v;
}

/* ---------------------------------------------------------------------------- tunes --- */
/* (re)start playlist entry idx; header: take the chip model from the tune */
static void start(uint32_t idx, int header)
{
    uint32_t t, i, song;
    running = 0;
    fm1_delay_ms(4);                        /* the ISR finishes the half it is in */
    mode = 0;
    play_idx = idx % ntunes;
    {
        const uint8_t *t = tune_ptr[play_idx];
        uint32_t len = tune_len[play_idx];
        song_tail = 0;
        if (tune_slot[play_idx] >= 0)       /* saved in flash: into RAM first */
            t = song_ram((uint32_t)tune_slot[play_idx], &len);
        gt = t && !memcmp(t, "GTS", 3);
        if (gt && gt2_load(G, t, len, on_write, 0))
            gt = 0;
        if (!gt && (!t || psid_load(P, t, len, on_write, 0)))
            psid_load(P, HOUSE, sizeof HOUSE, on_write, 0);   /* not one we play: ours instead */
    }
    if (header)
        model = (pack_flags >> 1 & 3u) ? (pack_flags >> 1 & 3u) - 1u : tune_info()->model;
    if (engine)
        sid_init(&own, model ? SID_8580 : SID_6581, SID_CLOCK_PAL, FS_HZ);
    else
        rs_init(model, 985248.0, 24000000.0 / 544.0);
    for (i = 0; i < 32; i++)
        regs[i] = 0;
    ev_head = ev_tail = 0;
    play_cyc = 0;
    cyc_frac = 0;
    call_base = 0;
    song = tune_song[play_idx] ? tune_song[play_idx] : tune_info()->start;
    t = gt ? gt2_init(G, song) : psid_init(P, song ? song : 1u);
    gen_cyc = (t / PSID_PAL_FRAME + 1u) * PSID_PAL_FRAME;
    played_ms = 0;
    running = 1;
}

/* the synth mode: the chip fresh, the four driver instances, the patches and knobs; the
 * sequencer's pattern, view and state carry over (an engine or chip change keeps it going) */

static void start_synth(void)
{
    uint32_t i;
    running = 0;
    fm1_delay_ms(4);
    mode = 1;
    if (engine)
        sid_init(&own, model ? SID_8580 : SID_6581, SID_CLOCK_PAL, FS_HZ);
    else
        rs_init(model, 985248.0, 24000000.0 / 544.0);
    for (i = 0; i < 32; i++)
        regs[i] = 0;
    for (i = 0; i < 27; i++)
        key_note[i] = 0;
    syn_held = 0;                           /* keys held across the switch sound */
    (void)fm1_input_note_edges();           /* (the player's, unheard) */
    ev_head = ev_tail = 0;
    cmd_head = cmd_tail = 0;
    play_cyc = 0;
    cyc_frac = 0;
    call_base = 0;
    for (i = 0; i <= POLY; i++) {
        sidsyn_init(&inst[i], 1, model ? SID_8580 : SID_6581, FS_HZ);
        inst[i].write = syn_write;
        inst[i].env = syn_env;
        inst[i].user = (void *)(uintptr_t)i;
        if (i < NTRK) {
            sidsyn_set_voice(&inst[i], (int)i);
            trk_on[i] = live[i] = 0;
        }
    }
    seq_on = 0;
    parts_req = parts = (uint8_t)(seq_view || seq_req || rec);
    if (parts)                              /* (the ISR is not running the synth yet) */
        for (i = 0; i < NTRK; i++)
            load_patch(i);
    else
        load_patch(POLY);
    syn_next = PSID_PAL_FRAME;
    running = 1;
    if (seq_req)
        syn_cmd(CMD_SEQ, 1, 0);
}

/* a key's note into the selected track: at the nearest step while it runs, else at the cursor,
 * which then moves on */
static void record(uint32_t n)
{
    uint32_t s;
    if (seq_req) {
        uint32_t len = step_q8() >> 8, left = seq_next - play_cyc;
        s = left > len / 2u ? seq_pos : (seq_pos + 1u) % NSTEP;
    } else {
        s = cursor;
        cursor = (cursor + 1u) % NSTEP;
    }
    pat[sel][s] = (uint8_t)n;
}

/* the synth mode's controls; e: button press edges */
static void synth_keys(uint32_t e)
{
    uint32_t cur = fm1_in.notes, held = syn_held, taps = fm1_input_note_edges() & ~cur & ~held, i, tgt;
    int32_t pre = fm1_enc_take(6), sl = fm1_enc_take(0), al = fm1_enc_take(1);
    uint8_t want;
    if (e & (1u << 11))                     /* SEQ: the sequencer's page */
        seq_view = !seq_view;
    if (e & (1u << 12)) {                   /* PLAY: run / stop */
        seq_req = !seq_req;
        if (seq_req)
            cursor = 0;
    }
    if (e & (1u << 13))                     /* REC */
        rec = !rec;
    for (i = 0; i < NTRK; i++)              /* SCL ENV LFO: track 1 2 3 */
        if (e & (1u << (3 + i)))
            sel = i;
    if (e & (1u << 7))                      /* GLO: clear the step */
        pat[sel][seq_req ? seq_pos : cursor] = 0;
    if (e & (1u << 10))                     /* ARP: clear the track */
        for (i = 0; i < NSTEP; i++)
            pat[sel][i] = 0;
    want = (uint8_t)(seq_view || seq_req || rec);
    if (want != parts_req) {
        parts_req = want;
        syn_cmd(CMD_PARTS, want, 0);
    }
    if (e & (1u << 12))
        syn_cmd(CMD_SEQ, seq_req, 0);
    tgt = parts_req ? sel : POLY;
    for (i = 0; i < 27; i++) {
        uint32_t bit = 1u << i;
        if ((cur & ~held & bit) || (taps & bit)) {   /* a press (a tap: pressed and gone again) */
            int32_t n = 53 + (int32_t)i + 12 * octave;
            key_note[i] = (uint8_t)n;
            key_tgt[i] = (uint8_t)tgt;
            syn_cmd(CMD_ON, (uint32_t)n, tgt);
            if (rec && tgt < NTRK)
                record((uint32_t)n);
        }
        if ((held & ~cur & bit) || (taps & bit))
            syn_cmd(CMD_OFF, key_note[i], key_tgt[i]);
    }
    syn_held = cur;
    if (seq_view && sl && !seq_req)         /* SELECT on the sequencer's page: the cursor */
        cursor = (cursor + NSTEP * 8u + (uint32_t)sl) % NSTEP;
    else
        pre += sl;
    if (pre && parts_req) {                 /* PRESETS: the track's patch (or the drum kit) */
        trk_patch[sel] = (uint8_t)((trk_patch[sel] + (KIT + 1u) * 8u + (uint32_t)pre) % (KIT + 1u));
        trk_knob[sel][0] = trk_knob[sel][1] = trk_knob[sel][2] = trk_knob[sel][3] = 0;
        syn_cmd(CMD_PATCH, 0, sel);
    } else if (pre) {                       /* PRESETS: the polyphonic synth's patch */
        patch = (patch + SID_NPATCHES * 8u + (uint32_t)pre) % SID_NPATCHES;
        knob[0] = knob[1] = knob[2] = knob[3] = 0;
        syn_cmd(CMD_PATCH, 0, POLY);
    }
    if (al) {                               /* ALGORITHM: the tempo */
        int32_t b = (int32_t)bpm + 2 * al;
        bpm = (uint32_t)(b < 40 ? 40 : b > 240 ? 240 : b);
    }
    if (e & (1u << 1) && octave < 2) octave++;
    if (e & (1u << 0) && octave > -2) octave--;
    for (i = 0; i < 4; i++) {
        int32_t d = fm1_enc_take(2 + i), lim = i ? 15 : 64, *k = parts_req ? trk_knob[sel] : knob;
        if (!d)
            continue;
        k[i] += d;
        if (k[i] > lim) k[i] = lim;
        if (k[i] < -lim) k[i] = -lim;
        syn_knobs(parts_req ? sel : POLY);
    }
}

#include "tracker.c"

/* MASTER: the pot (SARADC channel 4), read from the main loop as Felucca's src/master.c does:
 * x16, smoothed (an eighth of the way a read), its square the level (fx.c's output, Q12) */
static int32_t master_knob = -1;
static void master_poll(void)
{
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    uint32_t k;
    if (a < 0)
        return;
    master_knob = master_knob < 0 ? a * 16 : master_knob + (a * 16 - master_knob) / 8;
    k = (uint32_t)master_knob / 16u;
    fx_master = (int32_t)((k * k) >> 8);
}

/* the LEDs: a key or button lights while held; PLAY while playing, REC while recording, FX on
 * the effects page, SEQ in the editor or on the sequencer's page, SCL / ENV / LFO the sequencer's
 * track, LFO the editor's channel muted. The others that do something on
 * this page glow dim (fm1_input.h fm1_led_dim), so the black FM-1's buttons can be found in the
 * dark: the player's OCT- OCT+ FX EDIT HOME SEQ PLAY, the synth's buttons and keys, all of them in
 * the editor; on the FX page FX EDIT HOME (and the synth's keys); in USB mode HOME OCT- OCT+ */
static uint8_t in_usb;                      /* usb_mode is running */
static void leds(void)
{
    static uint32_t lit_b, lit_n, dim_b, dim_n, ready;
    uint32_t b = fm1_in.buttons, n = fm1_in.notes, gb, gn = 0, c, r;
    uint8_t nl[FM1_NCOL] = {0}, nd[FM1_NCOL] = {0};
    if (in_usb) {
        gb = 1u << 8 | 3u;
    } else if (fx_view) {
        gb = 1u << 2 | 1u << 6 | 1u << 8;
        gn = mode && !trk ? 0x7FFFFFFu : 0u;
    } else if (trk) {
        gb = 0x3FFFu;
        gn = 0x7FFFFFFu;
    } else if (mode) {
        gb = 0x3FFFu & ~(1u << 9);          /* (SAVE: nothing here) */
        gn = 0x7FFFFFFu;
    } else {
        gb = 3u | 1u << 2 | 1u << 6 | 1u << 8 | 1u << 11 | 1u << 12;
    }
    if (!in_usb && (trk ? gt2_playing(G) : mode ? seq_on : running))
        b |= 1u << 12;
    if (!in_usb && (trk ? tk_rec : mode && rec))
        b |= 1u << 13;
    if (!in_usb && fx_view)
        b |= 1u << 2;
    if (!in_usb && (trk || (mode && seq_view)))
        b |= 1u << 11;
    if (!in_usb && !trk && mode && parts_req)   /* SCL / ENV / LFO: the sequencer's track */
        b |= 1u << (3u + sel);
    if (!in_usb && trk && G->chn[tk_chn()].mute)   /* LFO: the editor's channel is muted */
        b |= 1u << 5;
    gb &= ~b;
    gn &= ~n;
    if (ready && b == lit_b && n == lit_n && gb == dim_b && gn == dim_n)
        return;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 1; r < 5u; r++) {
            int id = FM1_KEYMAP[r][c];
            uint32_t on, glow;
            if (id < 0)
                continue;
            on = id < 14 ? b >> id & 1u : n >> (id - 14) & 1u;
            glow = id < 14 ? gb >> id & 1u : gn >> (id - 14) & 1u;
            nl[c] |= (uint8_t)(on << r);
            nd[c] |= (uint8_t)(glow << r);
        }
    for (c = 0; c < FM1_NCOL; c++)          /* (the glow first: lit -> glow is never dark) */
        fm1_led_dim[c] = nd[c];
    for (c = 0; c < FM1_NCOL; c++)
        fm1_led[c] = nl[c];
    lit_b = b; lit_n = n; dim_b = gb; dim_n = gn;
    ready = 1;
}

/* ------------------------------------------------------------------------ MIDI in --- */
/* USB-MIDI's notes: the synth (as its keys, at their own pitch) or the editor (firmware/tracker.c);
 * a program change: the player's tune */
static uint8_t midi_tgt[128];               /* the synth instance each note plays */

static void midi_note(uint32_t n, int on)
{
    if (trk) {
        trk_midi(n, on);
    } else if (mode && on) {
        uint32_t tgt = parts_req ? sel : POLY;
        midi_tgt[n] = (uint8_t)tgt;
        syn_cmd(CMD_ON, n, tgt);
        if (rec && tgt < NTRK)
            record(n);
    } else if (mode) {
        syn_cmd(CMD_OFF, n, midi_tgt[n]);
    }
}

static void midi_in(void)
{
    if (midi_in_overflow) {                 /* a broken stream: dropped, the notes let go */
        mi_r = mi_w;
        midi_in_overflow = 0;
        if (mode && !trk)
            syn_cmd(CMD_ALLOFF, 0, POLY);
        return;
    }
    while (mi_r != mi_w) {
        uint32_t pkt, st, d1, d2;
        RING_PUBLISH();
        pkt = midi_in_q[mi_r % MQ];
        RING_PUBLISH();
        mi_r++;
        st = pkt >> 8 & 0xF0u;
        d1 = pkt >> 16 & 0x7Fu;
        d2 = pkt >> 24 & 0x7Fu;
        if (st == 0x90u && d2)
            midi_note(d1, 1);
        else if (st == 0x80u || st == 0x90u)
            midi_note(d1, 0);
        else if (st == 0xC0u && !mode && !trk)
            start(d1 % ntunes, 1);
    }
}

#if FELUCCA_OTA
/* ------------------------------------------------------------------ an update (ota.c) --- */
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    const char *t = STEP[step < 6u ? step : 0];
    char b[24];
    uint32_t n = 0, t0;
    fill(0, 0, 240, 240, C_BLUE);
    text(96, 92, "UPDATE", C_WHITE);
    if (step >= 9u) {                       /* the session's end without an update: why */
        uint32_t v = (uint32_t)-code;
        cat(b, code == 1 ? "DRY RUN OK" : "FAILED -");
        while (b[n])
            n++;
        if (code != 1) {
            if (v >= 10u)
                b[n++] = (char)('0' + v / 10u % 10u);
            b[n++] = (char)('0' + v % 10u);
            b[n] = 0;
        }
        t = b;
    }
    while (t[n])
        n++;
    text((int)(240u - 8u * n) / 2, 124, t, C_YELLOW);
    lcd_frame();
    if (step >= 9u)
        for (t0 = ms; ms - t0 < 1500u;)
            fm1_wdt_feed();
}

static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;                  /* (asked for: not a failed boot) */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);                 /* the record into RAM, a core reset (fm1_sys.h) */
}

/* the installer's upgrade command: the session, the sound off while it runs (the audio
 * interrupt renders the SID: it would hold the last note and take the CPU); it returns only
 * when nothing was installed, and FM-1 SID carries on */
static void ota_go(void)
{
    uint32_t i;
    fm1_audio_stop();
    ota_session();
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    FM1_ALNK_CON0 |= 0x800u;                /* (the DMA on again: fm1_audio_stop's bit) */
}
#endif

/* ------------------------------------------------------------------------- USB mode --- */
/* Everything off but USB: no sound (the audio interrupt stopped), no player, no SID, only the
 * USB driver (the TIMER5 interrupt) and, in the main loop, its requests: an installer's
 * handshake and update (FELUCCA_OTA), the soft key. Whatever load the player puts on the CPU,
 * here nothing can keep an installer from reaching FM-1 SID. HOME held 5 s, HOME held while
 * switching on, or the third of three boots in a row that die early; HOME again leaves (back
 * where it was: the sound on again, the editor's song as it was). The screen counts what USB
 * does, to tell a dead USB from a busy CPU. */
static uint8_t boot_usb;                    /* fm1_cstart: the boot guard asks for USB mode */

static void usb_mode_draw(uint32_t hs)
{
    char b[40], *p;
    fill(0, 0, 240, 240, C_BLUE);
    fill(0, 0, 240, 14, C_LBLUE);
    text(2, 1, "FM-1 SID", C_WHITE);
    text(2, 30, "USB MODE", C_YELLOW);
    text(2, 50, "SOUND AND PLAYER OFF:", C_WHITE);
    text(2, 66, "AN INSTALLER CAN REACH", C_WHITE);
    text(2, 82, "FM-1 SID NOW", C_WHITE);
    p = cat(b, "USB ");
    cat(p, !usb.up ? "OFF (NO CABLE?)" : usb.config && !usb.suspended ? "CONNECTED" : "WAITING");
    text(2, 112, b, usb.config ? C_LGREEN : C_ORANGE);
    p = cat(b, "IN ");
    p = fmt_u(p, usb.rx_pkts, 0);
    p = cat(p, "  OUT ");
    fmt_u(p, usb.tx_pkts, 0);
    text(2, 130, b, C_LBLUE);
    p = cat(b, "SETUPS ");
    p = fmt_u(p, usb.setups, 0);
    p = cat(p, "  ID ");
    fmt_u(p, hs, 0);
    text(2, 146, b, C_LBLUE);
    text(2, 190, "HOME: BACK", C_GREY);
    text(2, 206, "OCT- OCT+ 3 S: UBOOT", C_GREY);
    lcd_frame();
}

static void usb_mode(void)
{
    uint32_t last = 0, hold = 0, hs = 0, rel, e, i;
    int armed = 0;                          /* HOME let go once (the hold that came here) */
    fm1_audio_stop();
    in_usb = 1;
    for (;;) {
        fm1_wdt_feed();
        if (bootguard.pending && ms > 30000u)
            bootguard.pending = bootguard.failed = 0;   /* (as the main loop) */
        usb_retry(ms);
        if (usb.uboot_req) {
            bootguard.pending = 0;
            fm1_enter_uboot();
        }
#if FELUCCA_OTA
        {
            uint32_t w = so_w;
            ota_service();
            if (so_w != w)
                hs++;                       /* (an identity went out) */
        }
        if (usb.ota_req) {
            usb.ota_req = 0;
            ota_session();                  /* returns only when nothing was installed */
            last = 0;
        }
#endif
        mi_r = mi_w;                        /* (notes: nothing plays them here) */
        e = fm1_input_edges(&rel);
        if (!(fm1_in.buttons & (1u << 8)))
            armed = 1;
        else if (armed && (e & (1u << 8)))
            break;
        if ((fm1_in.buttons & 3u) == 3u) {  /* OCT- and OCT+ 3 s: the chip's update mode, as always */
            if (!hold)
                hold = ms;
            else if (ms - hold > 3000u) {
                bootguard.pending = 0;
                fm1_enter_uboot();
            }
        } else {
            hold = 0;
        }
        leds();
        if (!last || ms - last >= 200u) {
            last = ms;
            usb_mode_draw(hs);
        }
        __asm__ volatile("idle");
    }
    in_usb = 0;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    FM1_ALNK_CON0 |= 0x800u;                /* the sound on again (fm1_audio_stop's bit) */
}

/* ----------------------------------------------------------------------------- main --- */
static void fm1_main(void)
{
    uint32_t last_draw = 0, last_ms = 0, hold = 0, home_hold = 0;
    uint8_t home_skip = 0;                  /* the press that left USB mode: not a HOME */
    lcd_init();
    fm1_input_init();
    fm1_adc_init();
    (void)fm1_adc_read(FM1_ADC_MASTER);     /* (the first conversion after the pin setup: dropped) */
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    fm1_timer5_start(isr_timer5, 4);
    fm1_irq_enable_all();
    usb_start();
    {
        uint32_t t0 = ms;                   /* (the keys' first scans: debounced in ~10 ms) */
        while (ms - t0 < 60u)
            fm1_wdt_feed();
    }
    if (boot_usb || (fm1_in.buttons & (1u << 8)))
        usb_mode();                         /* HOME held while switching on, or the boot guard */
    master_poll();                          /* (the level as the knob stands, before a tune starts) */
    load_pack();
    st_init((uint32_t)pack_end - 0x02000000u + 0x4000u);   /* (flash offset of the XIP address) */
#if FELUCCA_OTA
    ota_boot_cleanup();                     /* an update's staged loader left over: erased (as Felucca) */
#endif
    list_store();
    engine = pack_flags & 1u;
    start(first_tune, 1);
    if (pack_flags & 8u)
        start_synth();
    home_skip = (uint8_t)((fm1_in.buttons >> 8) & 1u);   /* (HOME still down from USB mode at boot) */
    for (;;) {
        uint32_t rel, e, now;
        int32_t step;
        fm1_wdt_feed();
        if (bootguard.pending && ms > 30000u)
            bootguard.pending = bootguard.failed = 0;   /* 30 s up: this boot made it */
        generate();
        usb_retry(ms);                      /* (a cable plugged in later) */
        if (usb.uboot_req) {                /* the soft key over USB: the update mode */
            fm1_audio_stop();
            bootguard.pending = 0;
            fm1_enter_uboot();
        }
#if FELUCCA_OTA
        ota_service();                      /* an installer's handshake: the identity */
        if (usb.ota_req) {                  /* its upgrade command */
            usb.ota_req = 0;
            ota_go();
        }
#endif
        midi_in();
        now = ms;
        if (running)
            played_ms += now - last_ms;
        last_ms = now;
        e = fm1_input_edges(&rel);
        if ((rel & (1u << 8)) && !home_skip && home_hold && now - home_hold < 1000u) {
            /* HOME (a short press, on its release: held 5 s it is USB mode): the player <-> the
             * synth; the editor: leave */
            if (trk) {
                if (!tk_dirty || tk_ask(8, "not saved: HOME again leaves"))
                    tk_leave();
            } else if (mode) {
                start(play_idx, 0);
            } else {
                start_synth();
            }
            e &= ~(1u << 8);
        }
        if (e & (1u << 2))                  /* FX: the effects page */
            fx_view = !fx_view;
        if (fx_view && fx_keys()) {         /* (its encoders are the page's) PRESETS: the chip */
            model = !model;
            if (trk) tk_chip(); else if (mode) start_synth(); else start(play_idx, 0);
        }
        if (trk) {
            if (!fx_view)
                trk_keys(e);
            trk_follow();
        } else if (mode) {
            synth_keys(e);
        } else if (e & (1u << 11)) {        /* SEQ: the GoatTracker editor */
            trk_enter(tune_slot[play_idx]);
        } else {
            step = fm1_enc_take(6) + fm1_enc_take(0);
            if (e & (1u << 1)) step++;
            if (e & (1u << 0)) step--;
            if (step) {
                start((play_idx + ntunes * 8u + (uint32_t)step) % ntunes, 1);
            }
            if (e & (1u << 12))
                running = !running;
        }
        if (e & (1u << 6)) {
            engine = !engine;
            if (trk) tk_chip(); else if (mode) start_synth(); else start(play_idx, 0);
        }
        /* HOME held 5 s: USB mode */
        if (fm1_in.buttons & (1u << 8)) {
            if (!home_hold)
                home_hold = now ? now : 1u;
            else if (now - home_hold > 5000u && !home_skip) {
                usb_mode();
                home_skip = 1;              /* (HOME is down again: the press that left it) */
                last_draw = 0;
            }
        } else {
            home_hold = 0;
            home_skip = 0;
        }
        /* OCT- and OCT+ held 3 s: the USB update mode */
        if ((fm1_in.buttons & 3u) == 3u) {
            if (!hold) hold = now;
            else if (now - hold > 3000u) {
                fm1_audio_stop();
                bootguard.pending = 0;      /* (asked for: not a failed boot) */
                fm1_enter_uboot();
            }
        } else {
            hold = 0;
        }
        leds();
        if (now - last_draw >= 40u) {
            last_draw = now;
            master_poll();
            draw();
            lcd_frame();
        }
        /* sleep until the next interrupt (TIMER5 ticks at 10 kHz): saves power on the FM-1,
         * and lets an emulator with idle skipping jump ahead instead of running the loop */
        __asm__ volatile("idle");
    }
}

void fm1_cstart(void)
{
    extern uint32_t _bss_start[], _bss_end[], _data_start[], _data_end[], _data_load[];
    extern uint32_t _rt_start[], _rt_end[], _rt_load[];
    typedef void (*ctor_t)(void);
    extern ctor_t __init_array_start[], __init_array_end[];
    uint32_t *w;
    ctor_t *c;
    cpu1_halt();                            /* a core 1 left running by the firmware before */
    fm1_time_init();
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = bootguard.pending = 0;
    }
    if (bootguard.pending)                  /* the last boot died within its first 30 s */
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 3u) {           /* and USB mode died too: the chip's update mode */
        bootguard.failed = bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (w = _bss_start; w < _bss_end; w++)
        *w = 0;
    for (w = _data_start; w < _data_end; w++)
        *w = _data_load[w - _data_start];
    for (w = _rt_start; w < _rt_end; w++)  /* (the flash code, firmware/store.c) */
        *w = _rt_load[w - _rt_start];
    __asm__ volatile("csync" ::: "memory");
    for (c = __init_array_start; c < __init_array_end; c++)
        (*c)();
    fm1_mailbox_clear();
    boot_usb = bootguard.failed == 2u;      /* the third boot in a row that dies early: USB mode */
    fm1_main();
}
