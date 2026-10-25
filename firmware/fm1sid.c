/* firmware/fm1sid.c - a SID player for the M-VAVE FM-1 on Dinghy 0.3 (fm1-sid/dinghy): C64 tunes
 * (PSID, RSID) played by their own 6502 code (player/psid.c, player/cpu6510.h) on the FM-1's CPU, and
 * GoatTracker 2 songs (.sng) by GoatTracker's playroutine (player/gt2.c); the SID rendered by reSID 0.16
 * (fast) or by fm1-sid's own model (src/sid.c), the three voices, sync / ring and the filter drawn on the
 * 240x240 LCD.
 *
 * The platform is Dinghy's (dinghy/src/platform.c, as Scintilla runs on real FM-1s): boot and its guard,
 * the USB rescue (OCT- held at power-on: no sound, only the updater), the installers' update path,
 * USB-MIDI and USB audio, the key scan and LEDs, MASTER (the volume), the crash screen. This file is the
 * app: app_audio (the audio interrupt, 256 frames) renders the SID, app_main is the player, the synth, the
 * editor and the screen, calling platform_poll often. GPL-3.0-only (Dinghy's, Felucca's and SLOOP's code).
 *
 * Two modes, HOME switches. PLAYER: PRESETS / SELECT encoder or OCT- / OCT+ = tune, ALGORITHM =
 * the tune's subtune (a SID or .sng of several), PLAY = pause. SYNTH: the note keys play fm1-sid's instrument driver (src/sidsynth.c) on the same
 * chip, PRESETS / SELECT = patch, OCT- / OCT+ = octave, KNOB1..4 = cutoff, resonance, attack,
 * release. Its step sequencer: SEQ = its page, PLAY = run / stop, REC = record, SCL / ENV / LFO =
 * track 1 / 2 / 3 (PRESETS then picks the track's patch), ALGORITHM = tempo, GLO = clear the step,
 * ARP = clear the track. Both: EDIT = engine (reSID / own); FX = the effects page (firmware/fx.c:
 * KNOB1 chorus, KNOB2 delay, KNOB3 its time, KNOB4 reverb, SELECT the delay's feedback, PRESETS
 * 6581 / 8580; FX again leaves it). SEQ in the player: the GoatTracker editor (firmware/tracker.c) on
 * the song playing; its songs are saved in flash (firmware/store.c) and join the player's list. The
 * keys and buttons light while held, PLAY / REC / FX / SEQ also while on, and the others that do
 * something on the page glow dim (leds). USB-MIDI: notes play the synth and the editor, a program
 * change picks the player's tune.
 *
 * Timing: the main loop runs the play routine up to three frames ahead and queues its SID writes with
 * their C64 cycle; the audio interrupt clocks the SID engine to each write's cycle and applies it. The
 * screen is a 4-bit framebuffer in the C64's colours: only the rows that changed since the last frame
 * go to the LCD (12 MHz SPI: a whole frame would be ~77 ms), a strip at a time, the play routine fed
 * between the strips (lcd_frame).
 * SPDX-License-Identifier: GPL-3.0-only */
#define APP_NAME "FM-1 SID"
#ifndef DOCK_BOOT
#define DOCK_BOOT 0                        /* 1: built for fm1-dock's boot slot (firmware/build_dinghy.sh) */
#endif
#include <stdint.h>
#ifdef PLATFORM_DOCK                      /* fm1-dock's module (dock/): the host's table as the platform */
#include "dock_platform.h"
#else
#define PLATFORM_DINGHY 1
/* the flash the platform's driver may erase and program: Dinghy's (its update area, its settings
 * sector) and FM-1 SID's song store (firmware/store.c: st_lo, st_hi, set at boot) */
static uint32_t st_lo, st_hi;
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, SETTINGS_AT, SETTINGS_AT + 0x1000u) || \
                             FL_IN(off, n, st_lo, st_hi))
#include "platform.c"
#endif
#include "sid.h"
#include "psid.h"
#include "gt2.h"
#include "sidsynth.h"
#define ms fm1_ms                           /* milliseconds since power-on (the platform's TIMER5) */

#include "assets.h"

