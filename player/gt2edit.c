/* player/gt2edit.c - a GoatTracker 2 song to edit (player/gt2.h): the order lists and patterns
 * in a pool of RAM, each as long as it is, one after the other; a change of length moves the
 * ones after it and the index follows. A pattern the song has not got stays GoatTracker's
 * cleared one (64 rests) and takes no room until it is written. The changes follow GoatTracker
 * v2.76's editor (gpattern.c, gorder.c, gtable.c, gsong.c: Lasse Oorni and contributors,
 * GPL-2.0-or-later), and so does the .sng it writes (gsong.c savesong).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "gt2.h"

#define SENT 0x7fffffffu                   /* gt2_chn_t.pattptr at a pattern's end */

static void mmove(uint8_t *d, const uint8_t *s, uint32_t n)
{
    if (d < s)
        while (n--) *d++ = *s++;
    else
        while (n--) d[n] = s[n];
}

static int in_pool(const gt2_t *g, const uint8_t *p)
{
    return g->pool && p >= g->pool && p < g->pool + g->pool_size;
}

uint32_t gt2_free(const gt2_t *g) { return g->pool ? g->pool_size - g->pool_used : 0; }

/* the block at *ptr (n bytes; not in the pool yet: none) made newn bytes long: the blocks after
 * it move and their pointers with them; the first min(n, newn) bytes stay */
static int resize(gt2_t *g, const uint8_t **ptr, uint16_t *n, uint32_t newn)
{
    uint32_t off, old, i, c, tail;
    int32_t delta;
    uint8_t *at;
    if (!g->pool || !newn)
        return -1;
    if (!in_pool(g, *ptr)) {
        if (newn > gt2_free(g))
            return -1;
        *ptr = g->pool + g->pool_used;
        *n = (uint16_t)newn;
        g->pool_used += newn;
        return 0;
    }
    at = (uint8_t *)*ptr;
    off = (uint32_t)(at - g->pool);
    old = *n;
    delta = (int32_t)newn - (int32_t)old;
    if (delta > 0 && (uint32_t)delta > gt2_free(g))
        return -1;
    tail = g->pool_used - off - old;
    mmove(at + newn, at + old, tail);
    for (i = 0; i < GT2_MAX_PATT; i++)
        if (in_pool(g, g->patt[i]) && g->patt[i] > at)
            g->patt[i] += delta;
    for (i = 0; i < GT2_MAX_SONGS; i++)
        for (c = 0; c < 3; c++)
            if (in_pool(g, g->order[i][c]) && g->order[i][c] > at)
                g->order[i][c] += delta;
    g->pool_used = (uint32_t)((int32_t)g->pool_used + delta);
    *n = (uint16_t)newn;
    return 0;
}

static void set_row(uint8_t *r, uint8_t note) { r[0] = note; r[1] = r[2] = r[3] = 0; }

/* the bytes of a pattern / an order list as the pool keeps them: the rows and the end row; the
 * entries, LOOPSONG and the restart */
static uint32_t patt_bytes(const gt2_t *g, uint32_t p) { return (g->pattlen[p] + 1u) * 4u; }
static uint32_t ord_bytes(const gt2_t *g, uint32_t s, uint32_t c) { return g->songlen[s][c] + 2u; }

/* a pattern that reads as a cleared one (64 rests): it needs no room */
static int cleared(const gt2_t *g, uint32_t p)
{
    uint32_t i;
    if (g->pattlen[p] != GT2_DEFROWS)
        return 0;
    for (i = 0; i < GT2_DEFROWS * 4u; i++)
        if (gt2_pat(g, p, i) != (i % 4u ? 0 : GT2_REST))
            return 0;
    return 1;
}

