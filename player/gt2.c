/* player/gt2.c - GoatTracker 2 songs played from the .sng file itself (player/gt2.h).
 *
 * The playroutine is GoatTracker v2.76's gplay.c (Copyright Lasse Oorni and contributors,
 * GPL-2.0-or-later), kept statement for statement (without the editor's end mark), with its
 * array reads going through the index: PAT, ORD, LT, RT below.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "gt2.h"

#define MAX_INSTR GT2_MAX_INSTR
#define MAX_PATT GT2_MAX_PATT
#define MAX_TABLELEN GT2_TABLELEN
#define MAX_PATTROWS GT2_MAX_ROWS
#define MAX_SONGLEN GT2_MAX_SONGLEN
#define MAX_SONGS GT2_MAX_SONGS
#define DEFPATTLEN GT2_DEFROWS

#define CMD_DONOTHING 0
#define CMD_PORTAUP 1
#define CMD_PORTADOWN 2
#define CMD_TONEPORTA 3
#define CMD_VIBRATO 4
#define CMD_SETAD 5
#define CMD_SETSR 6
#define CMD_SETWAVE 7
#define CMD_SETWAVEPTR 8
#define CMD_SETPULSEPTR 9
#define CMD_SETFILTERPTR 10
#define CMD_SETFILTERCTRL 11
#define CMD_SETFILTERCUTOFF 12
#define CMD_SETMASTERVOL 13
#define CMD_FUNKTEMPO 14
#define CMD_SETTEMPO 15

#define WTBL 0
#define PTBL 1
#define FTBL 2
#define STBL 3

#define REPEAT 0xd0
#define TRANSDOWN 0xe0
#define TRANSUP 0xf0
#define LOOPSONG 0xff
#define ENDPATT 0xff
#define FIRSTNOTE 0x60
#define LASTNOTE 0xbc
#define REST 0xbd
#define KEYOFF 0xbe
#define KEYON 0xbf
#define WAVELASTDELAY 0xf
#define WAVESILENT 0xe0
#define WAVELASTSILENT 0xef
#define WAVECMD 0xf0
#define WAVELASTCMD 0xfe

#define PLAY_BEGINNING 0x01
#define PLAY_POS 0x02
#define PLAY_PATTERN 0x03
#define PLAY_STOP 0x04
#define PLAY_STOPPED 0x80

#define ADPARAM 0x0f00                     /* the editor's defaults (goattrk2.c) */
#define TEMPO (6 - 1)
#define NREGS 25
#define WRITEDELAY 14                      /* gsid.h SIDWRITEDELAY */

typedef char gt2_fits[sizeof(gt2_t) <= GT2_SIZE_MAX ? 1 : -1];

static const uint8_t freqtbllo[129] = {
    0x17,0x27,0x39,0x4b,0x5f,0x74,0x8a,0xa1,0xba,0xd4,0xf0,0x0e,
    0x2d,0x4e,0x71,0x96,0xbe,0xe8,0x14,0x43,0x74,0xa9,0xe1,0x1c,
    0x5a,0x9c,0xe2,0x2d,0x7c,0xcf,0x28,0x85,0xe8,0x52,0xc1,0x37,
    0xb4,0x39,0xc5,0x5a,0xf7,0x9e,0x4f,0x0a,0xd1,0xa3,0x82,0x6e,
    0x68,0x71,0x8a,0xb3,0xee,0x3c,0x9e,0x15,0xa2,0x46,0x04,0xdc,
    0xd0,0xe2,0x14,0x67,0xdd,0x79,0x3c,0x29,0x44,0x8d,0x08,0xb8,
    0xa1,0xc5,0x28,0xcd,0xba,0xf1,0x78,0x53,0x87,0x1a,0x10,0x71,
    0x42,0x89,0x4f,0x9b,0x74,0xe2,0xf0,0xa6,0x0e,0x33,0x20,0xff,
    [128] = 0x01};                         /* (gplay.c reads one past its table: freqtblhi[0]) */

static const uint8_t freqtblhi[129] = {
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x02,
    0x02,0x02,0x02,0x02,0x02,0x02,0x03,0x03,0x03,0x03,0x03,0x04,
    0x04,0x04,0x04,0x05,0x05,0x05,0x06,0x06,0x06,0x07,0x07,0x08,
    0x08,0x09,0x09,0x0a,0x0a,0x0b,0x0c,0x0d,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x17,0x18,0x1a,0x1b,0x1d,0x1f,0x20,
    0x22,0x24,0x27,0x29,0x2b,0x2e,0x31,0x34,0x37,0x3a,0x3e,0x41,
    0x45,0x49,0x4e,0x52,0x57,0x5c,0x62,0x68,0x6e,0x75,0x7c,0x83,
    0x8b,0x93,0x9c,0xa5,0xaf,0xb9,0xc4,0xd0,0xdd,0xea,0xf8,0xff};

