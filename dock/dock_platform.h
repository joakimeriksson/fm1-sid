/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-dock/dock_platform.h - Dinghy's platform calls, as FM-1 SID's app (firmware/fm1sid.c) makes
 * them, on fm1-dock's host table (its firmware/dock/hb_module.h): included by fm1sid.c in
 * place of Dinghy's platform.c when built as a fm1-dock module (PLATFORM_DOCK, dock/README.md).
 * The panel, the time and MIDI come from fm1-dock's modules/lib/hb_fm1.h; here the rest: the LEDs
 * (the app's matrix words, sent on as fm1-dock's LED ids: lit 1, the glow 2), the screen strips
 * (fm1-dock's blit, done when it returns: one strip), the waits and what the app shows of USB
 * (fm1-dock's own: nothing here). The song store is store.c's, in the host's banks. */
#ifndef DOCK_PLATFORM_H
#define DOCK_PLATFORM_H
#include "hb_fm1.h"

#define FS 44100u                           /* the host's sample rate (Dinghy's) */
#define LCD_STRIPS 1u

void *memcpy(void *d, const void *s, unsigned n);   /* (fm1-dock's modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
static int memcmp(const void *a, const void *b, unsigned n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p < *q ? -1 : 1;
    return 0;
}

/* the panel's LEDs as Dinghy keeps them (hal/fm1_input.h): a word per column, a bit per row */
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL];
static uint8_t hbp_led_sent[41];            /* what the host was told: 0 off, 1 lit, 2 the glow; 3 nothing yet */

static void hbp_leds_reset(void)
{
    uint32_t i;
    for (i = 0; i < 41u; i++)
        hbp_led_sent[i] = 3;
}

static void hbp_leds(void)                  /* the changes on to the host (after the app's leds()) */
{
    uint32_t c, r;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 1; r < 5u; r++) {
            int id = FM1_KEYMAP[r][c];
            uint8_t v;
            if (id < 0)
                continue;
            v = (uint8_t)(fm1_led[c] >> r & 1u ? 1u : fm1_led_dim[c] >> r & 1u ? 2u : 0u);
            if (v != hbp_led_sent[id]) {
                hbp_led_sent[id] = v;
                H->led((uint32_t)id, v);
            }
        }
}

static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}

/* the app waits a few ms for the audio interrupt to finish its block (start, start_synth) */
static void fm1_delay_ms(uint32_t n)
{
    uint32_t t0 = H->ms();
    while (H->ms() - t0 < n + 1u)
        ;
}
static void fm1_wdt_feed(void) {}           /* (the host's) */

static struct { uint32_t up, config, suspended; } usb;   /* (USB is the host's: the header shows none) */

/* the song store: firmware/store.c's own, in the host's banks (hb_host_t.bank_load_n) */

#endif