int gt2_edit(gt2_t *g, uint8_t *pool, uint32_t size)
{
    static uint8_t tmp[(GT2_MAX_ROWS + 1) * 4 + 2];
    uint32_t need = 0, p, s, c, i, n;
    if (g->pool)
        return 0;
    for (p = 0; p < GT2_MAX_PATT; p++) {
        if (g->patt[p] && cleared(g, p)) {
            g->patt[p] = 0;
            g->pattn[p] = 0;
        }
        if (g->patt[p])
            need += patt_bytes(g, p);
    }
    for (s = 0; s < GT2_MAX_SONGS; s++)
        for (c = 0; c < 3; c++)
            if (g->order[s][c])
                need += ord_bytes(g, s, c);
    if (need > size)
        return -1;
    g->pool_size = size;
    g->pool_used = 0;
    /* each block read from the file into tmp (with no pool set: gt2_pat reads it as the player
     * does), then into the pool */
    for (p = 0; p < GT2_MAX_PATT; p++)
        if (g->patt[p]) {
            n = patt_bytes(g, p);
            g->pool = 0;
            for (i = 0; i < n; i++)
                tmp[i] = i + 4u >= n ? (i % 4u ? 0 : GT2_ENDPATT) : gt2_pat(g, p, i);
            g->pool = pool;
            g->patt[p] = 0;
            resize(g, &g->patt[p], &g->pattn[p], n);
            mmove((uint8_t *)g->patt[p], tmp, n);
        }
    for (s = 0; s < GT2_MAX_SONGS; s++)
        for (c = 0; c < 3; c++)
            if (g->order[s][c]) {
                n = ord_bytes(g, s, c);
                g->pool = 0;
                for (i = 0; i + 2u < n; i++)
                    tmp[i] = gt2_ord(g, s, c, i);
                tmp[n - 2] = GT2_LOOPSONG;
                tmp[n - 1] = gt2_ord(g, s, c, n - 1);
                g->pool = pool;
                g->order[s][c] = 0;
                resize(g, &g->order[s][c], &g->ordn[s][c], n);
                mmove((uint8_t *)g->order[s][c], tmp, n);
            }
    return 0;
}

int gt2_new(gt2_t *g, uint8_t *pool, uint32_t size, psid_write_fn wr, void *user)
{
    uint32_t i, c;
    uint8_t *z = (uint8_t *)g;
    for (i = 0; i < sizeof *g; i++)
        z[i] = 0;
    g->wr = wr;
    g->user = user;
    g->pool = pool;
    g->pool_size = size;
    for (i = 1; i < GT2_MAX_INSTR; i++) {   /* ginstr.c clearinstr() at 1x */
        g->instr[i].gatetimer = 2;
        g->instr[i].firstwave = 0x9;
    }
    for (i = 0; i < GT2_MAX_PATT; i++)
        g->pattlen[i] = GT2_DEFROWS;
    for (c = 0; c < 3; c++) {              /* subtune 0 plays patterns 0, 1, 2 */
        uint8_t *o;
        if (resize(g, &g->order[0][c], &g->ordn[0][c], 3))
            return -1;
        o = (uint8_t *)g->order[0][c];
        o[0] = (uint8_t)c;
        o[1] = GT2_LOOPSONG;
        o[2] = 0;
        g->songlen[0][c] = 1;
        g->chn[c].instr = 1;
        g->chn[c].tempo = 6 - 1;
    }
    g->nsongs = 1;
    g->funktable[0] = 9 - 1;
    g->funktable[1] = 6 - 1;
    g->masterfader = 0x0f;
    g->songinit = 0x80;                    /* stopped */
    return 0;
}

/* -------------------------------------------------------------------------- patterns --- */
uint8_t *gt2_patt_w(gt2_t *g, uint32_t p)
{
    uint32_t r;
    uint8_t *b;
    if (p >= GT2_MAX_PATT || !g->pool)
        return 0;
    if (!in_pool(g, g->patt[p])) {         /* a cleared pattern: its rows and its end row */
        uint32_t n = patt_bytes(g, p);
        g->patt[p] = 0;
        if (resize(g, &g->patt[p], &g->pattn[p], n))
            return 0;
        b = (uint8_t *)g->patt[p];
        for (r = 0; r + 1u < n / 4u; r++)
            set_row(b + 4u * r, GT2_REST);
        set_row(b + n - 4u, GT2_ENDPATT);
    }
    return (uint8_t *)g->patt[p];
}

/* the channels playing pattern p past its new end go on at the end row */
static void clip_play(gt2_t *g, uint32_t p)
{
    uint32_t c;
    for (c = 0; c < 3; c++)
        if (g->chn[c].pattnum == p && g->chn[c].pattptr != SENT && g->chn[c].pattptr > g->pattlen[p] * 4u)
            g->chn[c].pattptr = g->pattlen[p] * 4u;
}