static const uint8_t sidorder[NREGS] = {
    0x18,0x17,0x16,0x15,
    0x14,0x13,0x12,0x11,0x10,0x0f,0x0e,
    0x0d,0x0c,0x0b,0x0a,0x09,0x08,0x07,
    0x06,0x05,0x04,0x03,0x02,0x01,0x00};

/* ------------------------------------------------------------------------ the index --- */
/* byte i of pattern p as GoatTracker's array holds it: the file's, else a cleared pattern's
 * (an edited pattern in the pool: past its end row the end, where a channel that played past a
 * shortened pattern's end finds it) */
static uint8_t PAT(const gt2_t *g, uint32_t p, uint32_t i)
{
    if (p < MAX_PATT && i < g->pattn[p])
        return g->patt[p][i];
    if (i % 4u)
        return 0;
    if (g->pool && p < MAX_PATT && g->patt[p])
        return ENDPATT;
    return i / 4u < DEFPATTLEN ? REST : ENDPATT;
}

/* entry i of channel c's order list in subtune song: its bytes, else as GoatTracker's cleared
 * list (past a list's bytes 0; a subtune the song does not have: the end at once) */
static uint8_t ORDS(const gt2_t *g, uint32_t song, uint32_t c, uint32_t i)
{
    if (i < g->ordn[song][c])
        return g->order[song][c][i];
    return !g->ordn[song][c] && !i ? LOOPSONG : 0;
}
#define ORD(g, c, i) ORDS(g, (g)->psnum, c, i)

uint8_t gt2_pat(const gt2_t *g, uint32_t p, uint32_t i) { return PAT(g, p, i); }
uint8_t gt2_ord(const gt2_t *g, uint32_t song, uint32_t c, uint32_t i) { return song < MAX_SONGS ? ORDS(g, song, c, i) : LOOPSONG; }

static uint8_t LT(const gt2_t *g, int t, int i) { return i >= 0 && i < MAX_TABLELEN ? g->ltable[t][i] : 0; }
static uint8_t RT(const gt2_t *g, int t, int i) { return i >= 0 && i < MAX_TABLELEN ? g->rtable[t][i] : 0; }

static uint16_t FREQ(uint32_t n) { return (uint16_t)(freqtbllo[n & 0x7f] | freqtblhi[n & 0x7f] << 8); }
static uint16_t FREQ1(uint32_t n) { n = (n & 0x7f) + 1; return (uint16_t)(freqtbllo[n] | freqtblhi[n] << 8); }

static void text(char *d, const char *s)
{
    int i;
    for (i = 0; i < 32 && s[i]; i++)
        d[i] = (char)s[i];
    d[i] = 0;
}

uint32_t gt2_size(void) { return sizeof(gt2_t); }

const psid_info_t *gt2_info(const gt2_t *g)
{
    psid_info_t *f = (psid_info_t *)&g->info;   /* (from the texts, which the editor changes) */
    text(f->name, g->songname);
    text(f->author, g->authorname);
    text(f->released, g->copyrightname);
    f->songs = (uint16_t)g->nsongs;
    f->start = 1;
    return f;
}
int gt2_playing(const gt2_t *g) { return g->songinit != PLAY_STOPPED; }

