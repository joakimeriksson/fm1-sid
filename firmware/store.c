/* firmware/store.c - GoatTracker songs saved in flash (included by main.c).
 *
 * Where: the free end of this firmware's own app slot, from the first 4 KB sector after the
 * image and its tune pack to the slot's end (0x92000): flash no other firmware keeps anything
 * in (Felucca's store, fm1-dock's table and modules are above 0x93000). Installing a package
 * again rewrites the slot, and the songs in it go (send them out first). In fm1-dock's boot
 * slot (DOCK_BOOT) there is no store: the slot's flash is fm1-dock's to give.
 *
 * The store is read with the flash's own read command (fl_read_ram, a page at a time), not
 * through the XIP window: the SFC's settings stay as the boot left them (its plain window is
 * not moved, so no firmware after this one can find it moved), and a saved song is copied into
 * RAM to play. Erase, program and read run from RAM with the flash unmapped (Felucca's
 * hal/fm1_flash.h, its sequence the AC79 SDK's), with interrupts off: the audio stops while a
 * song is written.
 *
 * A song is a record of whole sectors: a 32-byte head ("GT2SLOT1", length, CRC-32 of the
 * .sng, slot, save number), then the .sng. The head is programmed last, so a cut-off save
 * leaves no record; the slot's older record is erased only once the new one reads back right,
 * and of two records of a slot the later save counts.
 * SPDX-License-Identifier: GPL-3.0-only */

#if !defined(PLATFORM_DINGHY) && !defined(PLATFORM_DOCK)   /* (Dinghy's flash driver and st_read, or fm1-dock's store) */
static uint32_t st_lo, st_hi;              /* the store: flash [st_lo, st_hi); 0, 0: none */
/* where the flash driver erases and programs: the store, and (FELUCCA_OTA, an update: main.c)
 * Felucca's OTA area 0xE0000..0xE5000, the staged update loader and its record */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, st_lo, st_hi) || (FELUCCA_OTA && FL_IN(off, n, 0xE0000u, 0xE5000u)))
#include "fm1_flash.h"
#endif

#define ST_SECT 4096u
#define ST_HEAD 32u
#define ST_SLOTS 16u
#define ST_END 0x92000u                    /* the app slot (0x4120 + 0x8DFBC) to the sector */

typedef struct {
    char magic[8];
    uint32_t len, crc, slot, seq;
    char name[8];                          /* the song's first 8 characters, for the list */
} st_head_t;

static struct { uint32_t off, len, seq; char name[9]; } st_rec[ST_SLOTS];   /* off 0: none */
static uint32_t st_seq;

static uint32_t crc32_add(uint32_t c, uint8_t b)
{
    uint32_t k;
    c ^= b;
    for (k = 0; k < 8; k++)
        c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    return c;
}

#if !defined(PLATFORM_DINGHY) && !defined(PLATFORM_DOCK)
/* n bytes of flash at off into RAM, a page at a time (interrupts off for each) */
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f;
        int rc;
        f = irq_save();
        rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k; d += k; n -= k;
    }
    return 0;
}
#endif

#ifndef PLATFORM_DOCK
static uint32_t st_crc(uint32_t off, uint32_t n)   /* CRC-32 of n bytes of flash */
{
    static uint8_t buf[256];
    uint32_t c = 0xFFFFFFFFu, i;
    while (n) {
        uint32_t k = n > sizeof buf ? (uint32_t)sizeof buf : n;
        if (st_read(off, buf, k))
            return 0;
        for (i = 0; i < k; i++)
            c = crc32_add(c, buf[i]);
        off += k; n -= k;
    }
    return ~c;
}