int gt2_patt_len(gt2_t *g, uint32_t p, uint32_t rows)
{
    uint32_t old, r;
    uint8_t *b;
    if (rows < 1 || rows > GT2_MAX_ROWS || !gt2_patt_w(g, p))
        return -1;
    old = g->pattlen[p];
    if (resize(g, &g->patt[p], &g->pattn[p], (rows + 1u) * 4u))
        return -1;
    b = (uint8_t *)g->patt[p];
    for (r = old; r < rows; r++)
        set_row(b + 4u * r, GT2_REST);
    set_row(b + 4u * rows, GT2_ENDPATT);
    g->pattlen[p] = (uint8_t)rows;
    clip_play(g, p);
    return 0;
}

/* gpattern.c KEY_INS / KEY_DEL */
int gt2_patt_ins(gt2_t *g, uint32_t p, uint32_t row)
{
    uint8_t *b = gt2_patt_w(g, p);
    uint32_t len;
    if (!b)
        return -1;
    len = g->pattlen[p];
    if (row < len) {
        mmove(b + row * 4u + 4u, b + row * 4u, (len - row) * 4u - 4u);
        set_row(b + row * 4u, GT2_REST);
        return 0;
    }
    return row == len && len < GT2_MAX_ROWS ? gt2_patt_len(g, p, len + 1u) : -1;
}

int gt2_patt_del(gt2_t *g, uint32_t p, uint32_t row)
{
    uint8_t *b = gt2_patt_w(g, p);
    uint32_t len;
    if (!b)
        return -1;
    len = g->pattlen[p];
    if (row < len) {
        mmove(b + row * 4u, b + row * 4u + 4u, (len - row) * 4u - 4u);
        set_row(b + len * 4u - 4u, GT2_REST);
        return 0;
    }
    return row == len && len > 1 ? gt2_patt_len(g, p, len - 1u) : -1;
}

/* ------------------------------------------------------------------------ order lists --- */
/* song s's three lists in the pool (a subtune the song has not got: each just its end) */
static int ord_w(gt2_t *g, uint32_t s)
{
    uint32_t c;
    if (s >= GT2_MAX_SONGS || !g->pool)
        return -1;
    for (c = 0; c < 3; c++)
        if (!in_pool(g, g->order[s][c])) {
            uint8_t *o;
            g->order[s][c] = 0;
            if (resize(g, &g->order[s][c], &g->ordn[s][c], 2))
                return -1;
            o = (uint8_t *)g->order[s][c];
            o[0] = GT2_LOOPSONG;
            o[1] = 0;
            g->songlen[s][c] = 0;
        }
    if (g->nsongs < s + 1u)
        g->nsongs = s + 1u;
    return 0;
}

int gt2_ord_set(gt2_t *g, uint32_t s, uint32_t c, uint32_t pos, uint8_t v)
{
    if (c > 2 || v == GT2_LOOPSONG || ord_w(g, s) || pos >= g->songlen[s][c])
        return -1;
    ((uint8_t *)g->order[s][c])[pos] = v;
    return 0;
}

int gt2_ord_ins(gt2_t *g, uint32_t s, uint32_t c, uint32_t pos, uint8_t v)
{
    uint32_t len;
    uint8_t *o, r;
    if (c > 2 || v == GT2_LOOPSONG || ord_w(g, s))
        return -1;
    len = g->songlen[s][c];
    if (pos > len || len >= GT2_MAX_SONGLEN)
        return -1;
    r = g->order[s][c][len + 1];
    if (resize(g, &g->order[s][c], &g->ordn[s][c], len + 3u))
        return -1;
    o = (uint8_t *)g->order[s][c];
    mmove(o + pos + 1u, o + pos, len - pos);
    o[pos] = v;
    o[len + 1] = GT2_LOOPSONG;
    if (pos < len && r > pos && r < len)  /* (gorder.c insertorder) */
        r++;
    o[len + 2] = r;
    g->songlen[s][c] = (uint8_t)(len + 1u);
    return 0;
}