int gt2_load(gt2_t *g, const uint8_t *b, uint32_t len, psid_write_fn wr, void *user)
{
    uint32_t pos = 100, amount, c, d, n, i;
    uint8_t ver;
    uint8_t *z = (uint8_t *)g;
    for (i = 0; i < sizeof *g; i++)
        z[i] = 0;
    g->wr = wr;
    g->user = user;
    if (len < pos + 1 || b[0] != 'G' || b[1] != 'T' || b[2] != 'S' || b[3] < '3' || b[3] > '5')
        return -1;
    ver = b[3];
    for (i = 0; i < 32; i++) {
        g->songname[i] = (char)b[4 + i];
        g->authorname[i] = (char)b[36 + i];
        g->copyrightname[i] = (char)b[68 + i];
    }
    /* order lists: amount, then per subtune and channel a length L and L + 1 bytes */
    amount = b[pos++];
    if (amount > MAX_SONGS)
        return -1;
    for (d = 0; d < amount; d++)
        for (c = 0; c < 3; c++) {
            if (pos >= len)
                return -1;
            n = b[pos++] + 1u;
            if (n > len - pos)
                return -1;
            g->order[d][c] = b + pos;
            g->ordn[d][c] = (uint16_t)n;
            pos += n;
        }
    /* instruments 1..amount, 9 bytes and a 16-byte name each; the rest as cleared */
    if (pos >= len || (amount = b[pos++]) >= MAX_INSTR || amount * 25u > len - pos)
        return -1;
    for (c = 1; c < MAX_INSTR; c++) {
        gt2_ins_t *s = &g->instr[c];
        if (c <= amount) {
            const uint8_t *r = b + pos + (c - 1u) * 25u;
            s->ad = r[0]; s->sr = r[1];
            s->ptr[WTBL] = r[2]; s->ptr[PTBL] = r[3]; s->ptr[FTBL] = r[4]; s->ptr[STBL] = r[5];
            s->vibdelay = r[6]; s->gatetimer = r[7]; s->firstwave = r[8];
            for (i = 0; i < 16; i++)
                s->name[i] = (char)r[9 + i];
        } else {
            s->gatetimer = 2;              /* ginstr.c clearinstr() at 1x */
            s->firstwave = 0x9;
        }
    }
    pos += amount * 25u;
    /* the wave, pulse, filter and speed tables: a length, then the left and the right column */
    for (c = 0; c < 4; c++) {
        if (pos >= len)
            return -1;
        n = b[pos++];
        if (n > MAX_TABLELEN || 2u * n > len - pos)
            return -1;
        for (i = 0; i < n; i++) {
            g->ltable[c][i] = b[pos + i];
            g->rtable[c][i] = b[pos + n + i];
        }
        pos += 2u * n;
    }
    /* patterns: amount, then per pattern its rows (with the end mark) and 4 bytes a row */
    if (pos >= len || (amount = b[pos++]) > MAX_PATT)
        return -1;
    for (c = 0; c < amount; c++) {
        if (pos >= len)
            return -1;
        n = b[pos++] * 4u;
        if (n > (MAX_PATTROWS + 1u) * 4u || n > len - pos)
            return -1;
        g->patt[c] = b + pos;
        g->pattn[c] = (uint16_t)n;
        pos += n;
    }
    /* gsong.c countpatternlengths() */
    for (c = 0; c < MAX_PATT; c++) {
        for (d = 0; d <= MAX_PATTROWS; d++)
            if (PAT(g, c, d * 4u) == ENDPATT)
                break;
        g->pattlen[c] = (uint8_t)d;
    }
    for (g->psnum = 0; g->psnum < MAX_SONGS; g->psnum++)
        for (c = 0; c < 3; c++) {
            for (d = 0; d < MAX_SONGLEN; d++)
                if (ORD(g, c, d) >= LOOPSONG)
                    break;
            g->songlen[g->psnum][c] = (uint8_t)d;
        }
    g->psnum = 0;
    /* gsong.c loadsong(): the older versions' pulse speeds and legato / no hard restart bits */
    if (ver < '4')
        for (c = 0; c < MAX_TABLELEN; c++)
            if (g->ltable[PTBL][c] < 0x80 && g->rtable[PTBL][c]) {
                int speed = (int8_t)g->rtable[PTBL][c] * 2;
                g->rtable[PTBL][c] = (uint8_t)(speed > 127 ? 127 : speed < -128 ? -128 : speed);
            }
    if (ver < '5')
        for (c = 1; c < MAX_INSTR; c++) {
            if (g->instr[c].firstwave >= 0x80) {
                g->instr[c].gatetimer |= 0x80;
                g->instr[c].firstwave &= 0x7f;
            }
            if (!g->instr[c].firstwave)
                g->instr[c].gatetimer |= 0x40;
        }
    g->nsongs = 0;
    for (d = 0; d < MAX_SONGS && g->order[d][0]; d++)
        g->nsongs = d + 1;
    if (!g->nsongs || !g->songlen[0][0] || !g->songlen[0][1] || !g->songlen[0][2])
        return -1;                         /* no subtune, or the first one empty */
    g->songinit = PLAY_STOPPED;
    return 0;
}

/* ------------------------------------------------------------------ the playroutine --- */
static void stopsong(gt2_t *g)
{
    if (g->songinit != PLAY_STOPPED)
        g->songinit = PLAY_STOP;
}

static void sequencer(gt2_t *g, int c, gt2_chn_t *cptr)
{
    if ((g->songinit != PLAY_STOPPED) && (cptr->pattptr == 0x7fffffff)) {
        cptr->pattptr = g->startpattpos * 4u;
        if (!cptr->advance) return;
        /* Song loop */
        if (ORD(g, c, cptr->songptr) == LOOPSONG) {
            cptr->songptr = ORD(g, c, cptr->songptr + 1u);
            if (cptr->songptr >= g->songlen[g->psnum][c]) {
                stopsong(g);
                cptr->songptr = 0;
                return;
            }
        }
        /* Transpose */
        if ((ORD(g, c, cptr->songptr) >= TRANSDOWN) && (ORD(g, c, cptr->songptr) < LOOPSONG)) {
            cptr->trans = (uint8_t)(ORD(g, c, cptr->songptr) - TRANSUP);
            cptr->songptr++;
        }
        /* Repeat */
        if ((ORD(g, c, cptr->songptr) >= REPEAT) && (ORD(g, c, cptr->songptr) < TRANSDOWN)) {
            cptr->repeat = (uint8_t)(ORD(g, c, cptr->songptr) - REPEAT);
            cptr->songptr++;
        }
        /* Pattern number */
        cptr->pattnum = ORD(g, c, cptr->songptr);
        if (cptr->repeat)
            cptr->repeat--;
        else
            cptr->songptr++;
        /* Check for illegal pattern now */
        if (cptr->pattnum >= MAX_PATT) {
            stopsong(g);
            cptr->pattnum = 0;
        }
        if (cptr->pattptr >= (uint32_t)(g->pattlen[cptr->pattnum] * 4))
            cptr->pattptr = 0;
    }
}