static void st_scan(void)
{
    uint32_t off, i;
    for (i = 0; i < ST_SLOTS; i++)
        st_rec[i].off = 0;
    st_seq = 0;
    for (off = st_lo; off + ST_SECT <= st_hi;) {
        st_head_t hd, *h = &hd;
        if (!st_read(off, &hd, sizeof hd) && !memcmp(h->magic, "GT2SLOT1", 8) && h->slot < ST_SLOTS &&
            h->len && h->len <= st_hi - off - ST_HEAD && st_crc(off + ST_HEAD, h->len) == h->crc) {
            if (!st_rec[h->slot].off || h->seq > st_rec[h->slot].seq) {
                st_rec[h->slot].off = off;
                st_rec[h->slot].len = h->len;
                st_rec[h->slot].seq = h->seq;
                for (i = 0; i < 8; i++)
                    st_rec[h->slot].name[i] = h->name[i];
                st_rec[h->slot].name[8] = 0;
            }
            if (h->seq > st_seq)
                st_seq = h->seq;
            off += (ST_HEAD + h->len + ST_SECT - 1u) / ST_SECT * ST_SECT;
        } else {
            off += ST_SECT;
        }
    }
}

/* at boot: end = where the image and its pack end (flash offset) */
static void st_init(uint32_t end)
{
#if DOCK_BOOT
    (void)end;
#else
    st_lo = (end + ST_SECT - 1u) & ~(ST_SECT - 1u);
    st_hi = ST_END;
    if (st_lo + 8u * ST_SECT > st_hi) {    /* (under 32 KB left: none) */
        st_lo = st_hi = 0;
        return;
    }
    st_scan();
#endif
}

static int st_on(void) { return st_hi != 0; }
static uint32_t st_sectors(uint32_t len) { return (ST_HEAD + len + ST_SECT - 1u) / ST_SECT; }

/* is sector off part of a record that counts? */
static int st_used(uint32_t off)
{
    uint32_t i;
    for (i = 0; i < ST_SLOTS; i++)
        if (st_rec[i].off && off >= st_rec[i].off && off < st_rec[i].off + st_sectors(st_rec[i].len) * ST_SECT)
            return 1;
    return 0;
}

/* bytes free for one more song (the largest run of sectors no record holds) */
static uint32_t st_free(void)
{
    uint32_t off, run = 0, best = 0;
    for (off = st_lo; off + ST_SECT <= st_hi; off += ST_SECT) {
        run = st_used(off) ? 0 : run + ST_SECT;
        if (run > best)
            best = run;
    }
    return best > ST_HEAD ? best - ST_HEAD : 0;
}

/* the .sng's bytes to flash, a page at a time */
static struct { uint32_t off, n, crc; uint8_t page[256]; int err; } st_w;

static void st_put_crc(void *user, uint8_t b) { (void)user; st_w.crc = crc32_add(st_w.crc, b); }

static void st_flush(void)
{
    if (st_w.n && !st_w.err && fl_write(st_w.off, st_w.page, st_w.n))
        st_w.err = 1;
    st_w.off += st_w.n;
    st_w.n = 0;
}

static void st_put(void *user, uint8_t b)
{
    (void)user;
    st_w.page[st_w.n++] = b;
    if (((st_w.off + st_w.n) & 0xFFu) == 0)   /* (to the flash page's end) */
        st_flush();
}

static int st_erase(uint32_t off, uint32_t n)
{
    uint32_t took;
    for (; n; n--, off += ST_SECT) {
        fm1_wdt_feed();
        if (fl_erase4k(off, &took))
            return -1;
    }
    return 0;
}

