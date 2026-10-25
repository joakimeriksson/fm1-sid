/* player/gt2.h - GoatTracker 2 songs (.sng, GTS3..GTS5) played by GoatTracker's own playroutine
 * (gplay.c of GoatTracker v2.76, Lasse Oorni / Cadaver), its SID registers once a PAL frame, and
 * edited (player/gt2edit.c).
 *
 * GoatTracker keeps a song in fixed arrays (208 patterns of 128 rows, 32 order lists of 256
 * bytes: about 135 KB). Here the song stays where it is, in flash, as the .sng file: loading
 * checks the file and makes an index of it (where each order list and pattern starts, how long
 * it is), copies the instruments and the four tables (the version conversions applied), and the
 * playroutine reads the patterns and order lists through that index. Whatever the file leaves
 * out reads as GoatTracker's cleared song does (an empty pattern is 64 rests), so the register
 * stream is GoatTracker's own, frame for frame (build/gt2log against sid-tracker's gt2dump,
 * tools/gt2check.sh). To edit, gt2_edit moves the order lists and patterns into a pool of RAM,
 * each as long as it is, and the index points there.
 *
 * As GoatTracker plays in its editor at the default settings: 1x speed (50 Hz), hard restart
 * ADSR $0F00, the pulse and realtime optimizations on; the 25 registers written every frame
 * from $18 down to $00, 14 cycles apart, as its sound code does. A song's own player settings
 * (made when it is packed into a .sid) are not in the .sng and so not here.
 *
 * Portable C: the host (player/gt2log.c) and the FM-1 firmware (firmware/) run the same code.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GT2_H
#define GT2_H
#include <stdint.h>
#include "psid.h"

#define GT2_FRAME PSID_PAL_FRAME
#define GT2_SIZE_MAX 8192u                 /* sizeof(gt2_t) is at most this (player/gt2.c checks) */

#define GT2_MAX_INSTR 64
#define GT2_MAX_PATT 208
#define GT2_TABLELEN 255
#define GT2_MAX_ROWS 128
#define GT2_MAX_SONGLEN 254
#define GT2_MAX_SONGS 32
#define GT2_DEFROWS 64                     /* a cleared pattern's length */
enum { GT2_WTBL, GT2_PTBL, GT2_FTBL, GT2_STBL };

/* a pattern row: note (or one of these), instrument, command, data */
#define GT2_FIRSTNOTE 0x60                 /* C-0 */
#define GT2_LASTNOTE 0xbc                  /* G#7 */
#define GT2_REST 0xbd
#define GT2_KEYOFF 0xbe
#define GT2_KEYON 0xbf
#define GT2_ENDPATT 0xff
/* an order list entry: a pattern, or one of these; the list ends in LOOPSONG and the restart */
#define GT2_REPEAT 0xd0                    /* D0..DF: the next pattern 1..16 times more */
#define GT2_TRANSDOWN 0xe0                 /* E0..EF: transpose -16..-1 */
#define GT2_TRANSUP 0xf0                   /* F0..FE: transpose 0..+14 */
#define GT2_LOOPSONG 0xff

enum { GT2_PLAY_BEGIN = 1, GT2_PLAY_POS = 2, GT2_PLAY_PATTERN = 3 };

typedef struct {
    uint8_t ad, sr, ptr[4], vibdelay, gatetimer, firstwave;
    char name[16];
} gt2_ins_t;

typedef struct {
    uint8_t trans, instr, note, lastnote, newnote;
    uint32_t pattptr;                      /* the byte in the pattern (0x7fffffff: at its end) */
    uint8_t pattnum, songptr, repeat;
    uint16_t freq;
    uint8_t gate, wave;
    uint16_t pulse;
    uint8_t ptr[2], pulsetime, wavetime, vibtime, vibdelay;
    uint8_t command, cmddata, newcommand, newcmddata, tick, tempo, advance, gatetimer, mute;
} gt2_chn_t;

typedef struct gt2 {
    /* the song: the index into the file (or the pool), the instruments and tables */
    const uint8_t *order[GT2_MAX_SONGS][3];   /* each order list, and its bytes there */
    uint16_t ordn[GT2_MAX_SONGS][3];
    uint8_t songlen[GT2_MAX_SONGS][3];
    const uint8_t *patt[GT2_MAX_PATT];        /* each pattern, and its bytes there (0: cleared) */
    uint16_t pattn[GT2_MAX_PATT];
    uint8_t pattlen[GT2_MAX_PATT];
    gt2_ins_t instr[GT2_MAX_INSTR];
    uint8_t ltable[4][GT2_TABLELEN], rtable[4][GT2_TABLELEN];
    char songname[32], authorname[32], copyrightname[32];
    uint32_t nsongs;
    psid_info_t info;
    psid_write_fn wr;
    void *user;
    /* editing (player/gt2edit.c): the pool the order lists and patterns are in (0: the file) */
    uint8_t *pool;
    uint32_t pool_size, pool_used;
    /* the playroutine */
    gt2_chn_t chn[3];
    uint8_t filterctrl, filtertype, filtercutoff, filtertime, filterptr, funktable[2], masterfader;
    uint8_t songinit, psnum, startpattpos;
    uint8_t espos[3], epnum[3];               /* GT2_PLAY_POS / _PATTERN: where each channel starts */
    uint8_t sidreg[25];
} gt2_t;