/* ------------------------------------------------------------------------- the tunes --- */
/* A tune pack appended to the image (tools/sidpack.py, or the web page): "SIDPACK1", u32
 * count, u32 start (bits 0-15 the first entry, bit 16 the own model instead of reSID, bits
 * 17-18 the chip for every tune: 0 the tune's header, 1 6581, 2 8580, bit 19 start in the
 * synth mode), then per entry u32
 * offset (from the pack), length, song (0: the tune's start song); little-endian. An entry is a
 * PSID / RSID file or a GoatTracker 2 song (.sng: song = its subtune, from 1). Without one: the
 * firmware's own tune (tools/housetune.py). */
#ifdef PACK_AFTER_IMAGE                     /* FM-1 SID's own firmware (dock/solo): the pack right after this module's
                                            * image in flash, where tools/build.py --pack and the web page put it */
extern const uint8_t _hb_rt_load[], _hb_rt_start[], _hb_rt_end[];
static const uint8_t *pack_at;
#define _pack pack_at
static const uint8_t *find_pack(void)
{
    const uint8_t *p = _hb_rt_load + (_hb_rt_end - _hb_rt_start);   /* the image's end: the last load image's */
    uint32_t i;
    p = (const uint8_t *)(((uint32_t)p + 3u) & ~3u);
    for (i = 0; i < 0x4000u; i += 4u)       /* (a little padding between: the slot's, the app's) */
        if (!memcmp(p + i, "SIDPACK1", 8))
            return p + i;
    return (const uint8_t *)"none";         /* no pack: the own tune */
}
#else
extern const uint8_t _pack[];
#endif
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
#ifdef PACK_AFTER_IMAGE
    pack_at = find_pack();
#endif
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

#define BLK 128u                           /* the SID rendered a block at a time (two to a Dinghy half) */
#define FS_HZ FS                           /* Dinghy's 44100 Hz */
#define CPS_Q16 1464161u                   /* SID cycles per output sample, Q16: 985248 / 44100 */
#define OUT_SHIFT 0                        /* fx.c's output is Q15: Dinghy scales it (MASTER) */

/* ------------------------------------------------------------------------------- state --- */
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
static uint32_t cur_song, song_req;         /* the subtune playing; one ALGORITHM asked for (start) */

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
#define HALF_CYC 2860u                      /* SID cycles in a 128-frame block, rounded up */

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
    /* behind the sound (a play routine slower than real time, a long stall): on from now, the
     * time lost dropped (unsigned, gen_cyc - play_cyc wrapped and the tune stopped for good: an
     * RSID on an FM-1, whose CPU has about half the time fm1-emu gave it, 2026-10-24) */
    uint32_t calls = 0;
    if (running && !mode && (int32_t)(gen_cyc - play_cyc) < 0)
        gen_cyc = play_cyc;
    /* (two calls at most a time: a tune whose play routine is slower than real time still leaves
     * the main loop its keys and its screen) */
    while (running && !mode && calls++ < 2u && (int32_t)(gen_cyc - play_cyc) < (int32_t)((trk ? 2u : 3u) * PSID_PAL_FRAME) &&
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
    while (got < BLK) {
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
            got += (uint32_t)rs_clock(&left, out + got, (int)(BLK - got));
            play_cyc += (uint32_t)(dt - left);
        }
    }
}

