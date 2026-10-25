/* player/gt2edtest.c - player/gt2edit.c on the host, per song:
 *   - saved as loaded, it is the file again (GoatTracker's own songs: byte for byte);
 *   - moved into the pool, it saves the same and plays the same registers;
 *   - random edits while it plays (rows, lengths, order lists, tables, instruments, new
 *     subtunes): the saved song loads back to the same song (saved again: the same bytes) and
 *     plays the same registers as the edited one.
 *   build/gt2edtest [--seed N] [--edits N] SONG.sng ...   (and once with no song: a new one)
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gt2.h"

#define POOL 61808u                        /* as the firmware's: psid_mem less GT2_SIZE_MAX */
static uint8_t regs[2][25];
static int which;
static void wr(void *user, uint32_t cycle, uint32_t reg, uint32_t val) { (void)user; (void)cycle; regs[which][reg] = (uint8_t)val; }

static uint32_t rnd_s = 1;
static uint32_t rnd(uint32_t n) { rnd_s = rnd_s * 1103515245u + 12345u; return (rnd_s >> 8) % n; }

/* the same registers for frames frames from song (1-based) */
static int same_play(gt2_t *a, gt2_t *b, uint32_t song, uint32_t frames)
{
    uint32_t f;
    gt2_init(a, song);
    gt2_init(b, song);
    for (f = 0; f < frames; f++) {
        which = 0; gt2_play(a);
        which = 1; gt2_play(b);
        if (memcmp(regs[0], regs[1], 25)) {
            printf("    registers differ at frame %u\n", f);
            return 0;
        }
        if (!gt2_playing(a) && !gt2_playing(b))
            break;
    }
    return 1;
}

static void edit_once(gt2_t *g)
{
    uint32_t p = rnd(GT2_MAX_PATT), s = rnd(g->nsongs + 1u), c = rnd(3), op = rnd(16), r;
    uint8_t *b;
    if (rnd(3)) p = rnd(8);                /* (mostly the patterns songs use) */
    if (s >= GT2_MAX_SONGS) s = 0;
    switch (op) {
    case 0: case 1: case 2:                /* a row: note, instrument, command, data */
        if ((b = gt2_patt_w(g, p))) {
            r = rnd(g->pattlen[p]);
            b[r * 4] = (uint8_t)(rnd(4) ? GT2_FIRSTNOTE + rnd(GT2_LASTNOTE - GT2_FIRSTNOTE + 1) :
                                 rnd(2) ? GT2_REST : GT2_KEYOFF);
            b[r * 4 + 1] = (uint8_t)rnd(8);
            b[r * 4 + 2] = (uint8_t)rnd(16);
            b[r * 4 + 3] = (uint8_t)rnd(256);
        }
        break;
    case 3: gt2_patt_len(g, p, 1 + rnd(GT2_MAX_ROWS)); break;
    case 4: gt2_patt_ins(g, p, rnd(g->pattlen[p] + 1u)); break;
    case 5: gt2_patt_del(g, p, rnd(g->pattlen[p] + 1u)); break;
    case 6: gt2_ord_set(g, s, c, rnd(g->songlen[s][c] + 1u), (uint8_t)(rnd(5) ? rnd(12) : 0xd0 + rnd(0x2f))); break;
    case 7: gt2_ord_ins(g, s, c, rnd(g->songlen[s][c] + 1u), (uint8_t)rnd(12)); break;
    case 8: gt2_ord_del(g, s, c, rnd(g->songlen[s][c] + 1u)); break;
    case 9: gt2_ord_restart(g, s, c, (uint8_t)rnd(g->songlen[s][c] + 1u)); break;
    case 10: gt2_tbl_ins(g, (int)rnd(4), (int)rnd(40)); break;
    case 11: gt2_tbl_del(g, (int)rnd(40) ? (int)rnd(4) : 0, (int)rnd(40)); break;
    case 12: {                             /* a table entry */
        int t = (int)rnd(4), i = (int)rnd(40);
        g->ltable[t][i] = (uint8_t)rnd(256);
        g->rtable[t][i] = (uint8_t)rnd(256);
        break;
    }
    case 13: {                             /* an instrument (something set: it is saved) */
        gt2_ins_t *n = &g->instr[1 + rnd(GT2_MAX_INSTR - 1)];
        n->ad = (uint8_t)rnd(256); n->sr = (uint8_t)(rnd(256) | 1);
        n->ptr[rnd(4)] = (uint8_t)rnd(40);
        n->gatetimer = (uint8_t)rnd(256); n->firstwave = (uint8_t)rnd(256); n->vibdelay = (uint8_t)rnd(8);
        n->name[rnd(16)] = (char)('A' + rnd(26));
        break;
    }
    case 14: g->songname[rnd(32)] = (char)('a' + rnd(26)); break;
    case 15: g->chn[c].mute ^= rnd(2); break;
    }
}

