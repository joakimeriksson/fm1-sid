/* firmware/fx.c - the effects after the SID (included by main.c): a stereo chorus, a tempo delay
 * whose right side answers half a time later, and a room reverb. The chip is mono, the FM-1's
 * output stereo: the chorus's two taps swing in opposite directions and the delay bounces, so
 * the sound gets a width the SID has not.
 *
 * After Felucca's src/fx.c (Leo Kuroshita, Hügelton Instruments, GPL-3.0-only): its chorus line,
 * its delay's low-passed feedback and its ROOM reverb (4 damped combs + 2 allpasses, Freeverb-
 * like), here in one block function for the audio ISR. All three always run (a few percent of
 * the core), so turning one up sounds at once, like a send; their levels at 0 leave the chip's
 * own samples untouched.
 * SPDX-License-Identifier: GPL-3.0-only */

#define DLY_LEN 32768u                     /* 0.74 s of line: the right tap at 1.5x the time fits */
#define DLY_MAX ((DLY_LEN - 1u) * 2u / 3u)  /* the longest time: 0.49 s */
#define CHO_LEN 1024u
static int16_t dly_buf[DLY_LEN];
static int16_t cho_buf[CHO_LEN];
static const uint16_t REV_COMB[4] = {1116, 1188, 1277, 1356};
static const uint16_t REV_AP[2] = {556, 441};
static int16_t rev_comb[1116 + 1188 + 1277 + 1356];
static int16_t rev_ap[556 + 441];

/* the settings, 0..15 each, one set per mode (the player starts dry: tunes were mixed for the
 * bare chip; the synth with a little of each); FX_TIME 0..3 = 1/16, 1/8, dotted 1/8, 1/4 */
enum { FX_CHO, FX_DLY, FX_TIME, FX_FB, FX_REV, FX_N };
#define FX_MAX 15
#define FX_TIMES 4u
static volatile uint8_t fxs[2][FX_N] = {{0, 0, 2, 6, 0}, {5, 4, 2, 6, 5}};
static uint8_t fx_view;                    /* main loop: the FX page is up (FX switches) */
static struct {
    uint32_t dly_w, cho_w, cho_ph;
    int32_t dly_lp, comb_lp[4];
    uint16_t comb_i[4], ap_i[2];
} fx;

static inline int32_t clamp16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
static inline int32_t mulq15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }

/* the delay's time in samples at the tempo (the sequencer's, 120 BPM in the player) */
static uint32_t fx_delay_samples(uint32_t t)
{
    uint32_t q = FS_HZ * 60u / bpm, s = t == 0 ? q / 4u : t == 1 ? q / 2u : t == 2 ? q * 3u / 4u : q;
    return s > DLY_MAX ? DLY_MAX : s;
}

/* the chorus line read q8 samples back, between samples */
static inline int32_t cho_tap(uint32_t q8)
{
    uint32_t i = q8 >> 8;
    int32_t f = (int32_t)(q8 & 255u), c0 = cho_buf[(fx.cho_w - i) & (CHO_LEN - 1u)];
    int32_t c1 = cho_buf[(fx.cho_w - i - 1u) & (CHO_LEN - 1u)];
    return c0 + (((c1 - c0) * f) >> 8);
}