/* g into slot (its name from the song's); 0, or -1 no store / -2 no room / -3 a flash error */
static int st_save(uint32_t slot, const gt2_t *g)
{
    uint32_t len, need, off, run = 0, at = 0, i;
    st_head_t h;
    if (!st_on() || slot >= ST_SLOTS)
        return -1;
    st_w.crc = 0xFFFFFFFFu;
    len = gt2_save_to(g, st_put_crc, 0);
    need = st_sectors(len);
    for (off = st_lo; off + ST_SECT <= st_hi && run < need; off += ST_SECT) {
        if (st_used(off))
            run = 0;
        else if (!run++)
            at = off;
    }
    if (run < need)
        return -2;
    if (st_erase(at, need))
        return -3;
    st_w.off = at + ST_HEAD;
    st_w.n = 0;
    st_w.err = 0;
    gt2_save_to(g, st_put, 0);
    st_flush();
    if (st_w.err)
        return -3;
    for (i = 0; i < 8; i++)
        h.magic[i] = "GT2SLOT1"[i];
    h.len = len;
    h.crc = ~st_w.crc;
    h.slot = slot;
    h.seq = st_seq + 1u;
    for (i = 0; i < 8; i++)
        h.name[i] = g->songname[i];
    if (fl_write(at, (const uint8_t *)&h, ST_HEAD) || st_crc(at + ST_HEAD, len) != h.crc)
        return -3;
    if (st_rec[slot].off)                  /* the slot's older record goes */
        st_erase(st_rec[slot].off, st_sectors(st_rec[slot].len));
    st_scan();
    return 0;
}

static int st_delete(uint32_t slot)
{
    if (!st_on() || slot >= ST_SLOTS || !st_rec[slot].off)
        return -1;
    if (st_erase(st_rec[slot].off, st_sectors(st_rec[slot].len)))
        return -3;
    st_scan();
    return 0;
}

/* the song in slot: its length (0: none), and into dst when there is room for it */
static uint32_t st_song(uint32_t slot, uint8_t *dst, uint32_t max)
{
    if (slot >= ST_SLOTS || !st_rec[slot].off)
        return 0;
    if (dst && (st_rec[slot].len > max || st_read(st_rec[slot].off + ST_HEAD, dst, st_rec[slot].len)))
        return 0;
    return st_rec[slot].len;
}
#else
/* fm1-dock's module: the songs in the host's banks (hb_module.h: bank_load_n / bank_save_n, 8 KiB of
 * the bank area each), not in flash of its own. A slot's record (the same 32-byte head, then the .sng)
 * in up to ST_BANKS banks of HB_BANK_MAX bytes: bank 1 + slot * ST_BANKS + k. It goes through RAM the
 * screen's last frame keeps (fb_shown: nothing draws while a song is saved or read; the next frame
 * is drawn whole). The bank area is shared by every module on the FM-1: a few songs fit. */
#define ST_BANKS 3u
static uint8_t fb_shown[240 * 120];         /* (fm1sid.c's, below: the staging buffer) */
static uint8_t shown_ok;
typedef char st_fits[ST_BANKS * HB_BANK_MAX <= sizeof fb_shown ? 1 : -1];
static uint32_t st_bank(uint32_t slot, uint32_t k) { return 1u + slot * ST_BANKS + k; }

static int st_on(void)
{
    return H->size >= (uint32_t)((const char *)&H->bank_free - (const char *)H) + sizeof H->bank_free;
}

/* slot's record into fb_shown, its bytes (head + song): 0 none or broken */
static uint32_t st_get(uint32_t slot)
{
    uint32_t n = 0, k;
    shown_ok = 0;
    for (k = 0; k < ST_BANKS; k++) {
        int got = H->bank_load_n(st_bank(slot, k), fb_shown + n, HB_BANK_MAX);
        if (got <= 0)
            break;
        n += (uint32_t)got;
        if ((uint32_t)got < HB_BANK_MAX)
            break;
    }
    return n;
}

static void st_scan(void)
{
    uint32_t slot, i;
    st_seq = 0;
    for (slot = 0; slot < ST_SLOTS; slot++) {
        const st_head_t *h = (const st_head_t *)fb_shown;
        uint32_t n, c = 0xFFFFFFFFu;
        st_rec[slot].off = 0;
        if (H->bank_load_n(st_bank(slot, 0), 0, 0) <= 0 || (n = st_get(slot)) < ST_HEAD)
            continue;
        if (memcmp(h->magic, "GT2SLOT1", 8) || h->slot != slot || h->len != n - ST_HEAD)
            continue;
        for (i = 0; i < h->len; i++)
            c = crc32_add(c, fb_shown[ST_HEAD + i]);
        if (~c != h->crc)
            continue;
        st_rec[slot].off = 1;               /* (here: there is one) */
        st_rec[slot].len = h->len;
        st_rec[slot].seq = h->seq;
        for (i = 0; i < 8; i++)
            st_rec[slot].name[i] = h->name[i];
        st_rec[slot].name[8] = 0;
        if (h->seq > st_seq)
            st_seq = h->seq;
    }
}