static int check(const char *name, const uint8_t *file, uint32_t len, uint32_t edits)
{
    static uint8_t pool[POOL], out1[1 << 17], out2[1 << 17];
    static gt2_t a, b;
    uint32_t n1, n2, i, ok = 1;
    if (file) {
        if (gt2_load(&a, file, len, wr, 0) || gt2_load(&b, file, len, wr, 0)) {
            printf("%s: not loaded\n", name);
            return 0;
        }
        n1 = gt2_save(&a, out1, sizeof out1);
        printf("%s: saved as loaded %s (%u / %u bytes)\n", name,
               n1 == len && !memcmp(out1, file, len) ? "is the file" : "differs from the file", n1, len);
        if (gt2_edit(&b, pool, POOL)) {
            printf("    too big for the pool\n");
            return 0;
        }
        n2 = gt2_save(&b, out2, sizeof out2);
        if (n1 != n2 || memcmp(out1, out2, n1)) { printf("    FAIL: in the pool it saves differently\n"); ok = 0; }
        for (i = 1; i <= a.nsongs; i++)
            if (!same_play(&a, &b, i, 3000)) { printf("    FAIL: in the pool subtune %u plays differently\n", i); ok = 0; }
    } else {
        if (gt2_new(&b, pool, POOL, wr, 0)) return 0;
        printf("%s:\n", name);
    }
    /* edits while it plays */
    gt2_init(&b, 1);
    for (i = 0; i < edits; i++) {
        edit_once(&b);
        which = 0;
        gt2_play(&b);
        if (!gt2_playing(&b) && !rnd(8))
            gt2_init(&b, 1 + rnd(b.nsongs));
    }
    for (i = 0; i < 3; i++)
        b.chn[i].mute = 0;
    n1 = gt2_save(&b, out1, sizeof out1);
    if (!n1 || gt2_load(&a, out1, n1, wr, 0)) {
        printf("    FAIL: the edited song (%u bytes, pool %u used) does not load back\n", n1, b.pool_used);
        return 0;
    }
    n2 = gt2_save(&a, out2, sizeof out2);
    if (n1 != n2 || memcmp(out1, out2, n1)) { printf("    FAIL: loaded back it saves differently\n"); ok = 0; }
    for (i = 1; i <= a.nsongs; i++)
        if (b.songlen[i - 1][0] && b.songlen[i - 1][1] && b.songlen[i - 1][2] && !same_play(&b, &a, i, 3000)) {
            printf("    FAIL: loaded back subtune %u plays differently\n", i);
            ok = 0;
        }
    printf("    %u edits: saved %u bytes, %u subtune(s), pool %u of %u used: %s\n", edits, n1, a.nsongs,
           b.pool_used, POOL, ok ? "ok" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 17];
    uint32_t edits = 2000, fails = 0, songs = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            rnd_s = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--edits") && i + 1 < argc)
            edits = (uint32_t)atoi(argv[++i]);
        else {
            FILE *f = fopen(argv[i], "rb");
            size_t n;
            if (!f) { perror(argv[i]); return 1; }
            n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            fails += !check(argv[i], buf, (uint32_t)n, edits);
            songs++;
        }
    }
    fails += !check("a new song", 0, 0, edits);
    printf("%u of %u ok\n", songs + 1 - fails, songs + 1);
    return fails != 0;
}