/* one half buffer: the chip's samples in, the stereo output words (24-bit) out */
static void fx_block(const short *in, int32_t *o, uint32_t n)
{
    const volatile uint8_t *q = fxs[mode];
    int32_t mc = q[FX_CHO] * 2, dm = q[FX_DLY] * 2184, fb = q[FX_FB] * 1950, rl = q[FX_REV];
    uint32_t dl = fx_delay_samples(q[FX_TIME]), dr = dl + dl / 2u, i, k;
    for (i = 0; i < n; i++) {
        int32_t d = in[i], l = d, r = d, m, x, xr, a = 0, t;
        int16_t *c;
        /* chorus: 8 ms +-3 ms, a 0.7 Hz triangle, the right tap against the left */
        cho_buf[fx.cho_w & (CHO_LEN - 1u)] = (int16_t)d;
        fx.cho_ph += 68146u;
        t = (int32_t)(fx.cho_ph >> 15);
        t = t < 65536 ? t - 32768 : 98303 - t;
        if (mc) {
            int32_t dd = d - ((d * mc) >> 7);   /* the dry part gives way a little: the level holds */
            l = dd + ((cho_tap((353u << 8) + (uint32_t)((t * 132) >> 7)) * mc) >> 6);
            r = dd + ((cho_tap((353u << 8) - (uint32_t)((t * 132) >> 7)) * mc) >> 6);
        }
        fx.cho_w++;
        m = (l + r) >> 1;
        /* delay: the repeats low-passed (they darken), the right side 1.5 times later */
        x = dly_buf[(fx.dly_w - dl) & (DLY_LEN - 1u)];
        xr = dly_buf[(fx.dly_w - dr) & (DLY_LEN - 1u)];
        fx.dly_lp += mulq15(x - fx.dly_lp, 20000);
        dly_buf[fx.dly_w & (DLY_LEN - 1u)] = (int16_t)clamp16(m + mulq15(fx.dly_lp, fb));
        fx.dly_w++;
        if (dm) {
            x = mulq15(x, dm);
            l += x;
            r += mulq15(xr, dm);
            m += x;                        /* the echoes into the room too */
        }
        /* reverb: Felucca's ROOM at a fixed size and damping */
        m = mulq15(m, 2580);
        c = rev_comb;
        for (k = 0; k < 4u; k++) {
            int32_t v = c[fx.comb_i[k]];
            fx.comb_lp[k] = v + mulq15(fx.comb_lp[k] - v, 12800);
            c[fx.comb_i[k]] = (int16_t)clamp16(m + mulq15(fx.comb_lp[k], 28000));
            if (++fx.comb_i[k] >= REV_COMB[k])
                fx.comb_i[k] = 0;
            a += v;
            c += REV_COMB[k];
        }
        c = rev_ap;
        for (k = 0; k < 2u; k++) {
            int32_t v = c[fx.ap_i[k]];
            c[fx.ap_i[k]] = (int16_t)clamp16(a + (v >> 1));
            a = v - a;
            if (++fx.ap_i[k] >= REV_AP[k])
                fx.ap_i[k] = 0;
            c += REV_AP[k];
        }
        if (rl) {
            a = (a * rl) >> 4;
            l += a;
            r += a;
        }
        o[2u * i] = clamp16(l) * (1 << OUT_SHIFT);
        o[2u * i + 1u] = clamp16(r) * (1 << OUT_SHIFT);
    }
}

/* the FX page's controls (main loop): KNOB1 chorus, KNOB2 delay, KNOB3 its time, KNOB4 reverb,
 * SELECT the delay's feedback, PRESETS the chip (6581 / 8580); returns 1 when the chip changed */
static int fx_keys(void)
{
    static const uint8_t ENC[5] = {2, 3, 4, 0, 5};   /* FX_CHO, FX_DLY, FX_TIME, FX_FB, FX_REV */
    volatile uint8_t *q = fxs[mode];
    uint32_t i;
    for (i = 0; i < FX_N; i++) {
        int32_t d = fm1_enc_take(ENC[i]), v, lim = i == FX_TIME ? (int32_t)FX_TIMES - 1 : FX_MAX;
        if (!d)
            continue;
        v = q[i] + d;
        q[i] = (uint8_t)(v < 0 ? 0 : v > lim ? lim : v);
    }
    return fm1_enc_take(6) != 0;
}

/* the FX page, in place of the voices and the filter */
static void draw_fx(void)
{
    static const char *const NAME[FX_N] = {"CHORUS", "DELAY", "TIME", "FEEDBK", "REVERB"};
    static const char *const CTRL[FX_N] = {"KNOB1", "KNOB2", "KNOB3", "SELECT", "KNOB4"};
    static const char *const TIME[FX_TIMES] = {"1/16", "1/8", "1/8.", "1/4"};
    static const uint8_t ORDER[FX_N] = {FX_CHO, FX_DLY, FX_TIME, FX_FB, FX_REV};
    const volatile uint8_t *q = fxs[mode];
    char line[32], *p;
    uint32_t i;
    text(2, 58, "EFFECTS", C_WHITE);
    text(186, 58, "FX:", C_GREY);
    text(214, 58, "<-", C_GREY);
    for (i = 0; i < FX_N; i++) {
        uint32_t f = ORDER[i], v = q[f];
        int y = 76 + (int)i * 20;
        text(2, y, NAME[f], C_CYAN);
        if (f == FX_TIME) {
            p = cat(line, TIME[v]);
            p = cat(p, " ");
            p = fmt_u(p, fx_delay_samples(v) * 1000u / FS_HZ, 1);
            cat(p, " ms");
            text(58, y, line, C_WHITE);
        } else {
            bar(58, y + 2, 92, 8, v, FX_MAX, f == FX_REV ? C_LGREEN : f == FX_CHO ? C_YELLOW : C_ORANGE);
            fmt_u(line, v, 2);
            text(156, y, line, C_WHITE);
        }
        text(186, y, CTRL[f], C_GREY);
    }
    p = cat(line, "CHIP ");
    p = cat(p, model ? "8580" : "6581");
    text(2, 178, line, C_CYAN);
    text(186, 178, "PRESET", C_GREY);
}
