/* firmware/tracker.c - a GoatTracker 2 editor on the FM-1 (included by main.c): the song in RAM
 * (player/gt2edit.c, in the PSID player's memory), GoatTracker's playroutine playing it while
 * it is edited, and five pages on the LCD, as GoatTracker's: the patterns, the order lists, an
 * instrument, the four tables, the song (its texts, and the flash slots it is saved in:
 * firmware/store.c).
 *
 * SEQ in the player opens it on the GoatTracker song playing (a .sid: a new song); HOME leaves
 * (twice with changes not saved). On every page: SEQ the next page, REC edit on / off (off, the
 * keys only play), PLAY play / stop, OCT- / OCT+ the octave, SAVE save, KNOB1 the instrument,
 * LFO mute the channel, FX the effects, EDIT the engine. Turning: PRESETS moves up / down,
 * SELECT left / right (on the emulator's keyboard the arrows), ALGORITHM changes the value at
 * the cursor. With REC on, where the cursor
 * is on a hex digit, the 16 keys from C4 to D#5 type 0..F (on the emulator's keyboard: A W S E D
 * F T G Y H U J K O L P).
 *   PATTERNS: the three channels' patterns; a key writes its note with the instrument and moves
 *     on by the step (KNOB4); GLO clears, ARP key off, SCL / ENV insert / delete a row; KNOB2
 *     the channel's pattern, KNOB3 its length; PLAY plays the patterns over and over.
 *   ORDERS: the subtune's order lists (KNOB2 the subtune); two hex digits an entry (00-CF a
 *     pattern, D0-DF repeat, E0-FE transpose); SCL / ENV insert / delete, GLO the restart here;
 *     PLAY plays from the cursor's position.
 *   INSTRUMENT: its nine bytes and its name (PRESETS the letter, ALGORITHM changes it).
 *   TABLES: wave, pulse, filter, speed side by side; SCL / ENV insert / delete a row (pointers
 *     to the rows after it follow, as GoatTracker does), GLO clears it.
 *   SONG: name, author, released; the 16 slots: SAVE saves into the slot at the cursor, GLO
 *     loads it, ARP deletes it (each twice where it would lose something); SCL a new song.
 * MIDI notes over USB play and write as the keys do (trk_midi).
 * SPDX-License-Identifier: GPL-3.0-only */

#define TK_POOL (psid_mem + GT2_SIZE_MAX)
#define TK_POOL_SIZE (sizeof psid_mem - GT2_SIZE_MAX)
#define TK_SENT 0x7fffffffu
#define TK_LH 12                           /* a line */
#define TK_Y0 30                           /* the first content line */
#define TK_ROWS 16

enum { TK_PATT, TK_ORD, TK_INS, TK_TBL, TK_SONG, TK_PAGES };
static const char *const TK_PAGE[TK_PAGES] = {"PATTERNS", "ORDERS", "INSTRUMENT", "TABLES", "SONG"};

static uint8_t tk_page, tk_rec, tk_dirty, tk_oct = 4, tk_ins = 1, tk_step = 1, tk_song;
static uint8_t tk_row, tk_col;             /* patterns: the row, the column (channel * 6 + field) */
static uint8_t tk_view[3];                 /* the pattern each channel shows */
static uint8_t tk_opos, tk_ochn, tk_ohex;  /* orders: position, channel, a first digit (0x10 | d) */
static uint8_t tk_ifield, tk_ichar, tk_ihex;   /* instrument: field, the name's letter */
static uint8_t tk_trow, tk_tcol;           /* tables: row, column (table * 4 + digit) */
static uint8_t tk_sline, tk_schar;         /* song: line (3 texts, then the slots), letter */
static int8_t tk_slot = -1;                /* the slot the song is from / was saved in */
static uint32_t tk_held;                   /* the note keys down */
static uint8_t tk_keych[27];               /* the channel each one plays on */
static char tk_msg[40];
static uint32_t tk_msg_until, tk_ask_id, tk_ask_until;

static void tk_say(const char *s)
{
    cat(tk_msg, s);
    tk_msg_until = ms + 2500u;
}

/* a second press of button b within 3 s (the first says what it would do) */
static int tk_ask(uint32_t b, const char *what)
{
    if (tk_ask_id == b + 1u && (int32_t)(tk_ask_until - ms) > 0) {
        tk_ask_id = 0;
        return 1;
    }
    tk_ask_id = b + 1u;
    tk_ask_until = ms + 3000u;
    tk_say(what);
    return 0;
}

static char *hex2(char *p, uint32_t v)
{
    static const char H[] = "0123456789ABCDEF";
    p[0] = H[v >> 4 & 15u];
    p[1] = H[v & 15u];
    p[2] = 0;
    return p + 2;
}

static char *hex1(char *p, uint32_t v) { p[0] = "0123456789ABCDEF"[v & 15u]; p[1] = 0; return p + 1; }