int gt2_ord_del(gt2_t *g, uint32_t s, uint32_t c, uint32_t pos)
{
    uint32_t len;
    uint8_t *o, r;
    if (c > 2 || ord_w(g, s))
        return -1;
    len = g->songlen[s][c];
    if (pos >= len || len < 2)              /* (a list keeps one entry) */
        return -1;
    o = (uint8_t *)g->order[s][c];
    r = o[len + 1];
    mmove(o + pos, o + pos + 1u, len - pos - 1u);
    if (r > pos && r > 0)                  /* (gorder.c deleteorder) */
        r--;
    if (r >= len - 1u)
        r = 0;
    o[len - 1] = GT2_LOOPSONG;
    o[len] = r;
    resize(g, &g->order[s][c], &g->ordn[s][c], len + 1u);
    g->songlen[s][c] = (uint8_t)(len - 1u);
    for (pos = 0; pos < 3; pos++)          /* (a channel playing past the end: to the start) */
        if (g->psnum == s && g->chn[pos].songptr > g->songlen[s][pos])
            g->chn[pos].songptr = 0;
    return 0;
}

int gt2_ord_restart(gt2_t *g, uint32_t s, uint32_t c, uint8_t pos)
{
    if (c > 2 || ord_w(g, s) || pos >= g->songlen[s][c])
        return -1;
    ((uint8_t *)g->order[s][c])[g->songlen[s][c] + 1u] = pos;
    return 0;
}

/* ----------------------------------------------------------------------------- tables --- */
/* the pointers into table t from instruments, wave table commands, patterns and the table's own
 * jumps: those past pos (ins: from pos on) one along (gtable.c inserttable mode 0 / deletetable) */
static int ptr_cmd(int t, uint32_t cmd)
{
    if (t < GT2_STBL)
        return cmd == 8u + (uint32_t)t;    /* CMD_SETWAVEPTR + t */
    return cmd == 14u || (cmd >= 1u && cmd <= 4u);   /* FUNKTEMPO, PORTAUP..VIBRATO */
}

static void shift_ptrs(gt2_t *g, int t, int pos, int ins)
{
    uint32_t c, d;
#define MOVE(v) do { int q_ = (int)(v) - 1; if (ins ? q_ >= pos : q_ > pos) (v) = (uint8_t)((v) + (ins ? 1 : -1)); } while (0)
    for (c = 1; c < GT2_MAX_INSTR; c++)
        MOVE(g->instr[c].ptr[t]);
    for (c = 0; c < GT2_TABLELEN; c++)
        if (g->ltable[GT2_WTBL][c] >= 0xf0 && g->ltable[GT2_WTBL][c] <= 0xfe &&
            ptr_cmd(t, g->ltable[GT2_WTBL][c] & 0xfu))
            MOVE(g->rtable[GT2_WTBL][c]);
    for (c = 0; c < GT2_MAX_PATT; c++)
        if (in_pool(g, g->patt[c])) {
            uint8_t *b = (uint8_t *)g->patt[c];
            for (d = 0; d < g->pattlen[c]; d++)
                if (ptr_cmd(t, b[d * 4 + 2]))
                    MOVE(b[d * 4 + 3]);
        }
    if (t != GT2_STBL)
        for (c = 0; c < GT2_TABLELEN; c++)
            if (g->ltable[t][c] == 0xff)
                MOVE(g->rtable[t][c]);
#undef MOVE
}

void gt2_tbl_ins(gt2_t *g, int t, int pos)
{
    int c;
    if (t < 0 || t > 3 || pos < 0 || pos >= GT2_TABLELEN)
        return;
    shift_ptrs(g, t, pos, 1);
    for (c = GT2_TABLELEN - 1; c > pos; c--) {
        g->ltable[t][c] = g->ltable[t][c - 1];
        g->rtable[t][c] = g->rtable[t][c - 1];
    }
    g->ltable[t][pos] = g->rtable[t][pos] = 0;
}

void gt2_tbl_del(gt2_t *g, int t, int pos)
{
    int c;
    if (t < 0 || t > 3 || pos < 0 || pos >= GT2_TABLELEN)
        return;
    shift_ptrs(g, t, pos, 0);
    for (c = pos; c < GT2_TABLELEN - 1; c++) {
        g->ltable[t][c] = g->ltable[t][c + 1];
        g->rtable[t][c] = g->rtable[t][c + 1];
    }
    g->ltable[t][GT2_TABLELEN - 1] = g->rtable[t][GT2_TABLELEN - 1] = 0;
}

