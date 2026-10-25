/* SPDX-License-Identifier: GPL-3.0-only */
/* dock/fm1sid_module.c - FM-1 SID (firmware/fm1sid.c, all of it: the C64 tune player, the
 * GoatTracker player and editor, the synth and its sequencer, the effects; reSID and the own model)
 * as an fm1-dock module: fm1-dock keeps USB, the installer and its rescue, and the module is
 * deployed over USB-MIDI (fm1-dock's tools/dock.py deploy). The app's main loop body runs from
 * draw(), its audio interrupt from render(); its platform is dock_platform.h. */
#define PLATFORM_DOCK 1
#include "../firmware/fm1sid.c"

static void render(int32_t *out, uint32_t n) { app_audio(out, n); }

static void draw_step(uint32_t full)        /* the host's main loop, often: the app's loop body once */
{
    if (full) {                             /* (the host drew over the screen: the picker) */
        shown_ok = 0;
        hbp_leds_reset();
    }
    app_step();
    hbp_leds();
}

static void ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static int init(const hb_host_t *h)
{
    H = h;
    hbp_leds_reset();
    app_init();
    H->log("fm1sid: FM-1 SID, reSID 0.16 and the own model");
    return 0;
}

static void release(void)
{
    running = 0;                            /* (the host stops calling render: nothing more to do) */
}

HB_DESC = {HB_ABI, "FM-1 SID", init, release, note, cc, render, ui, draw_step, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