static void tk_note(char *p, uint8_t n)
{
    static const char *const NN[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    if (n >= GT2_FIRSTNOTE && n <= GT2_LASTNOTE) {
        uint32_t k = n - GT2_FIRSTNOTE;
        p = cat(p, NN[k % 12u]);
        p[0] = (char)('0' + k / 12u);
        p[1] = 0;
    } else {
        cat(p, n == GT2_KEYOFF ? "---" : n == GT2_KEYON ? "+++" : "...");
    }
}

/* the song's engine fresh (a chip or engine change, the song going on) */
static void tk_chip(void)
{
    uint32_t i;
    running = 0;
    fm1_delay_ms(4);
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
    gen_cyc = PSID_PAL_FRAME;
    played_ms = 0;
    running = 1;
}

/* each channel's view on the pattern at order list position pos (past transposes, repeats) */
static void tk_view_pos(uint32_t pos)
{
    uint32_t c, i;
    for (c = 0; c < 3; c++) {
        for (i = pos; i < G->songlen[tk_song][c] && gt2_ord(G, tk_song, c, i) >= GT2_REPEAT; i++) ;
        if (i < G->songlen[tk_song][c])
            tk_view[c] = gt2_ord(G, tk_song, c, i);
    }
}

static void tk_open(void)
{
    trk = 1;
    tk_dirty = 0;
    tk_song = 0;
    tk_row = tk_opos = 0;
    tk_view_pos(0);
    tk_chip();
}

/* SEQ in the player: the GoatTracker song playing into the editor, else a new song */
static void trk_enter(int32_t slot)
{
    running = 0;
    fm1_delay_ms(4);
    if (gt) {                              /* (a saved song's file is at the memory's end: the
                                             * pool stays clear of it while the song moves in) */
        if (gt2_edit(G, TK_POOL, TK_POOL_SIZE - song_tail)) {
            tk_say("too big for the editor");
            running = 1;
            return;
        }
        G->pool_size = TK_POOL_SIZE;
        song_tail = 0;
        tk_slot = (int8_t)slot;
    } else {
        gt2_new(G, TK_POOL, TK_POOL_SIZE, on_write, 0);
        gt = 1;
        tk_slot = -1;
    }
    gt2_reset(G);
    tk_open();
}

static void tk_leave(void)
{
    trk = 0;
    start(play_idx, 0);
}

/* --------------------------------------------------------------------------- keys --- */
/* the cursor's two knobs: PRESETS up / down (clockwise up: the emulator's arrow up), SELECT
 * left / right (clockwise right, as SELECT moves the step sequencer's cursor) */
static int32_t tk_updown(void) { return -fm1_enc_take(6); }
static int32_t tk_across(void) { return fm1_enc_take(0); }

static uint32_t tk_chn(void) { return tk_col / 6u; }
static uint32_t tk_field(void) { return tk_col % 6u; }
static int tk_playing(void) { return gt2_playing(G); }

/* the row a channel plays (its pattern pointer is one row on) */
static uint32_t tk_play_row(uint32_t c)
{
    uint32_t pp = G->chn[c].pattptr, len = G->pattlen[G->chn[c].pattnum];
    if (pp == TK_SENT)
        return len ? len - 1u : 0u;
    return pp >= 4u ? pp / 4u - 1u : 0u;
}

static uint32_t tk_gt2note(uint32_t key)   /* note key 0..26 (F3..G5) -> GoatTracker's note */
{
    int32_t n = 53 + (int32_t)key - 12 + 12 * ((int32_t)tk_oct - 4);
    if (n < 0) n = 0;
    if (n > GT2_LASTNOTE - GT2_FIRSTNOTE) n = GT2_LASTNOTE - GT2_FIRSTNOTE;
    return GT2_FIRSTNOTE + (uint32_t)n;
}

static void tk_down(int32_t d)             /* the cursor on by d rows (in the pattern) */
{
    uint32_t len = G->pattlen[tk_view[tk_chn()]];
    tk_row = (uint8_t)(((int32_t)tk_row + d % (int32_t)len + (int32_t)len) % (int32_t)len);
}

static uint8_t *tk_cell(void)              /* the cursor's row, to write (0: no room) */
{
    uint8_t *b = gt2_patt_w(G, tk_view[tk_chn()]);
    if (!b) {
        tk_say("the song's memory is full");
        return 0;
    }
    tk_dirty = 1;
    return b + tk_row * 4u;
}

static void tk_hex_patt(uint32_t d)
{
    uint8_t *r = tk_cell();
    uint32_t f = tk_field();
    if (!r)
        return;
    switch (f) {
    case 1: r[1] = (uint8_t)((d << 4 | (r[1] & 15u)) > 0x3f ? 0x3f : d << 4 | (r[1] & 15u)); tk_col++; return;
    case 2: r[1] = (uint8_t)(((r[1] & 0xf0u) | d) > 0x3f ? 0x3f : (r[1] & 0xf0u) | d); tk_col--; break;
    case 3: r[2] = (uint8_t)d; tk_col++; return;
    case 4: r[3] = (uint8_t)(d << 4 | (r[3] & 15u)); tk_col++; return;
    case 5: r[3] = (uint8_t)((r[3] & 0xf0u) | d); tk_col -= 2; break;
    }
    tk_down(tk_step);
}

static void tk_turn_patt(int32_t d)        /* ALGORITHM: the value at the cursor */
{
    uint8_t *r = tk_cell();
    int32_t v;
    if (!r)
        return;
    switch (tk_field()) {
    case 0:
        if (r[0] >= GT2_FIRSTNOTE && r[0] <= GT2_LASTNOTE) {
            v = r[0] + d;
            r[0] = (uint8_t)(v < GT2_FIRSTNOTE ? GT2_FIRSTNOTE : v > GT2_LASTNOTE ? GT2_LASTNOTE : v);
        } else {
            r[0] = (uint8_t)(GT2_FIRSTNOTE + 12u * tk_oct);
        }
        break;
    case 1: case 2: v = r[1] + d; r[1] = (uint8_t)(v < 0 ? 0 : v > 0x3f ? 0x3f : v); break;
    case 3: r[2] = (uint8_t)((r[2] + d) & 15); break;
    case 4: r[3] = (uint8_t)(r[3] + 16 * d); break;
    case 5: r[3] = (uint8_t)(r[3] + d); break;
    }
}

static void tk_keys_patt(uint32_t e)
{
    int32_t d;
    uint32_t c = tk_chn();
    if ((d = tk_updown()) && !tk_playing())
        tk_down(d);
    if ((d = tk_across()))
        tk_col = (uint8_t)((tk_col + 18 * 8 + d) % 18);
    if ((d = fm1_enc_take(1)) && tk_rec)
        tk_turn_patt(d);
    if ((d = fm1_enc_take(3)) && !tk_playing()) {
        tk_view[c] = (uint8_t)((tk_view[c] + GT2_MAX_PATT * 8 + d) % GT2_MAX_PATT);
        tk_down(0);
    }
    if ((d = fm1_enc_take(4)) && tk_rec) {
        int32_t n = G->pattlen[tk_view[c]] + d;
        if (gt2_patt_len(G, tk_view[c], (uint32_t)(n < 1 ? 1 : n > GT2_MAX_ROWS ? GT2_MAX_ROWS : n)))
            tk_say("the song's memory is full");
        tk_dirty = 1;
        tk_down(0);
    }
    if ((d = fm1_enc_take(5))) {
        d += tk_step;
        tk_step = (uint8_t)(d < 0 ? 0 : d > 16 ? 16 : d);
    }
    if (e & (1u << 12)) {                  /* PLAY: the patterns over and over, from the top */
        if (tk_playing())
            gt2_stop(G);
        else
            gt2_start(G, GT2_PLAY_PATTERN, tk_song, 0, tk_view, 0);
    }
    if (!tk_rec)
        return;
    if (e & (1u << 7)) {                   /* GLO: clear */
        uint8_t *r = tk_cell();
        if (r) {
            if (tk_field() == 0) { r[0] = GT2_REST; r[1] = 0; }
            else if (tk_field() < 3) r[1] = 0;
            else r[2] = r[3] = 0;
            tk_down(tk_step);
        }
    }
    if (e & (1u << 10)) {                  /* ARP: key off */
        uint8_t *r = tk_cell();
        if (r) {
            r[0] = GT2_KEYOFF;
            tk_down(tk_step);
        }
    }
    if (e & (1u << 3)) {                   /* SCL / ENV: insert / delete a row */
        tk_dirty = 1;
        if (gt2_patt_ins(G, tk_view[c], tk_row)) tk_say("no room");
    }
    if (e & (1u << 4)) {
        tk_dirty = 1;
        gt2_patt_del(G, tk_view[c], tk_row);
        tk_down(0);
    }
}

static void tk_keys_ord(uint32_t e)
{
    int32_t d;
    uint32_t c, len, maxlen = 0;
    for (c = 0; c < 3; c++)
        if (G->songlen[tk_song][c] > maxlen)
            maxlen = G->songlen[tk_song][c];
    c = tk_ochn;
    len = G->songlen[tk_song][c];
    if ((d = tk_updown())) {
        d += tk_opos;
        tk_opos = (uint8_t)(d < 0 ? 0 : d > (int32_t)maxlen ? (int32_t)maxlen : d);
        tk_ohex = 0;
        tk_view_pos(tk_opos);
    }
    if ((d = tk_across())) {
        tk_ochn = (uint8_t)((tk_ochn + 3 * 8 + d) % 3);
        tk_ohex = 0;
    }
    if ((d = fm1_enc_take(3))) {           /* KNOB2: the subtune */
        tk_song = (uint8_t)((tk_song + GT2_MAX_SONGS * 8 + d) % GT2_MAX_SONGS);
        tk_opos = 0;
        tk_view_pos(0);
    }
    if (e & (1u << 12)) {                  /* PLAY: from the position */
        uint8_t pos[3];
        pos[0] = pos[1] = pos[2] = tk_opos;
        if (tk_playing())
            gt2_stop(G);
        else
            gt2_start(G, GT2_PLAY_POS, tk_song, pos, 0, 0);
    }
    if (!tk_rec)
        return;
    if ((d = fm1_enc_take(1))) {
        tk_dirty = 1;
        if (tk_opos < len) {
            uint32_t v = gt2_ord(G, tk_song, c, tk_opos);
            v = (v + 255u * 8u + (uint32_t)d) % 255u;   /* (0..FE: FF ends a list) */
            gt2_ord_set(G, tk_song, c, tk_opos, (uint8_t)v);
        } else if (tk_opos == len && len) {
            int32_t r = gt2_ord(G, tk_song, c, len + 1u) + d;
            gt2_ord_restart(G, tk_song, c, (uint8_t)(r < 0 ? 0 : r >= (int32_t)len ? (int32_t)len - 1 : r));
        }
        tk_view_pos(tk_opos);
    }
    if (e & (1u << 3)) {                   /* SCL: insert (a copy of the entry here) */
        uint8_t v = tk_opos < len ? gt2_ord(G, tk_song, c, tk_opos) : tk_opos ? gt2_ord(G, tk_song, c, tk_opos - 1u) : 0;
        tk_dirty = 1;
        if (gt2_ord_ins(G, tk_song, c, tk_opos > len ? len : tk_opos, v == GT2_LOOPSONG ? 0 : v))
            tk_say("no room");
        tk_view_pos(tk_opos);
    }
    if (e & (1u << 4)) {                   /* ENV: delete */
        tk_dirty = 1;
        if (gt2_ord_del(G, tk_song, c, tk_opos))
            tk_say("a list keeps one entry");
        tk_view_pos(tk_opos);
    }
    if ((e & (1u << 7)) && tk_opos < len) {   /* GLO: the restart here */
        tk_dirty = 1;
        gt2_ord_restart(G, tk_song, c, tk_opos);
    }
}

static void tk_hex_ord(uint32_t d)
{
    uint32_t c = tk_ochn, len = G->songlen[tk_song][c], v;
    if (!(tk_ohex & 0x10u)) {
        tk_ohex = (uint8_t)(0x10u | d);
        return;
    }
    v = (tk_ohex & 15u) << 4 | d;
    tk_ohex = 0;
    tk_dirty = 1;
    if (tk_opos < len && v != GT2_LOOPSONG)
        gt2_ord_set(G, tk_song, c, tk_opos, (uint8_t)v);
    else if (tk_opos == len && v < len)
        gt2_ord_restart(G, tk_song, c, (uint8_t)v);
    else if (tk_opos == len && v != GT2_LOOPSONG)
        gt2_ord_ins(G, tk_song, c, len, (uint8_t)v);   /* (typed on the end: a new entry) */
    if (tk_opos < G->songlen[tk_song][c])
        tk_opos++;
    tk_view_pos(tk_opos);
}

/* the instrument's fields: the nine bytes, then the name */
static uint8_t *tk_ibyte(uint32_t f)
{
    gt2_ins_t *n = &G->instr[tk_ins];
    switch (f) {
    case 0: return &n->ad;
    case 1: return &n->sr;
    case 2: case 3: case 4: case 5: return &n->ptr[f - 2u];
    case 6: return &n->vibdelay;
    case 7: return &n->gatetimer;
    default: return &n->firstwave;
    }
}

static const char TK_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.,!?'()/&+:#";

static char tk_char_turn(char ch, int32_t d)
{
    int32_t i, n = (int32_t)sizeof TK_CHARS - 1;
    for (i = 0; i < n && TK_CHARS[i] != ch; i++) ;
    if (i == n) i = 0;
    return TK_CHARS[((i + d) % n + n) % n];
}

static void tk_keys_ins(uint32_t e)
{
    int32_t d;
    if ((d = tk_updown())) {
        d += tk_ifield;
        tk_ifield = (uint8_t)(d < 0 ? 0 : d > 9 ? 9 : d);
        tk_ihex = 0;
    }
    if ((d = tk_across()) && tk_ifield == 9)
        tk_ichar = (uint8_t)((tk_ichar + 16 * 8 + d) % 16);
    if (e & (1u << 12)) {
        if (tk_playing()) gt2_stop(G);
        else { uint8_t pos[3] = {tk_opos, tk_opos, tk_opos}; gt2_start(G, GT2_PLAY_POS, tk_song, pos, 0, 0); }
    }
    if (!tk_rec)
        return;
    if ((d = fm1_enc_take(1))) {
        tk_dirty = 1;
        if (tk_ifield == 9) {
            char *ch = &G->instr[tk_ins].name[tk_ichar];
            *ch = tk_char_turn(*ch ? *ch : ' ', d);
        } else {
            uint8_t *b = tk_ibyte(tk_ifield);
            *b = (uint8_t)(*b + d);
        }
    }
    if (e & (1u << 7)) {                   /* GLO: clear */
        tk_dirty = 1;
        if (tk_ifield == 9) G->instr[tk_ins].name[tk_ichar] = ' ';
        else *tk_ibyte(tk_ifield) = 0;
    }
}

static void tk_hex_ins(uint32_t d)
{
    uint8_t *b;
    if (tk_ifield == 9)
        return;
    b = tk_ibyte(tk_ifield);
    tk_dirty = 1;
    if (!(tk_ihex & 0x10u)) {
        *b = (uint8_t)(d << 4 | (*b & 15u));
        tk_ihex = 0x10;
    } else {
        *b = (uint8_t)((*b & 0xf0u) | d);
        tk_ihex = 0;
        if (tk_ifield < 8) tk_ifield++;
    }
}

static void tk_keys_tbl(uint32_t e)
{
    int32_t d;
    uint32_t t = tk_tcol / 4u;
    if ((d = tk_updown())) {
        d += tk_trow;
        tk_trow = (uint8_t)(d < 0 ? 0 : d > GT2_TABLELEN - 1 ? GT2_TABLELEN - 1 : d);
    }
    if ((d = tk_across()))
        tk_tcol = (uint8_t)((tk_tcol + 16 * 8 + d) % 16);
    if (e & (1u << 12)) {
        if (tk_playing()) gt2_stop(G);
        else { uint8_t pos[3] = {tk_opos, tk_opos, tk_opos}; gt2_start(G, GT2_PLAY_POS, tk_song, pos, 0, 0); }
    }
    if (!tk_rec)
        return;
    if ((d = fm1_enc_take(1))) {
        uint8_t *b = (tk_tcol & 2u) ? &G->rtable[t][tk_trow] : &G->ltable[t][tk_trow];
        *b = (uint8_t)(*b + d * ((tk_tcol & 1u) ? 1 : 16));
        tk_dirty = 1;
    }
    if (e & (1u << 3)) { gt2_tbl_ins(G, (int)t, tk_trow); tk_dirty = 1; }
    if (e & (1u << 4)) { gt2_tbl_del(G, (int)t, tk_trow); tk_dirty = 1; }
    if (e & (1u << 7)) { G->ltable[t][tk_trow] = G->rtable[t][tk_trow] = 0; tk_dirty = 1; }
}

static void tk_hex_tbl(uint32_t d)
{
    uint32_t t = tk_tcol / 4u, n = tk_tcol & 3u;
    uint8_t *b = (n & 2u) ? &G->rtable[t][tk_trow] : &G->ltable[t][tk_trow];
    *b = (uint8_t)((n & 1u) ? (*b & 0xf0u) | d : d << 4 | (*b & 15u));
    tk_dirty = 1;
    if (n < 3u)
        tk_tcol++;
    else {
        tk_tcol -= 3;
        if (tk_trow < GT2_TABLELEN - 1) tk_trow++;
    }
}

static char *tk_text_of(uint32_t line)    /* the song page's three texts */
{
    return line == 0 ? G->songname : line == 1 ? G->authorname : G->copyrightname;
}

static void tk_save(uint32_t slot)
{
    int rc;
    tk_say("saving ...");
    draw();
    lcd_frame();
    gt2_stop(G);
    running = 0;
    fm1_delay_ms(4);
    rc = st_save(slot, G);
    tk_chip();
    if (!rc) {
        char m[32], *p = cat(m, "saved in slot ");
        fmt_u(p, slot, 1);
        tk_say(m);
        tk_slot = (int8_t)slot;
        tk_dirty = 0;
        list_store();
    } else {
        tk_say(rc == -1 ? "no flash store in this build" : rc == -2 ? "the flash store is full" : "flash error");
    }
}

static void tk_load(uint32_t slot)
{
    uint32_t len = 0;
    const uint8_t *s;
    running = 0;
    fm1_delay_ms(4);
    s = song_ram(slot, &len);
    if (!s) {
        tk_say("the slot is empty");
        tk_chip();
        return;
    }
    if (gt2_load(G, s, len, on_write, 0) || gt2_edit(G, TK_POOL, TK_POOL_SIZE - song_tail)) {
        gt2_new(G, TK_POOL, TK_POOL_SIZE, on_write, 0);
        tk_say("could not load it: a new song");
        tk_slot = -1;
    } else {
        tk_say("loaded");
        tk_slot = (int8_t)slot;
        G->pool_size = TK_POOL_SIZE;
    }
    song_tail = 0;
    gt2_reset(G);
    tk_open();
}

static void tk_quick_save(void)            /* SAVE: into the song's slot, else the first free */
{
    uint32_t i;
    if (tk_slot >= 0) {
        tk_save((uint32_t)tk_slot);
        return;
    }
    for (i = 0; i < ST_SLOTS && st_rec[i].off; i++) ;
    if (i == ST_SLOTS) {
        tk_say("all slots taken: SONG page");
        return;
    }
    tk_save(i);
}

static void tk_keys_song(uint32_t e)
{
    int32_t d;
    if ((d = tk_updown())) {
        d += tk_sline;
        tk_sline = (uint8_t)(d < 0 ? 0 : d > 2 + (int32_t)ST_SLOTS ? 2 + (int32_t)ST_SLOTS : d);
    }
    if ((d = tk_across()) && tk_sline < 3)
        tk_schar = (uint8_t)((tk_schar + 32 * 8 + d) % 32);
    if ((d = fm1_enc_take(1)) && tk_rec && tk_sline < 3) {
        char *s = tk_text_of(tk_sline);
        uint32_t i;
        for (i = 0; i < tk_schar; i++)     /* (letters before it: spaces, not the end) */
            if (!s[i]) s[i] = ' ';
        s[tk_schar] = tk_char_turn(s[tk_schar] ? s[tk_schar] : ' ', d);
        tk_dirty = 1;
    }
    if (e & (1u << 12)) {
        if (tk_playing()) gt2_stop(G);
        else { uint8_t pos[3] = {0, 0, 0}; gt2_start(G, GT2_PLAY_POS, tk_song, pos, 0, 0); }
    }
    if ((e & (1u << 3)) && tk_ask(3, "SCL again: a new song")) {   /* SCL: a new song */
        running = 0;
        fm1_delay_ms(4);
        gt2_new(G, TK_POOL, TK_POOL_SIZE, on_write, 0);
        tk_slot = -1;
        tk_open();
        tk_say("a new song");
    }
    if (tk_sline >= 3) {
        uint32_t slot = tk_sline - 3u;
        if ((e & (1u << 9)) && (!st_rec[slot].off || (int32_t)slot == tk_slot ||
                                tk_ask(9, "SAVE again: over that song")))
            tk_save(slot);
        if ((e & (1u << 7)) && (!tk_dirty || tk_ask(7, "GLO again: lose the changes")))
            tk_load(slot);
        if ((e & (1u << 10)) && st_rec[slot].off && tk_ask(10, "ARP again: delete the slot")) {
            if (st_delete(slot)) tk_say("flash error");
            else { tk_say("deleted"); list_store(); if ((int32_t)slot == tk_slot) tk_slot = -1; }
        }
    } else if ((e & (1u << 7)) && tk_rec) {
        tk_text_of(tk_sline)[tk_schar] = ' ';
        tk_dirty = 1;
    }
}

/* the keys: the main loop's button edges e; HOME, FX and EDIT are the main loop's */
static void trk_keys(uint32_t e)
{
    uint32_t cur = fm1_in.notes, held = tk_held, taps = fm1_input_note_edges() & ~cur & ~held, i;
    int32_t d;
    uint32_t page = tk_page;
    if (e & (1u << 11)) {                  /* SEQ: the next page */
        tk_page = (uint8_t)((tk_page + 1u) % TK_PAGES);
        tk_ohex = tk_ihex = 0;
    }
    if (e & (1u << 13))
        tk_rec ^= 1;
    if (e & (1u << 1) && tk_oct < 7) tk_oct++;
    if (e & (1u << 0) && tk_oct > 0) tk_oct--;
    if (e & (1u << 5))                     /* LFO: mute the channel */
        G->chn[tk_chn()].mute ^= 1;
    if ((d = fm1_enc_take(2)))             /* KNOB1: the instrument */
        tk_ins = (uint8_t)((tk_ins - 1 + (GT2_MAX_INSTR - 1) * 8 + d) % (GT2_MAX_INSTR - 1) + 1);
    if ((e & (1u << 9)) && page != TK_SONG)
        tk_quick_save();
    switch (page) {
    case TK_PATT: tk_keys_patt(e); break;
    case TK_ORD: tk_keys_ord(e); break;
    case TK_INS: tk_keys_ins(e); break;
    case TK_TBL: tk_keys_tbl(e); break;
    default: tk_keys_song(e); break;
    }
    /* the note keys: hex digits where the cursor is on one (REC on), else notes */
    for (i = 0; i < 27; i++) {
        uint32_t bit = 1u << i, c = tk_chn();
        int hex = tk_rec && ((page == TK_PATT && tk_field()) || page == TK_ORD ||
                             (page == TK_INS && tk_ifield < 9) || page == TK_TBL);
        if ((cur & ~held & bit) || (taps & bit)) {
            if (hex) {
                if (i >= 7 && i < 23) {
                    uint32_t dg = i - 7u;
                    if (page == TK_PATT) tk_hex_patt(dg);
                    else if (page == TK_ORD) tk_hex_ord(dg);
                    else if (page == TK_INS) tk_hex_ins(dg);
                    else tk_hex_tbl(dg);
                }
            } else {
                uint32_t n = tk_gt2note(i);
                if (page == TK_PATT && tk_rec) {   /* the note and the instrument, then on */
                    uint8_t *r = tk_cell();
                    if (r) {
                        r[0] = (uint8_t)n;
                        r[1] = tk_ins;
                        tk_down(tk_step);
                    }
                }
                tk_keych[i] = (uint8_t)c;
                gt2_jam(G, n, tk_ins, c);
            }
        }
        if (((held & ~cur & bit) || (taps & bit)) && !hex)
            gt2_jam(G, GT2_REST, tk_ins, tk_keych[i]);
    }
    tk_held = cur;
}

/* a MIDI note (USB), as a note key at its own pitch: played on the cursor's channel, and on
 * PATTERNS with REC on written as a key writes it */
static uint8_t tk_midich[128];

static void trk_midi(uint32_t n, int on)
{
    int32_t g = (int32_t)n - 12;
    uint32_t note;
    if (!on) {
        gt2_jam(G, GT2_REST, tk_ins, tk_midich[n]);
        return;
    }
    if (g < 0) g = 0;
    if (g > GT2_LASTNOTE - GT2_FIRSTNOTE) g = GT2_LASTNOTE - GT2_FIRSTNOTE;
    note = GT2_FIRSTNOTE + (uint32_t)g;
    if (tk_page == TK_PATT && tk_rec && tk_field() == 0) {
        uint8_t *r = tk_cell();
        if (r) {
            r[0] = (uint8_t)note;
            r[1] = tk_ins;
            tk_down(tk_step);
        }
    }
    tk_midich[n] = (uint8_t)tk_chn();
    gt2_jam(G, note, tk_ins, tk_chn());
}

/* once a main loop round: the views follow what plays */
static void trk_follow(void)
{
    uint32_t c;
    if (!tk_playing())
        return;
    for (c = 0; c < 3; c++)
        tk_view[c] = G->chn[c].pattnum;
    tk_row = (uint8_t)tk_play_row(tk_chn());
    if (G->songinit == 0 && G->psnum == tk_song && G->chn[tk_ochn].songptr)
        tk_opos = (uint8_t)(G->chn[tk_ochn].songptr - 1u);
}

/* --------------------------------------------------------------------------- draw --- */
static void tk_box(int x, int y, int w) { fill(x, y, w, TK_LH, tk_rec ? C_LRED : C_LBLUE); }

static void tk_draw_patt(void)
{
    char s[8];
    int k;
    uint32_t c;
    for (c = 0; c < 3; c++) {              /* the patterns over the channels */
        int x = 24 + (int)c * 72;
        char t[12], *p = cat(t, "P");
        p = hex2(p, tk_view[c]);
        p = cat(p, ":");
        hex2(p, G->pattlen[tk_view[c]]);
        text(x, 16, t, G->chn[c].mute ? C_DGREY : C_CYAN);
        if (G->chn[c].mute)
            text(x + 56, 16, "M", C_LRED);
    }
    for (k = 0; k < TK_ROWS; k++) {
        int32_t row = (int32_t)tk_row - 7 + k;
        int y = TK_Y0 + k * TK_LH;
        uint32_t lenc = G->pattlen[tk_view[tk_chn()]];
        if (row == tk_row)
            fill(0, y, 240, TK_LH, C_DARK2);
        if (row < 0)
            continue;
        if ((uint32_t)row < lenc) {
            hex2(s, (uint32_t)row);
            text(0, y, s, row % 4 ? C_GREY : C_YELLOW);
        }
        for (c = 0; c < 3; c++) {
            int x = 24 + (int)c * 72, mute = G->chn[c].mute;
            uint32_t p = tk_view[c];
            uint8_t n, in, cm, dt;
            if ((uint32_t)row >= G->pattlen[p])
                continue;
            n = gt2_pat(G, p, (uint32_t)row * 4u);
            in = gt2_pat(G, p, (uint32_t)row * 4u + 1u);
            cm = gt2_pat(G, p, (uint32_t)row * 4u + 2u);
            dt = gt2_pat(G, p, (uint32_t)row * 4u + 3u);
            if (row == tk_row && c == tk_chn()) {
                static const uint8_t FX[6] = {0, 24, 32, 40, 48, 56}, FW[6] = {24, 8, 8, 8, 8, 8};
                tk_box(x + FX[tk_field()], y, FW[tk_field()]);
            }
            tk_note(s, n);
            text(x, y, s, mute ? C_DGREY : n >= GT2_FIRSTNOTE && n <= GT2_LASTNOTE ? C_WHITE : C_GREY);
            if (in) hex2(s, in); else cat(s, "..");
            text(x + 24, y, s, mute ? C_DGREY : in ? C_CYAN : C_DGREY);
            if (cm || dt) {
                hex1(s, cm);
                text(x + 40, y, s, mute ? C_DGREY : C_YELLOW);
                hex2(s, dt);
                text(x + 48, y, s, mute ? C_DGREY : C_ORANGE);
            } else {
                text(x + 40, y, "...", C_DGREY);
            }
        }
    }
}

static void tk_draw_ord(void)
{
    char s[12], *p;
    int k;
    uint32_t c;
    p = cat(s, "SUBTUNE ");
    hex2(p, tk_song);
    text(0, 16, s, C_CYAN);
    for (c = 0; c < 3; c++) {
        p = cat(s, "CH");
        fmt_u(p, c + 1u, 1);
        text(112 + (int)c * 40, 16, s, c == tk_ochn ? C_WHITE : C_GREY);
    }
    for (k = 0; k < TK_ROWS; k++) {
        int32_t pos = (int32_t)tk_opos - 7 + k;
        int y = TK_Y0 + k * TK_LH;
        if (pos == tk_opos)
            fill(0, y, 240, TK_LH, C_DARK2);
        if (pos < 0)
            continue;
        hex2(s, (uint32_t)pos);
        text(80, y, s, C_GREY);
        for (c = 0; c < 3; c++) {
            uint32_t len = G->songlen[tk_song][c];
            int x = 112 + (int)c * 40;
            uint16_t col;
            if ((uint32_t)pos > len)
                continue;
            if (pos == tk_opos && c == tk_ochn)
                tk_box(x, y, (uint32_t)pos == len ? 40 : 16);
            if ((uint32_t)pos == len) {
                p = cat(s, "R");
                hex2(p, gt2_ord(G, tk_song, c, len + 1u));
                text(x, y, s, C_GREY);
                continue;
            }
            {
                uint8_t v = gt2_ord(G, tk_song, c, (uint32_t)pos);
                if (pos == tk_opos && c == tk_ochn && (tk_ohex & 0x10u)) {
                    hex1(s, tk_ohex);
                    cat(s + 1, "_");
                } else {
                    hex2(s, v);
                }
                col = v < GT2_REPEAT ? C_WHITE : v < GT2_TRANSDOWN ? C_CYAN : C_YELLOW;
                if (tk_playing() && G->psnum == tk_song && G->chn[c].songptr == (uint32_t)pos + 1u)
                    col = C_LGREEN;
                text(x, y, s, col);
            }
        }
    }
    text(0, TK_Y0, "00-CF", C_GREY);
    text(0, TK_Y0 + 12, "patt", C_DGREY);
    text(0, TK_Y0 + 30, "D0-DF", C_CYAN);
    text(0, TK_Y0 + 42, "rept", C_DGREY);
    text(0, TK_Y0 + 60, "E0-FE", C_YELLOW);
    text(0, TK_Y0 + 72, "trns", C_DGREY);
    text(0, TK_Y0 + 90, "R xx", C_GREY);
    text(0, TK_Y0 + 102, "rstrt", C_DGREY);
}

static void tk_draw_ins(void)
{
    static const char *const LABEL[10] = {"Attack/Decay", "Sustain/Rel.", "Wavetable", "Pulsetable",
                                          "Filtertable", "Vibrato par.", "Vibrato del.", "HR/Gatetimer",
                                          "1st wave", "Name"};
    const gt2_ins_t *n = &G->instr[tk_ins];
    char s[20];
    uint32_t f;
    hex2(cat(s, "INSTRUMENT "), tk_ins);
    text(0, 16, s, C_CYAN);
    for (f = 0; f < 10; f++) {
        int y = TK_Y0 + 2 + (int)f * 16;
        text(0, y, LABEL[f], f == tk_ifield ? C_WHITE : C_GREY);
        if (f < 9) {
            if (f == tk_ifield)
                tk_box(120 + ((tk_ihex & 0x10u) ? 8 : 0), y, (tk_ihex & 0x10u) ? 8 : 16);
            hex2(s, *tk_ibyte(f));
            text(120, y, s, C_WHITE);
        } else {
            uint32_t i;
            if (f == tk_ifield)
                tk_box(104 + 8 * tk_ichar, y, 8);
            for (i = 0; i < 16; i++)
                s[i] = n->name[i] >= 32 && n->name[i] < 127 ? n->name[i] : ' ';
            s[16] = 0;
            text(104, y, s, C_YELLOW);
        }
    }
    text(152, TK_Y0 + 2 + 2 * 16, "-> TABLES", C_DGREY);
}

static void tk_draw_tbl(void)
{
    static const char *const NAME[4] = {"WAVE", "PULSE", "FILT", "SPEED"};
    char s[8];
    int k;
    uint32_t t, c;
    for (t = 0; t < 4; t++)
        text(24 + (int)t * 54, 16, NAME[t], t == tk_tcol / 4u ? C_WHITE : C_CYAN);
    for (k = 0; k < TK_ROWS; k++) {
        int32_t row = (int32_t)tk_trow - 7 + k;
        int y = TK_Y0 + k * TK_LH;
        if (row == tk_trow)
            fill(0, y, 240, TK_LH, C_DARK2);
        if (row < 0 || row >= GT2_TABLELEN)
            continue;
        hex2(s, (uint32_t)row + 1u);
        text(0, y, s, C_GREY);
        for (t = 0; t < 4; t++) {
            int x = 24 + (int)t * 54, hot = 0;
            uint8_t l = G->ltable[t][row], r = G->rtable[t][row];
            for (c = 0; c < 3 && t < 2; c++)   /* (a playing channel's wave / pulse row) */
                if (G->chn[c].ptr[t] == (uint32_t)row + 1u && tk_playing())
                    hot = 1;
            if (t == 2 && G->filterptr == (uint32_t)row + 1u && tk_playing())
                hot = 1;
            if (row == tk_trow && t == tk_tcol / 4u) {
                uint32_t n = tk_tcol & 3u;
                tk_box(x + (int)(n < 2 ? n * 8 : 24 + (n - 2) * 8), y, 8);
            }
            hex2(s, l);
            text(x, y, s, hot ? C_LGREEN : l == 0xff ? C_ORANGE : (l | r) ? C_WHITE : C_DGREY);
            hex2(s, r);
            text(x + 24, y, s, hot ? C_LGREEN : (l | r) ? C_YELLOW : C_DGREY);
        }
    }
}

static void tk_draw_song(void)
{
    static const char *const LABEL[3] = {"NAME", "BY", "YEAR"};
    char s[40], *p;
    uint32_t i, l;
    int y;
    for (l = 0; l < 3; l++) {
        const char *t = tk_text_of(l);
        uint32_t off = tk_schar > 20u && l == tk_sline ? tk_schar - 20u : 0u;
        y = 16 + (int)l * 14;
        text(0, y, LABEL[l], l == tk_sline ? C_WHITE : C_GREY);
        if (l == tk_sline)
            tk_box(40 + 8 * (int)(tk_schar - off), y, 8);
        for (i = 0; i < 24 && off + i < 32; i++)
            s[i] = t[off + i] >= 32 && t[off + i] < 127 ? t[off + i] : (t[off + i] ? '?' : ' ');
        s[i] = 0;
        text(40, y, s, C_YELLOW);
    }
    p = cat(s, "song ");
    p = fmt_u(p, gt2_save_to(G, 0, 0), 1);
    p = cat(p, " B, memory free ");
    fmt_u(p, gt2_free(G), 1);
    text(0, 60, s, C_GREY);
    if (!st_on()) {
        text(0, 80, "no flash store in this build", C_LRED);
        return;
    }
    p = cat(s, "FLASH SLOTS     free ");
    p = fmt_u(p, st_free() / 1024u, 1);
    cat(p, " KB");
    text(0, 76, s, C_CYAN);
    for (i = 0; i < 10; i++) {             /* ten slots about the cursor */
        int32_t first = tk_sline >= 3 ? (int32_t)tk_sline - 3 - 4 : 0, slot;
        if (first < 0) first = 0;
        if (first > (int32_t)ST_SLOTS - 10) first = (int32_t)ST_SLOTS - 10;
        slot = first + (int32_t)i;
        y = 90 + (int)i * 13;
        if ((uint32_t)slot + 3u == tk_sline)
            fill(0, y, 240, TK_LH, tk_rec ? C_LRED : C_DARK2);
        p = cat(s, slot == tk_slot ? ">" : " ");
        p = hex1(p, (uint32_t)slot);
        p = cat(p, " ");
        if (st_rec[slot].off) {
            p = cat(p, st_rec[slot].name);
            while (p < s + 12) *p++ = ' ';
            *p = 0;
            p = fmt_u(p, st_rec[slot].len, 5);
            cat(p, " B");
            text(0, y, s, C_WHITE);
        } else {
            cat(p, "(empty)");
            text(0, y, s, C_DGREY);
        }
    }
}

static void trk_draw(void)
{
    char s[40], *p;
    fill(0, 0, 240, 240, C_BLUE);
    fill(0, 0, 240, 14, C_LBLUE);
    text(2, 1, TK_PAGE[tk_page], C_WHITE);
    p = cat(s, "I");
    p = hex2(p, tk_ins);
    p = cat(p, " O");
    p = fmt_u(p, tk_oct, 1);
    if (tk_page == TK_PATT) {
        p = cat(p, " S");
        fmt_u(p, tk_step, 1);
    }
    text(96, 1, s, C_YELLOW);
    if (tk_rec) {
        fill(178, 1, 26, 12, C_LRED);
        text(179, 1, "REC", C_WHITE);
    }
    text(208, 1, tk_playing() ? "PLY" : "   ", C_LGREEN);
    if (tk_dirty)
        text(232, 1, "*", C_ORANGE);
    switch (tk_page) {
    case TK_PATT: tk_draw_patt(); break;
    case TK_ORD: tk_draw_ord(); break;
    case TK_INS: tk_draw_ins(); break;
    case TK_TBL: tk_draw_tbl(); break;
    default: tk_draw_song(); break;
    }
    fill(0, 226, 240, 14, C_DARK);
    if ((int32_t)(tk_msg_until - ms) > 0) {
        text(2, 227, tk_msg, C_YELLOW);
    } else {
        static const char *const HELP[TK_PAGES][2] = {
            {"REC edit PLAY loop SEQ page", "GLO clr ARP off SCL/ENV row"},
            {"REC edit PLAY from SEQ page", "SCL/ENV entry GLO restart"},
            {"REC edit KNOB1 instr SEQ pg", "C4..D#5 = hex 0..F"},
            {"REC edit SEQ page", "SCL/ENV row GLO clear"},
            {"SAVE save GLO load ARP del", "SCL new song HOME leave"}};
        text(2, 227, HELP[tk_page][tk_rec], C_GREY);
    }
}
