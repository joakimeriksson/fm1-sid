/* player/gt2log.c - player/gt2.c on the host: a GoatTracker song's registers a frame at a time,
 * as sid-tracker's gt2dump prints GoatTracker's own (tools/gt2check.sh compares the two), or
 * its register stream as sidlog v2 (build/replay renders it, as for build/psidlog).
 *   build/gt2log SONG.sng [--frames N] [--subtune N] [--sidlog OUT.sidlog]
 *                [--pos A,B,C | --pattern P,Q,R] [--row N]
 * (--subtune counts from 0, as gt2dump's does; --pos / --pattern: the editor's ways to play,
 * from those order list positions or those patterns over and over, from row N)
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gt2.h"

typedef struct { uint32_t cyc; uint8_t chip, reg, val, pad; } ev_t;
static ev_t *ev;
static uint32_t nev, cap, base;

static void wr(void *user, uint32_t cycle, uint32_t reg, uint32_t val)
{
    (void)user;
    if (nev == cap)
        ev = realloc(ev, (cap = cap ? 2 * cap : 65536) * sizeof *ev);
    ev[nev].cyc = base + cycle;
    ev[nev].chip = 0;
    ev[nev].reg = (uint8_t)reg;
    ev[nev].val = (uint8_t)val;
    ev[nev++].pad = 0;
}

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 20];
    gt2_t *g = malloc(gt2_size());
    const char *path = 0, *sidlog = 0, *name;
    uint32_t frames = 1500, subtune = 0, f, i, regs[32] = {0}, row = 0;
    int mode = 0;
    uint8_t at[3] = {0, 0, 0};
    size_t n;
    FILE *fp;
    for (i = 1; i < (uint32_t)argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < (uint32_t)argc)
            frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--subtune") && i + 1 < (uint32_t)argc)
            subtune = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sidlog") && i + 1 < (uint32_t)argc)
            sidlog = argv[++i];
        else if ((!strcmp(argv[i], "--pos") || !strcmp(argv[i], "--pattern")) && i + 1 < (uint32_t)argc) {
            unsigned a = 0, b = 0, c = 0;
            mode = argv[i][2] == 'p' && argv[i][3] == 'o' ? GT2_PLAY_POS : GT2_PLAY_PATTERN;
            sscanf(argv[++i], "%u,%u,%u", &a, &b, &c);
            at[0] = (uint8_t)a; at[1] = (uint8_t)b; at[2] = (uint8_t)c;
        } else if (!strcmp(argv[i], "--row") && i + 1 < (uint32_t)argc)
            row = (uint32_t)atoi(argv[++i]);
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s SONG.sng [--frames N] [--subtune N] [--sidlog OUT.sidlog]\n", argv[0]);
        return 2;
    }
    fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    if (gt2_load(g, buf, (uint32_t)n, wr, 0)) {
        fprintf(stderr, "%s: not a GoatTracker 2 song this plays\n", path);
        return 1;
    }
    fprintf(stderr, "\"%s\" by %s (%s), %u subtune(s)\n", gt2_info(g)->name, gt2_info(g)->author,
            gt2_info(g)->released, gt2_info(g)->songs);
    if (mode) {                            /* (its first frame sets the channels up, as gt2_init's) */
        gt2_reset(g);
        gt2_start(g, mode, subtune, at, at, row);
        gt2_play(g);
        nev = 0;
    } else
        gt2_init(g, subtune + 1);
    name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (!sidlog)
        printf("{\"source\":\"ref\",\"song\":\"%s\",\"subtune\":%u}\n", name, subtune);
    for (f = 0; f < frames; f++) {
        base = (f + 1) * GT2_FRAME;
        i = nev;
        gt2_play(g);
        if (!gt2_playing(g)) {
            fprintf(stderr, "song stopped at frame %u\n", f);
            break;
        }
        if (sidlog)
            continue;
        for (; i < nev; i++)
            regs[ev[i].reg] = ev[i].val;
        printf("{\"f\":%u,\"regs\":[", f);
        for (i = 0; i < 25; i++)
            printf(i ? ",%u" : "%u", regs[i]);
        printf("]}\n");
    }
    if (sidlog) {
        uint32_t secs = (f + 1) * GT2_FRAME / PSID_PAL_CLOCK;
        uint32_t h[7] = {gt2_info(g)->model, 44100, PSID_PAL_CLOCK, 1, secs * 44100u, nev, 256};
        fp = fopen(sidlog, "wb");
        if (!fp) { perror(sidlog); return 1; }
        fwrite("SIDLOG2", 1, 8, fp);
        fwrite(h, 4, 7, fp);
        fwrite(ev, 8, nev, fp);
        fclose(fp);
    }
    fprintf(stderr, "%u frames\n", f);
    return 0;
}