/* ------------------------------------------------------------------------------- .sng --- */
uint32_t gt2_save_to(const gt2_t *g, void (*put)(void *user, uint8_t b), void *user)
{
    uint32_t pos = 0, hp = 0, hi = 0, amount, c, d, i, n;
#define PUT(v) do { if (put) put(user, (uint8_t)(v)); pos++; } while (0)
    /* gsong.c countpatternlengths(): the highest pattern with a note or a command in it or in
     * an order list, the highest instrument a pattern names or with something set */
    for (c = 0; c < GT2_MAX_PATT; c++)
        for (d = 0; d < g->pattlen[c]; d++) {
            uint8_t a = gt2_pat(g, c, d * 4), b = gt2_pat(g, c, d * 4 + 1);
            if (a != GT2_REST || b || gt2_pat(g, c, d * 4 + 2) || gt2_pat(g, c, d * 4 + 3))
                hp = c;
            if (b > hi)
                hi = b;
        }
    for (i = 0; i < GT2_MAX_SONGS; i++)
        for (c = 0; c < 3; c++)
            for (d = 0; d < g->songlen[i][c]; d++) {
                uint8_t e = gt2_ord(g, i, c, d);
                if (e < GT2_REPEAT && e > hp)
                    hp = e;
            }
    for (c = 1; c < GT2_MAX_INSTR; c++) {
        const gt2_ins_t *s = &g->instr[c];
        if ((s->ad || s->sr || s->ptr[0] || s->ptr[1] || s->ptr[2] || s->vibdelay || s->ptr[3]) && c > hi)
            hi = c;
    }
    if (hi >= GT2_MAX_INSTR)
        hi = GT2_MAX_INSTR - 1;
    for (i = 0; i < 4; i++)
        PUT("GTS5"[i]);
    for (i = 0; i < 32; i++) PUT(g->songname[i]);
    for (i = 0; i < 32; i++) PUT(g->authorname[i]);
    for (i = 0; i < 32; i++) PUT(g->copyrightname[i]);
    for (amount = GT2_MAX_SONGS; amount > 1; amount--)
        if (g->songlen[amount - 1][0] && g->songlen[amount - 1][1] && g->songlen[amount - 1][2])
            break;
    PUT(amount);
    for (d = 0; d < amount; d++)
        for (c = 0; c < 3; c++) {
            n = g->songlen[d][c] + 1u;
            PUT(n);
            for (i = 0; i <= n; i++)
                PUT(gt2_ord(g, d, c, i));
        }
    PUT(hi);
    for (c = 1; c <= hi; c++) {
        const gt2_ins_t *s = &g->instr[c];
        PUT(s->ad); PUT(s->sr);
        PUT(s->ptr[GT2_WTBL]); PUT(s->ptr[GT2_PTBL]); PUT(s->ptr[GT2_FTBL]); PUT(s->ptr[GT2_STBL]);
        PUT(s->vibdelay); PUT(s->gatetimer); PUT(s->firstwave);
        for (i = 0; i < 16; i++)
            PUT(s->name[i]);
    }
    for (c = 0; c < 4; c++) {              /* gtable.c gettablelen() */
        for (n = GT2_TABLELEN; n > 0; n--)
            if (g->ltable[c][n - 1] | g->rtable[c][n - 1])
                break;
        PUT(n);
        for (i = 0; i < n; i++) PUT(g->ltable[c][i]);
        for (i = 0; i < n; i++) PUT(g->rtable[c][i]);
    }
    PUT(hp + 1u);
    for (c = 0; c <= hp; c++) {
        n = g->pattlen[c] + 1u;
        PUT(n);
        for (i = 0; i < n * 4u; i++)
            PUT(i + 4u >= n * 4u ? (i % 4u ? 0 : GT2_ENDPATT) : gt2_pat(g, c, i));
    }
#undef PUT
    return pos;
}

struct mem { uint8_t *out; uint32_t max, n; };
static void put_mem(void *user, uint8_t b)
{
    struct mem *m = user;
    if (m->n < m->max)
        m->out[m->n] = b;
    m->n++;
}

uint32_t gt2_save(const gt2_t *g, uint8_t *out, uint32_t max)
{
    struct mem m = {out, max, 0};
    uint32_t n = gt2_save_to(g, put_mem, &m);
    return n <= max ? n : 0;
}