/* (a shift of 16 or more: 0, where gplay.c's C is undefined) */
static uint16_t shr(uint16_t v, uint8_t n) { return n < 16 ? (uint16_t)(v >> n) : 0; }

/* the portamento / vibrato speed of speed table entry param: a fixed one, or (bit 15 set)
 * the note's step shifted right */
static uint16_t speed16(const gt2_t *g, const gt2_chn_t *cptr, uint8_t param)
{
    uint16_t speed = 0;
    if (param)
        speed = (uint16_t)(LT(g, STBL, param - 1) << 8 | RT(g, STBL, param - 1));
    if (speed >= 0x8000) {
        speed = FREQ1(cptr->lastnote);
        speed -= FREQ(cptr->lastnote);
        speed = shr(speed, RT(g, STBL, param - 1));
    }
    return speed;
}

static void toneporta(const gt2_t *g, gt2_chn_t *cptr, uint8_t param)
{
    uint16_t targetfreq = FREQ(cptr->note);
    uint16_t speed;
    if (!param) {
        cptr->freq = targetfreq;
        cptr->vibtime = 0;
    } else {
        speed = speed16(g, cptr, param);
        if (cptr->freq < targetfreq) {
            cptr->freq += speed;
            if (cptr->freq > targetfreq) {
                cptr->freq = targetfreq;
                cptr->vibtime = 0;
            }
        }
        if (cptr->freq > targetfreq) {
            cptr->freq -= speed;
            if (cptr->freq < targetfreq) {
                cptr->freq = targetfreq;
                cptr->vibtime = 0;
            }
        }
    }
}

static void vibrato(const gt2_t *g, gt2_chn_t *cptr, uint8_t param)
{
    uint16_t speed = 0;
    uint8_t cmpvalue = 0;
    if (param) {
        cmpvalue = LT(g, STBL, param - 1);
        speed = RT(g, STBL, param - 1);
    }
    if (cmpvalue >= 0x80) {
        cmpvalue &= 0x7f;
        speed = FREQ1(cptr->lastnote);
        speed -= FREQ(cptr->lastnote);
        speed = shr(speed, RT(g, STBL, param - 1));
    }
    if ((cptr->vibtime < 0x80) && (cptr->vibtime > cmpvalue))
        cptr->vibtime ^= 0xff;
    cptr->vibtime += 0x02;
    if (cptr->vibtime & 0x01)
        cptr->freq -= speed;
    else
        cptr->freq += speed;
}

/* a table pointer just set: GoatTracker stops a song that jumps into a jump */
static void check_jump(gt2_t *g, int t, uint8_t ptr)
{
    if (ptr && LT(g, t, ptr - 1) == 0xff)
        stopsong(g);
}