static void render_own(short *out)
{
    static int32_t tmp[BLK];
    uint32_t got = 0, i;
    while (got < BLK) {
        uint32_t t = ev_tail, k = BLK - got;
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
            if (k > BLK - got)
                k = BLK - got;
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

/* the audio interrupt (Dinghy's ALNK0 handler): n stereo frames, Q15, two blocks of the SID */
static void app_audio(int32_t *out, uint32_t n)
{
    static short s[BLK];
    uint32_t t0 = fm1_ticks(), b, i;
    for (b = 0; b + BLK <= n; b += BLK) {
        if (running) {
            if (mode)
                syn_step();
            if (engine)
                render_own(s);
            else
                render_resid(s);
        } else {
            for (i = 0; i < BLK; i++)
                s[i] = 0;
        }
        fx_block(s, out + 2u * b, BLK);
        for (i = 1; i < BLK; i += 2u)
            scope[scope_w++ & 511u] = s[i];
        isr_halves++;
    }
    isr_ticks += fm1_ticks() - t0;
}

/* ------------------------------------------------------------------------------ LCD --- */
/* The picture is drawn into fb, 4 bits a pixel (the C64's 16 colours, two pixels a byte), then
 * lcd_frame sends the rows that differ from what the panel shows (fb_shown), as RGB565 strips
 * through Dinghy's lcd_blit (a strip's DMA runs while the next one is made), feeding the play routine
 * between the strips: a page that changes little costs little, and a whole new page (~77 ms at
 * 12 MHz) never starves the tune. */
#undef C_BLACK
#undef C_WHITE
#undef RGB
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3) >> 8 | \
                                 (((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3) << 8))   /* (byte-swapped) */
/* the C64's palette (the colours the screen uses), as indices into PAL */
enum { C_BLUE, C_LBLUE, C_WHITE, C_CYAN, C_YELLOW, C_LGREEN, C_LRED, C_GREY, C_DGREY, C_DARK, C_ORANGE,
       C_DARK2, C_BLACK };
static const uint16_t PAL[16] = {
    RGB(0x35, 0x28, 0x79), RGB(0x6C, 0x5E, 0xB5), RGB(0xFF, 0xFF, 0xFF), RGB(0x70, 0xA4, 0xB2),
    RGB(0xB8, 0xC7, 0x6F), RGB(0x9A, 0xD2, 0x84), RGB(0x9A, 0x67, 0x59), RGB(0x95, 0x95, 0x95),
    RGB(0x44, 0x44, 0x44), RGB(0x20, 0x18, 0x4c), RGB(0xD8, 0x8C, 0x3C), RGB(0x30, 0x26, 0x6a), 0, 0, 0, 0,
};
#define STRIP 16u                           /* rows a DMA: 7.5 KB */
static uint8_t fb[240 * 120], fb_shown[240 * 120];
#ifndef LCD_STRIPS
#define LCD_STRIPS 2u                       /* (Dinghy's lcd_blit: a strip's DMA runs while the next is made) */
#endif
static uint16_t strip[LCD_STRIPS][240 * STRIP];
static uint8_t shown_ok;                    /* fb_shown is what the panel shows */
static void generate(void);

static void lcd_frame(void)
{
    uint32_t y = 0, k = 0, i;
    while (y < 240u) {
        uint32_t h = 0;
        while (y < 240u && shown_ok && !memcmp(fb + y * 120u, fb_shown + y * 120u, 120u))
            y++;                            /* (rows as shown: skipped) */
        while (y + h < 240u && h < STRIP && (!shown_ok || memcmp(fb + (y + h) * 120u, fb_shown + (y + h) * 120u, 120u)))
            h++;
        if (!h)
            break;
        for (i = 0; i < 120u * h; i++) {    /* (strip k is free: lcd_blit waited for the one before it) */
            uint8_t v = fb[y * 120u + i];
            strip[k][2u * i] = PAL[v & 15u];
            strip[k][2u * i + 1u] = PAL[v >> 4];
        }
        memcpy(fb_shown + y * 120u, fb + y * 120u, 120u * h);
        lcd_blit(0, y, 240, h, strip[k]);
        k = (k + 1u) % LCD_STRIPS;
        y += h;
        generate();                         /* (the tune's look-ahead topped up while the strip goes out) */
    }
    shown_ok = 1;
}

static inline void pset(uint32_t x, uint32_t y, uint32_t c)
{
    uint8_t *p = &fb[y * 120u + (x >> 1)];
    *p = (uint8_t)(x & 1u ? (*p & 0x0Fu) | c << 4 : (*p & 0xF0u) | c);
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if ((unsigned)i < 240u && (unsigned)j < 240u)
                pset((uint32_t)i, (uint32_t)j, c);
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
                        pset((uint32_t)(x + b), (uint32_t)(y + r), c);
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
        if (info->songs > 1u) {            /* a tune of several subtunes: which one (ALGORITHM turns it) */
            p = fmt_u(line, play_idx + 1u, 1);
            p = cat(p, "/");
            p = fmt_u(p, ntunes, 1);
            p = cat(p, " Song ");
            p = fmt_u(p, cur_song, 1);
            p = cat(p, "/");
            p = fmt_u(p, info->songs, 1);
            p = cat(p, " ");
            p = fmt_u(p, played_ms / 60000u, 1);
            p = cat(p, ":");
            p = fmt_u(p, played_ms / 1000u % 60u, 2);
            p = cat(p, running ? "" : " II");
        } else {
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
        }
        text(2, 43, line, C_LGREEN);
    }
    /* CPU: the audio ISR's share of its period */
    load = isr_halves ? isr_ticks / isr_halves * 100u / (BLK * 544u) : 0;
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
    song = song_req ? song_req : tune_song[play_idx] ? tune_song[play_idx] : tune_info()->start;
    song_req = 0;
    cur_song = song ? song : 1u;
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

/* the LEDs: a key or button lights while held; PLAY while playing, REC while recording, FX on
 * the effects page, SEQ in the editor or on the sequencer's page, SCL / ENV / LFO the sequencer's
 * track, LFO the editor's channel muted. The others that do something on
 * this page glow dim (fm1_input.h fm1_led_dim), so the black FM-1's buttons can be found in the
 * dark: the player's OCT- OCT+ FX EDIT HOME SEQ PLAY, the synth's buttons and keys, all of them in
 * the editor; on the FX page FX EDIT HOME (and the synth's keys) */
static void leds(void)
{
    static uint32_t lit_b, lit_n, dim_b, dim_n, ready;
    uint32_t b = fm1_in.buttons, n = fm1_in.notes, gb, gn = 0, c, r;
    uint8_t nl[FM1_NCOL] = {0}, nd[FM1_NCOL] = {0};
    if (fx_view) {
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
    if ((trk ? gt2_playing(G) : mode ? seq_on : running))
        b |= 1u << 12;
    if ((trk ? tk_rec : mode && rec))
        b |= 1u << 13;
    if (fx_view)
        b |= 1u << 2;
    if ((trk || (mode && seq_view)))
        b |= 1u << 11;
    if (!trk && mode && parts_req)   /* SCL / ENV / LFO: the sequencer's track */
        b |= 1u << (3u + sel);
    if (trk && G->chn[tk_chn()].mute)   /* LFO: the editor's channel is muted */
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
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        st &= 0xF0u;
        if (st == 0x90u && d2)
            midi_note(d1, 1);
        else if (st == 0x80u || st == 0x90u)
            midi_note(d1, 0);
        else if (st == 0xC0u && !mode && !trk)
            start(d1 % ntunes, 1);
    }
}

/* ----------------------------------------------------------------------------- main --- */
/* Dinghy's USB rescue now, as at power-on (no sound, nothing of FM-1 SID: only the updater, so an
 * installer always reaches it): from OCT- held as FM-1 SID starts (the platform's own look at boot
 * is 50 ms, and on a real FM-1 it was missed, 2026-10-24) or HOME held 5 s. Its loop polls USB itself:
 * the interrupts go off first. */
#ifndef PLATFORM_DOCK
/* SAVE (in the player and the synth): USB's counters over the scope, to see on the FM-1 itself
 * whether a computer's messages come in and replies go out (an installer that gets no answer) */
static uint8_t diag;
static void diag_draw(void)
{
    char b[40], *p;
    fill(0, 168, 240, 72, C_DARK);
    p = cat(b, usb.up ? (usb.config ? "USB CONFIGURED" : "USB UP") : "USB OFF");
    p = cat(p, " RST ");
    fmt_u(p, usb.resets, 0);
    text(4, 170, b, C_LGREEN);
    p = cat(b, "IN ");
    p = fmt_u(p, usb.rx_pkts, 0);
    p = cat(p, " OUT ");
    p = fmt_u(p, usb.tx_pkts, 0);
    p = cat(p, " SET ");
    fmt_u(p, usb.setups, 0);
    text(4, 186, b, C_WHITE);
    p = cat(b, "SYSEX OUT ");
    p = fmt_u(p, so_w, 0);
    p = cat(p, " SENT ");
    fmt_u(p, so_r, 0);
    text(4, 202, b, C_WHITE);
    p = cat(b, "SOF ");
    p = fmt_u(p, usb.sof_seen, 0);
    p = cat(p, " TO ");
    fmt_u(p, usb.timeouts, 0);
    text(4, 218, b, C_GREY);
}

static void rescue_now(void)
{
    fm1_audio_stop();
    fm1_irq_off();
    usb_detach();
    fm1_delay_ms(30);
    recovery_main();
}
#endif

/* the app: app_init once, then app_step over and over (Dinghy: app_main, between its platform_poll;
 * fm1-dock's module: from the host's draw(), dock/fm1sid_module.c) */
static uint32_t last_draw, last_ms, hold, home_t;

static void app_init(void)
{
    last_ms = 0;
    load_pack();
    st_init((uint32_t)pack_end - 0x02000000u + 0x4000u);   /* (flash offset of the XIP address) */
    list_store();
    engine = pack_flags & 1u;
    start(first_tune, 1);
    if (pack_flags & 8u)
        start_synth();
}

static void app_step(void)
{
    uint32_t e, now;
    int32_t step;
    generate();
    midi_in();
    now = ms;
    if (running)
        played_ms += now - last_ms;
    last_ms = now;
    e = fm1_input_edges(0);
    if (e & (1u << 8)) {                /* HOME: the player <-> the synth; the editor: leave */
        if (trk) {
            if (!tk_dirty || tk_ask(8, "not saved: HOME again leaves"))
                tk_leave();
        } else if (mode) {
            start(play_idx, 0);
        } else {
            start_synth();
        }
    }
    if (e & (1u << 2))                  /* FX: the effects page */
        fx_view = !fx_view;
#ifndef PLATFORM_DOCK
    if ((e & (1u << 9)) && !trk)        /* SAVE (not the editor's): USB's counters */
        diag = !diag;
#endif
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
        } else if ((step = fm1_enc_take(1)) != 0) {   /* ALGORITHM: the tune's subtune */
            uint32_t n = tune_info()->songs;
            if (n > 1u) {
                song_req = (cur_song - 1u + n * 8u + (uint32_t)step) % n + 1u;
                start(play_idx, 0);
            }
        }
        if (e & (1u << 12))
            running = !running;
    }
    if (e & (1u << 6)) {
        engine = !engine;
        if (trk) tk_chip(); else if (mode) start_synth(); else start(play_idx, 0);
    }