static void st_init(uint32_t end)
{
    (void)end;
    if (st_on())
        st_scan();
}

static uint32_t st_banks_of(uint32_t len) { return (ST_HEAD + len + HB_BANK_MAX - 1u) / HB_BANK_MAX; }

static uint32_t st_free(void)               /* (for the list: what the free banks hold) */
{
    uint32_t b = st_on() ? H->bank_free() : 0;
    return b * HB_BANK_MAX > ST_HEAD ? b * HB_BANK_MAX - ST_HEAD : 0;
}

static struct { uint32_t n, crc; } st_w;
static void st_put_crc(void *user, uint8_t b) { (void)user; st_w.crc = crc32_add(st_w.crc, b); }
static void st_put(void *user, uint8_t b)
{
    (void)user;
    if (st_w.n < ST_BANKS * HB_BANK_MAX)
        fb_shown[st_w.n] = b;
    st_w.n++;
}

static int st_save(uint32_t slot, const gt2_t *g)
{
    uint32_t len, total, k, need, had = 0, i;
    st_head_t *h = (st_head_t *)fb_shown;
    if (!st_on() || slot >= ST_SLOTS)
        return -1;
    st_w.crc = 0xFFFFFFFFu;
    len = gt2_save_to(g, st_put_crc, 0);
    total = ST_HEAD + len;
    need = st_banks_of(len);
    if (need > ST_BANKS)
        return -2;
    if (st_rec[slot].off)
        had = st_banks_of(st_rec[slot].len);
    if (need > had && need - had > H->bank_free())
        return -2;
    shown_ok = 0;
    st_w.n = ST_HEAD;
    gt2_save_to(g, st_put, 0);
    for (i = 0; i < 8; i++) {
        h->magic[i] = "GT2SLOT1"[i];
        h->name[i] = g->songname[i];
    }
    h->len = len;
    h->crc = ~st_w.crc;
    h->slot = slot;
    h->seq = st_seq + 1u;
    for (k = need; k-- > 0;) {              /* (the head's bank last) */
        uint32_t at = k * HB_BANK_MAX, n = total - at < HB_BANK_MAX ? total - at : HB_BANK_MAX;
        if (H->bank_save_n(st_bank(slot, k), fb_shown + at, n))
            return -3;
    }
    for (k = need; k < had; k++)            /* the banks the older record had over */
        H->bank_save_n(st_bank(slot, k), 0, 0);
    st_scan();
    return st_rec[slot].off ? 0 : -3;
}

static int st_delete(uint32_t slot)
{
    uint32_t k;
    if (!st_on() || slot >= ST_SLOTS || !st_rec[slot].off)
        return -1;
    for (k = 0; k < ST_BANKS; k++)
        H->bank_save_n(st_bank(slot, k), 0, 0);
    st_scan();
    return 0;
}

static uint32_t st_song(uint32_t slot, uint8_t *dst, uint32_t max)
{
    uint32_t n, i;
    if (slot >= ST_SLOTS || !st_rec[slot].off)
        return 0;
    if (!dst)
        return st_rec[slot].len;
    if (st_rec[slot].len > max || (n = st_get(slot)) != ST_HEAD + st_rec[slot].len)
        return 0;
    for (i = 0; i < st_rec[slot].len; i++)
        dst[i] = fb_shown[ST_HEAD + i];
    return st_rec[slot].len;
}
#endif