static void playroutine(gt2_t *g)
{
    const gt2_ins_t *iptr;
    gt2_chn_t *cptr = &g->chn[0];
    int c;

    if ((g->songinit > 0) && (g->songinit < PLAY_STOPPED)) {
        g->filterctrl = 0;
        g->filterptr = 0;
        if ((g->songinit == PLAY_POS) || (g->songinit == PLAY_PATTERN))
            for (c = 0; c < 3; c++)
                if (g->espos[c] >= g->songlen[g->psnum][c])
                    g->songinit = PLAY_BEGINNING;
        for (c = 0; c < 3; c++) {
            cptr->songptr = 0;
            cptr->command = 0;
            cptr->cmddata = 0;
            cptr->newcommand = 0;
            cptr->newcmddata = 0;
            cptr->advance = 1;
            cptr->wave = 0;
            cptr->ptr[WTBL] = 0;
            cptr->newnote = 0;
            cptr->repeat = 0;
            cptr->tick = TEMPO;
            cptr->gatetimer = g->instr[1].gatetimer & 0x3f;
            cptr->pattptr = 0x7fffffff;
            if (cptr->tempo < 2) cptr->tempo = 0;
            if (g->songinit == PLAY_BEGINNING) {
                g->funktable[0] = 9 - 1;
                g->funktable[1] = 6 - 1;
                cptr->tempo = TEMPO;
                if ((g->instr[MAX_INSTR - 1].ad >= 2) && (!(g->instr[MAX_INSTR - 1].ptr[WTBL])))
                    cptr->tempo = g->instr[MAX_INSTR - 1].ad - 1;
                cptr->trans = 0;
                cptr->instr = 1;
                sequencer(g, c, cptr);
            } else if (g->songinit == PLAY_PATTERN) {
                cptr->advance = 0;
                cptr->pattptr = g->startpattpos * 4u;
                cptr->pattnum = g->epnum[c];
                if (cptr->pattptr >= (uint32_t)(g->pattlen[cptr->pattnum] * 4))
                    cptr->pattptr = 0;
            } else if (g->songinit == PLAY_POS) {
                cptr->songptr = g->espos[c];
                sequencer(g, c, cptr);
            }
            cptr++;
        }
        if (g->songinit != PLAY_STOP)
            g->songinit = 0;
        else
            g->songinit = PLAY_STOPPED;
        if ((!g->songlen[g->psnum][0]) || (!g->songlen[g->psnum][1]) || (!g->songlen[g->psnum][2]))
            g->songinit = PLAY_STOPPED;    /* Zero length song */
        g->startpattpos = 0;
        return;
    }

    if (g->filterptr) {
        /* Filter jump */
        if (LT(g, FTBL, g->filterptr - 1) == 0xff) {
            g->filterptr = RT(g, FTBL, g->filterptr - 1);
            if (!g->filterptr) goto FILTERSTOP;
        }
        if (!g->filtertime) {
            /* Filter set */
            if (LT(g, FTBL, g->filterptr - 1) >= 0x80) {
                g->filtertype = LT(g, FTBL, g->filterptr - 1) & 0x70;
                g->filterctrl = RT(g, FTBL, g->filterptr - 1);
                g->filterptr++;
                /* Can be combined with cutoff set */
                if (LT(g, FTBL, g->filterptr - 1) == 0x00) {
                    g->filtercutoff = RT(g, FTBL, g->filterptr - 1);
                    g->filterptr++;
                }
            } else {
                /* New modulation step */
                if (LT(g, FTBL, g->filterptr - 1))
                    g->filtertime = LT(g, FTBL, g->filterptr - 1);
                else {
                    /* Cutoff set */
                    g->filtercutoff = RT(g, FTBL, g->filterptr - 1);
                    g->filterptr++;
                }
            }
        }
        /* Filter modulation */
        if (g->filtertime) {
            g->filtercutoff += RT(g, FTBL, g->filterptr - 1);
            g->filtertime--;
            if (!g->filtertime) g->filterptr++;
        }
    }
FILTERSTOP:
    g->sidreg[0x15] = 0x00;
    g->sidreg[0x16] = g->filtercutoff;
    g->sidreg[0x17] = g->filterctrl;
    g->sidreg[0x18] = g->filtertype | g->masterfader;

    for (c = 0; c < 3; c++) {
        iptr = &g->instr[cptr->instr & (MAX_INSTR - 1)];

        /* Reset tempo in jammode */
        if ((g->songinit == PLAY_STOPPED) && (cptr->tempo < 2))
            cptr->tempo = TEMPO;

        /* Decrease tick */
        cptr->tick--;
        if (!cptr->tick) goto TICK0;

        /* Tick N: reload counter */
        if (cptr->tick >= 0x80) {
            if (cptr->tempo >= 2)
                cptr->tick = cptr->tempo;
            else {
                /* Set funktempo, switch between 2 values */
                cptr->tick = g->funktable[cptr->tempo];
                cptr->tempo ^= 1;
            }
            /* Check for illegally high gatetimer and stop the song in this case (gplay.c
             * compares channel 1's) */
            if (g->chn[0].gatetimer > cptr->tick)
                stopsong(g);
        }
        goto WAVEEXEC;

    TICK0:
        /* Advance in sequencer */
        sequencer(g, c, cptr);

        /* Get gatetimer compare-value */
        cptr->gatetimer = iptr->gatetimer & 0x3f;

        /* New note init */
        if (cptr->newnote) {
            cptr->note = cptr->newnote - FIRSTNOTE;
            cptr->command = 0;
            cptr->vibdelay = iptr->vibdelay;
            cptr->cmddata = iptr->ptr[STBL];
            if (cptr->newcommand != CMD_TONEPORTA) {
                if (iptr->firstwave) {
                    if (iptr->firstwave >= 0xfe) cptr->gate = iptr->firstwave;
                    else {
                        cptr->wave = iptr->firstwave;
                        cptr->gate = 0xff;
                    }
                }
                cptr->ptr[WTBL] = iptr->ptr[WTBL];
                check_jump(g, WTBL, cptr->ptr[WTBL]);
                if (iptr->ptr[PTBL]) {
                    cptr->ptr[PTBL] = iptr->ptr[PTBL];
                    cptr->pulsetime = 0;
                    check_jump(g, PTBL, cptr->ptr[PTBL]);
                }
                if (iptr->ptr[FTBL]) {
                    g->filterptr = iptr->ptr[FTBL];
                    g->filtertime = 0;
                    check_jump(g, FTBL, g->filterptr);
                }
                g->sidreg[0x5 + 7 * c] = iptr->ad;
                g->sidreg[0x6 + 7 * c] = iptr->sr;
            }
        }

        /* Tick 0 effects */
        switch (cptr->newcommand) {
        case CMD_DONOTHING:
            cptr->command = 0;
            cptr->cmddata = iptr->ptr[STBL];
            break;
        case CMD_PORTAUP:
        case CMD_PORTADOWN:
            cptr->vibtime = 0;
            cptr->command = cptr->newcommand;
            cptr->cmddata = cptr->newcmddata;
            break;
        case CMD_TONEPORTA:
        case CMD_VIBRATO:
            cptr->command = cptr->newcommand;
            cptr->cmddata = cptr->newcmddata;
            break;
        case CMD_SETAD:
            g->sidreg[0x5 + 7 * c] = cptr->newcmddata;
            break;
        case CMD_SETSR:
            g->sidreg[0x6 + 7 * c] = cptr->newcmddata;
            break;
        case CMD_SETWAVE:
            cptr->wave = cptr->newcmddata;
            break;
        case CMD_SETWAVEPTR:
            cptr->ptr[WTBL] = cptr->newcmddata;
            cptr->wavetime = 0;
            check_jump(g, WTBL, cptr->ptr[WTBL]);
            break;
        case CMD_SETPULSEPTR:
            cptr->ptr[PTBL] = cptr->newcmddata;
            cptr->pulsetime = 0;
            check_jump(g, PTBL, cptr->ptr[PTBL]);
            break;
        case CMD_SETFILTERPTR:
            g->filterptr = cptr->newcmddata;
            g->filtertime = 0;
            check_jump(g, FTBL, g->filterptr);
            break;
        case CMD_SETFILTERCTRL:
            g->filterctrl = cptr->newcmddata;
            if (!g->filterctrl) g->filterptr = 0;
            break;
        case CMD_SETFILTERCUTOFF:
            g->filtercutoff = cptr->newcmddata;
            break;
        case CMD_SETMASTERVOL:
            if (cptr->newcmddata < 0x10)
                g->masterfader = cptr->newcmddata;
            break;
        case CMD_FUNKTEMPO:
            if (cptr->newcmddata) {
                g->funktable[0] = LT(g, STBL, cptr->newcmddata - 1) - 1;
                g->funktable[1] = RT(g, STBL, cptr->newcmddata - 1) - 1;
            }
            g->chn[0].tempo = 0;
            g->chn[1].tempo = 0;
            g->chn[2].tempo = 0;
            break;
        case CMD_SETTEMPO: {
            uint8_t newtempo = cptr->newcmddata & 0x7f;
            if (newtempo >= 3) newtempo--;
            if (cptr->newcmddata >= 0x80)
                cptr->tempo = newtempo;
            else {
                g->chn[0].tempo = newtempo;
                g->chn[1].tempo = newtempo;
                g->chn[2].tempo = newtempo;
            }
            break;
        }
        }
        if (cptr->newnote) {
            cptr->newnote = 0;
            if (cptr->newcommand != CMD_TONEPORTA) goto NEXTCHN;
        }

    WAVEEXEC:
        if (cptr->ptr[WTBL]) {
            uint8_t wave = LT(g, WTBL, cptr->ptr[WTBL] - 1);
            uint8_t note = RT(g, WTBL, cptr->ptr[WTBL] - 1);

            if (wave > WAVELASTDELAY) {
                /* Normal waveform values */
                if (wave < WAVESILENT) cptr->wave = wave;
                /* Values without waveform selected */
                if ((wave >= WAVESILENT) && (wave <= WAVELASTSILENT)) cptr->wave = wave & 0xf;
                /* Command execution from wavetable */
                if ((wave >= WAVECMD) && (wave <= WAVELASTCMD)) {
                    uint8_t param = RT(g, WTBL, cptr->ptr[WTBL] - 1);
                    switch (wave & 0xf) {
                    case CMD_DONOTHING:
                    case CMD_SETWAVEPTR:
                    case CMD_FUNKTEMPO:
                        stopsong(g);
                        break;
                    case CMD_PORTAUP:
                        cptr->freq += speed16(g, cptr, param);
                        break;
                    case CMD_PORTADOWN:
                        cptr->freq -= speed16(g, cptr, param);
                        break;
                    case CMD_TONEPORTA:
                        toneporta(g, cptr, param);
                        break;
                    case CMD_VIBRATO:
                        vibrato(g, cptr, param);
                        break;
                    case CMD_SETAD:
                        g->sidreg[0x5 + 7 * c] = param;
                        break;
                    case CMD_SETSR:
                        g->sidreg[0x6 + 7 * c] = param;
                        break;
                    case CMD_SETWAVE:
                        cptr->wave = param;
                        break;
                    case CMD_SETPULSEPTR:
                        cptr->ptr[PTBL] = param;
                        cptr->pulsetime = 0;
                        check_jump(g, PTBL, cptr->ptr[PTBL]);
                        break;
                    case CMD_SETFILTERPTR:
                        g->filterptr = param;
                        g->filtertime = 0;
                        check_jump(g, FTBL, g->filterptr);
                        break;
                    case CMD_SETFILTERCTRL:
                        g->filterctrl = param;
                        if (!g->filterctrl) g->filterptr = 0;
                        break;
                    case CMD_SETFILTERCUTOFF:
                        g->filtercutoff = param;
                        break;
                    case CMD_SETMASTERVOL:
                        if (param < 0x10)
                            g->masterfader = param;
                        break;
                    }
                }
            } else {
                /* Wavetable delay */
                if (cptr->wavetime != wave) {
                    cptr->wavetime++;
                    goto TICKNEFFECTS;
                }
            }

            cptr->wavetime = 0;
            cptr->ptr[WTBL]++;
            /* Wavetable jump */
            if (LT(g, WTBL, cptr->ptr[WTBL] - 1) == 0xff)
                cptr->ptr[WTBL] = RT(g, WTBL, cptr->ptr[WTBL] - 1);

            if ((wave >= WAVECMD) && (wave <= WAVELASTCMD))
                goto PULSEEXEC;

            if (note != 0x80) {
                if (note < 0x80)
                    note += cptr->note;
                note &= 0x7f;
                cptr->freq = FREQ(note);
                cptr->vibtime = 0;
                cptr->lastnote = note;
                goto PULSEEXEC;
            }
        }

    TICKNEFFECTS:
        /* Tick N command (the realtime optimization: not on tick 0) */
        if (cptr->tick) {
            switch (cptr->command) {
            case CMD_PORTAUP:
                cptr->freq += speed16(g, cptr, cptr->cmddata);
                break;
            case CMD_PORTADOWN:
                cptr->freq -= speed16(g, cptr, cptr->cmddata);
                break;
            case CMD_DONOTHING:
                if ((!cptr->cmddata) || (!cptr->vibdelay))
                    break;
                if (cptr->vibdelay > 1) {
                    cptr->vibdelay--;
                    break;
                }
                /* fall through */
            case CMD_VIBRATO:
                vibrato(g, cptr, cptr->cmddata);
                break;
            case CMD_TONEPORTA:
                toneporta(g, cptr, cptr->cmddata);
                break;
            }
        }

    PULSEEXEC:
        /* (the pulse optimization) */
        if ((g->songinit != PLAY_STOPPED) && (cptr->tick == cptr->gatetimer)) goto GETNEWNOTES;

        if (cptr->ptr[PTBL]) {
            /* Skip pulse when sequencer has been executed */
            if ((!cptr->tick) && (!cptr->pattptr)) goto NEXTCHN;

            /* Pulsetable jump */
            if (LT(g, PTBL, cptr->ptr[PTBL] - 1) == 0xff) {
                cptr->ptr[PTBL] = RT(g, PTBL, cptr->ptr[PTBL] - 1);
                if (!cptr->ptr[PTBL]) goto PULSEEXEC;
            }

            if (!cptr->pulsetime) {
                /* Set pulse */
                if (LT(g, PTBL, cptr->ptr[PTBL] - 1) >= 0x80) {
                    cptr->pulse = (uint16_t)((LT(g, PTBL, cptr->ptr[PTBL] - 1) & 0xf) << 8);
                    cptr->pulse |= RT(g, PTBL, cptr->ptr[PTBL] - 1);
                    cptr->ptr[PTBL]++;
                } else {
                    cptr->pulsetime = LT(g, PTBL, cptr->ptr[PTBL] - 1);
                }
            }
            /* Pulse modulation */
            if (cptr->pulsetime) {
                uint8_t speed = RT(g, PTBL, cptr->ptr[PTBL] - 1);
                if (speed < 0x80) {
                    cptr->pulse += speed;
                    cptr->pulse &= 0xfff;
                } else {
                    cptr->pulse += speed;
                    cptr->pulse -= 0x100;
                    cptr->pulse &= 0xfff;
                }
                cptr->pulsetime--;
                if (!cptr->pulsetime) cptr->ptr[PTBL]++;
            }
        }
        if ((g->songinit == PLAY_STOPPED) || (cptr->tick != cptr->gatetimer)) goto NEXTCHN;

    GETNEWNOTES: {
            /* New notes processing */
            uint8_t newnote = PAT(g, cptr->pattnum, cptr->pattptr);
            if (PAT(g, cptr->pattnum, cptr->pattptr + 1))
                cptr->instr = PAT(g, cptr->pattnum, cptr->pattptr + 1);
            cptr->newcommand = PAT(g, cptr->pattnum, cptr->pattptr + 2);
            cptr->newcmddata = PAT(g, cptr->pattnum, cptr->pattptr + 3);
            cptr->pattptr += 4;
            if (PAT(g, cptr->pattnum, cptr->pattptr) == ENDPATT)
                cptr->pattptr = 0x7fffffff;

            if (newnote == KEYOFF)
                cptr->gate = 0xfe;
            if (newnote == KEYON)
                cptr->gate = 0xff;
            if (newnote <= LASTNOTE) {
                cptr->newnote = newnote + cptr->trans;
                if ((cptr->newcommand) != CMD_TONEPORTA) {
                    if (!(g->instr[cptr->instr & (MAX_INSTR - 1)].gatetimer & 0x40)) {
                        cptr->gate = 0xfe;
                        if (!(g->instr[cptr->instr & (MAX_INSTR - 1)].gatetimer & 0x80)) {
                            g->sidreg[0x5 + 7 * c] = ADPARAM >> 8;
                            g->sidreg[0x6 + 7 * c] = ADPARAM & 0xff;
                        }
                    }
                }
            }
        }
    NEXTCHN:
        if (cptr->mute)
            g->sidreg[0x4 + 7 * c] = cptr->wave = 0x08;
        else {
            g->sidreg[0x0 + 7 * c] = cptr->freq & 0xff;
            g->sidreg[0x1 + 7 * c] = cptr->freq >> 8;
            g->sidreg[0x2 + 7 * c] = cptr->pulse & 0xfe;
            g->sidreg[0x3 + 7 * c] = cptr->pulse >> 8;
            g->sidreg[0x4 + 7 * c] = cptr->wave & cptr->gate;
        }
        cptr++;
    }
}