uint32_t gt2_size(void);
/* 0: a song this plays (the file must stay where it is while it plays) */
int gt2_load(gt2_t *g, const uint8_t *data, uint32_t len, psid_write_fn wr, void *user);
/* name, author, copyright, the subtunes as songs (start 1); model 6581 (a .sng has none) */
const psid_info_t *gt2_info(const gt2_t *g);
/* start subtune song (1-based; 0 or one with an empty order list: the first) from the
 * beginning, as the player does; returns 0 */
uint32_t gt2_init(gt2_t *g, uint32_t song);
/* the channels and the filter as at the start, the song stopped */
void gt2_reset(gt2_t *g);
/* one frame: the playroutine, then its 25 register writes; returns the cycles they took */
uint32_t gt2_play(gt2_t *g);
/* 0 once the song has stopped (itself: an illegal pattern, a jump into a jump, ...; or gt2_stop).
 * Stopped, the playroutine still runs: test notes sound */
int gt2_playing(const gt2_t *g);

/* the editor's ways to play (gplay.c initsongpos): from the beginning of subtune song (0-based),
 * from order list positions pos[3], or the patterns patt[3] over and over, from row; on the next
 * gt2_play */
void gt2_start(gt2_t *g, int mode, uint32_t song, const uint8_t pos[3], const uint8_t patt[3], uint32_t row);
void gt2_stop(gt2_t *g);
/* a test note (GoatTracker's jam: note FIRSTNOTE.., REST or KEYOFF releases) on channel c */
void gt2_jam(gt2_t *g, uint32_t note, uint32_t ins, uint32_t c);
/* byte i of pattern p and entry i of an order list as the playroutine sees them */
uint8_t gt2_pat(const gt2_t *g, uint32_t p, uint32_t i);
uint8_t gt2_ord(const gt2_t *g, uint32_t song, uint32_t c, uint32_t i);

/* ---- editing (player/gt2edit.c): changes that need room take it from the pool; -1: full ---- */
/* move the song's order lists and patterns into pool (RAM; the file is not needed after) */
int gt2_edit(gt2_t *g, uint8_t *pool, uint32_t size);
/* GoatTracker's cleared song (gsong.c clearsong) in pool */
int gt2_new(gt2_t *g, uint8_t *pool, uint32_t size, psid_write_fn wr, void *user);
/* pattern p's bytes to write (4 a row, its end row after its length); 0: full */
uint8_t *gt2_patt_w(gt2_t *g, uint32_t p);
int gt2_patt_len(gt2_t *g, uint32_t p, uint32_t rows);          /* 1..128 rows */
int gt2_patt_ins(gt2_t *g, uint32_t p, uint32_t row);           /* a rest in, the last row out */
int gt2_patt_del(gt2_t *g, uint32_t p, uint32_t row);           /* out, a rest at the end */
/* order list entries (song 0-based): set, insert, delete (the restart follows as GoatTracker's
 * does), and the restart position */
int gt2_ord_set(gt2_t *g, uint32_t song, uint32_t c, uint32_t pos, uint8_t v);
int gt2_ord_ins(gt2_t *g, uint32_t song, uint32_t c, uint32_t pos, uint8_t v);
int gt2_ord_del(gt2_t *g, uint32_t song, uint32_t c, uint32_t pos);
int gt2_ord_restart(gt2_t *g, uint32_t song, uint32_t c, uint8_t pos);
/* a table row in / out at pos (gtable.c inserttable / deletetable: the pointers into the table
 * from instruments, wave table commands, patterns and its own jumps follow) */
void gt2_tbl_ins(gt2_t *g, int t, int pos);
void gt2_tbl_del(gt2_t *g, int t, int pos);
uint32_t gt2_free(const gt2_t *g);                               /* pool bytes left */
/* the song as a GTS5 .sng (gsong.c savesong); its length, 0 when it does not fit in max */
uint32_t gt2_save(const gt2_t *g, uint8_t *out, uint32_t max);
/* the same a byte at a time to put (0: only its length) */
uint32_t gt2_save_to(const gt2_t *g, void (*put)(void *user, uint8_t b), void *user);

#endif