#ifndef PLATFORM_DOCK
    /* HOME held 5 s: the USB rescue */
    if (fm1_in.buttons & (1u << 8)) {
        if (!home_t)
            home_t = now ? now : 1u;
        else if (now - home_t > 5000u)
            rescue_now();
    } else {
        home_t = 0;
    }
    /* OCT- and OCT+ held 3 s: the chip's USB update mode (as before; the USB rescue is OCT- as it
     * starts or HOME held 5 s) */
    if ((fm1_in.buttons & 3u) == 3u) {
        if (!hold) hold = now;
        else if (now - hold > 3000u) {
            fm1_audio_stop();
            bootguard_clear(&bootguard);  /* (asked for: not a failed boot) */
            usb_detach();
            fm1_delay_ms(30);
            fm1_enter_uboot();
        }
    } else {
        hold = 0;
    }
#endif
    leds();
    if (now - last_draw >= 40u) {
        last_draw = now;
        draw();
#ifndef PLATFORM_DOCK
        if (diag && !trk)
            diag_draw();
#endif
        lcd_frame();
    }
    /* (no "idle" here: no FM-1 firmware that works on the device sleeps the CPU, and FM-1 SID's
     * builds that did never answered an installer over USB on a real FM-1, 2026-10-24) */
}

#ifndef PLATFORM_DOCK
static void app_main(void)
{
    typedef void (*ctor_t)(void);
    extern ctor_t __init_array_start[], __init_array_end[];
    ctor_t *c;
    uint32_t t0;
    for (c = __init_array_start; c < __init_array_end; c++)
        (*c)();                             /* (reSID's C++ statics: Dinghy's startup has no constructors) */
    for (t0 = ms; ms - t0 < 400u;) {        /* OCT- held as it starts: the USB rescue */
        platform_poll();
        if ((fm1_in.buttons & 3u) == 1u)
            rescue_now();
    }
    app_init();
    for (;;) {
        platform_poll();                    /* the watchdog, USB, an installer, MASTER */
        app_step();
    }
}
#endif