void gt2_reset(gt2_t *g)
{
    uint32_t c;
    uint8_t *z = (uint8_t *)g->chn;
    for (c = 0; c < sizeof g->chn; c++)
        z[c] = 0;
    for (c = 0; c < NREGS; c++)
        g->sidreg[c] = 0;
    /* gplay.c initchannels(), gsong.c clearsong() */
    for (c = 0; c < 3; c++) {
        g->chn[c].instr = 1;
        g->chn[c].tempo = TEMPO;
    }
    g->funktable[0] = 9 - 1;
    g->funktable[1] = 6 - 1;
    g->masterfader = 0x0f;
    g->filterctrl = g->filtertype = g->filtercutoff = g->filtertime = g->filterptr = 0;
    g->songinit = PLAY_STOPPED;
}

uint32_t gt2_init(gt2_t *g, uint32_t song)
{
    gt2_reset(g);
    song = song ? song - 1u : 0u;
    if (song >= g->nsongs || !g->songlen[song][0] || !g->songlen[song][1] || !g->songlen[song][2])
        song = 0;
    g->psnum = (uint8_t)song;
    /* initsong(PLAY_BEGINNING); its first playroutine call sets the channels up, writes nothing */
    g->songinit = PLAY_BEGINNING;
    playroutine(g);
    return 0;
}

uint32_t gt2_play(gt2_t *g)
{
    uint32_t i;
    playroutine(g);
    for (i = 0; i < NREGS; i++)
        g->wr(g->user, i * WRITEDELAY, sidorder[i], g->sidreg[sidorder[i]]);
    return NREGS * WRITEDELAY;
}

void gt2_start(gt2_t *g, int mode, uint32_t song, const uint8_t pos[3], const uint8_t patt[3], uint32_t row)
{
    uint32_t c;
    g->psnum = (uint8_t)(song < MAX_SONGS ? song : 0);
    for (c = 0; c < 3; c++) {
        g->espos[c] = pos ? pos[c] : 0;
        g->epnum[c] = patt ? patt[c] : (uint8_t)c;
    }
    g->startpattpos = (uint8_t)(row < MAX_PATTROWS ? row : 0);
    g->songinit = (uint8_t)mode;
}

void gt2_stop(gt2_t *g) { stopsong(g); }

/* gplay.c playtestnote() */
void gt2_jam(gt2_t *g, uint32_t note, uint32_t ins, uint32_t c)
{
    gt2_chn_t *cptr = &g->chn[c % 3u];
    ins &= MAX_INSTR - 1;
    if (note == KEYON) return;
    if ((note == REST) || (note == KEYOFF)) {
        cptr->gate = 0xfe;
        return;
    }
    if (!(g->instr[ins].gatetimer & 0x40)) {
        cptr->gate = 0xfe;                 /* Keyoff */
        if (!(g->instr[ins].gatetimer & 0x80)) {
            g->sidreg[0x5 + (c % 3u) * 7] = ADPARAM >> 8;   /* Hardrestart */
            g->sidreg[0x6 + (c % 3u) * 7] = ADPARAM & 0xff;
        }
    }
    cptr->instr = (uint8_t)ins;
    cptr->newnote = (uint8_t)note;
    if (g->songinit == PLAY_STOPPED) {
        cptr->tick = (g->instr[ins].gatetimer & 0x3f) + 1;
        cptr->gatetimer = g->instr[ins].gatetimer & 0x3f;
    }
}
