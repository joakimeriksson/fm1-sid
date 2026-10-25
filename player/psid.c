/* player/psid.c - see psid.h
 * SPDX-License-Identifier: BSD-2-Clause */
#include <string.h>
#include "psid.h"
#define CPU6510_TYPES_ONLY
#include "cpu6510.h"                      /* (the CPU's state; its code after the bus, below) */
#undef CPU6510_TYPES_ONLY

/* RSID: a CIA's two timers and interrupt control (no TOD, no serial port, no keys) */
typedef struct {
    uint16_t ta, tb, la, lb;              /* counters and latches */
    uint8_t cra, crb;                     /* control (start, one-shot, timer B's input) */
    uint8_t icr, mask;                    /* interrupt flags, enabled sources */
    uint8_t pra;
} cia_t;

struct psid {
    cpu6510_t cpu;
    psid_info_t info;
    psid_write_fn wr;
    void *user;
    uint32_t cycle;                       /* cycles into the current call */
    uint32_t song;
    uint16_t cia_latch;
    uint32_t rnd;
    /* RSID */
    cia_t cia[2];                         /* CIA 1 (IRQ), CIA 2 (NMI) */
    uint16_t line, lcyc;                  /* the VIC's raster line and cycle in it */
    uint16_t raster_cmp;
    uint8_t d011, d019, d01a;
    uint8_t vic[64];                      /* the other VIC registers, as written */
    /* the chips run behind the CPU and catch up when it looks (chips_to): cycles of this c64_run */
    uint32_t done;                        /* the chips have stepped cycles 0 .. done - 1 */
    uint32_t next_ev;                     /* the first cycle something may change: the CPU runs up to it */
    uint32_t carry;                       /* the last instruction ran over the run's end by this */
    int32_t irq_t, nmi_t;                 /* the cycle the IRQ line went up, the NMI's edge */
    uint8_t irq_line, nmi_line, nmi_pend; /* the lines; an NMI edge not yet taken */
    uint8_t dirty;                        /* the CPU touched the VIC or a CIA: look again */
    uint32_t stall_at, stall_end;         /* this line's stop for the VIC (cycles 11..53), if a bad line */
    uint8_t poll_i;                       /* the I flag the last instruction's interrupt poll saw */
    uint16_t idle_ad;                     /* where init returns to: a JMP to itself */
    uint8_t mem[65536];
};

uint32_t psid_size(void) { return (uint32_t)sizeof(psid_t); }
const psid_info_t *psid_info(const psid_t *p) { return &p->info; }

static int io_on(const psid_t *p)
{
    uint8_t b = p->mem[1] & 7;
    return (b & 3) && (b & 4);
}

static uint8_t bus_read(psid_t *p, uint16_t ad)
{
    if (ad >= 0xD000 && ad < 0xE000 && io_on(p)) {
        if (ad >= 0xD400 && ad < 0xD800) {
            if ((ad & 0x1f) == 0x1b) {                 /* OSC3: a running random byte */
                p->rnd = p->rnd * 1103515245u + 12345u;
                return (uint8_t)(p->rnd >> 16);
            }
            return 0;
        }
        if (ad == 0xD012)
            return (uint8_t)((p->cycle / 63) % 312);   /* raster line, for busy-waits */
        if (ad == 0xD011)
            return (uint8_t)((((p->cycle / 63) % 312) >> 1) & 0x80);
        if (ad == 0xDC04)
            return (uint8_t)p->cia_latch;
        if (ad == 0xDC05)
            return (uint8_t)(p->cia_latch >> 8);
        return 0;
    }
    return p->mem[ad];
}

static void bus_write(psid_t *p, uint16_t ad, uint8_t v)
{
    if (ad >= 0xD000 && ad < 0xE000 && io_on(p)) {
        if (ad >= 0xD400 && ad < 0xD800)
            p->wr(p->user, p->cycle, ad & 0x1fu, v);
        else if (ad == 0xDC04)
            p->cia_latch = (uint16_t)((p->cia_latch & 0xff00) | v);
        else if (ad == 0xDC05)
            p->cia_latch = (uint16_t)((p->cia_latch & 0x00ff) | v << 8);
        return;
    }
    p->mem[ad] = v;
}

#include "cpu6510.h"

/* run from ad until the routine returns to the sentinel at $0000 / $0001 (RTI / RTS) or into
 * a KERNAL interrupt exit (no ROM here), at most max cycles */
static uint32_t call(psid_t *p, uint16_t ad, uint8_t a, int irq, uint32_t max)
{
    uint8_t s = 0xff, pi;
    p->mem[0x100 + s--] = 0x00;           /* return address $0000: RTS lands on $0001 */
    p->mem[0x100 + s--] = 0x00;
    if (irq)
        p->mem[0x100 + s--] = (uint8_t)(p->cpu.p | 0x20);
    p->cpu.s = s;
    p->cpu.a = a;
    p->cpu.x = p->cpu.y = 0;
    p->cpu.pc = ad;
    p->cpu.jam = 0;
    p->stall_at = 0xFFFFFFFFu;            /* (no VIC here) */
    p->cycle = 0;
    while (p->cycle < max) {
        uint16_t pc = p->cpu.pc;
        if (pc <= 1 || pc == 0xEA31 || pc == 0xEA81 || pc == 0xFEBC || p->cpu.jam)
            break;
        p->cpu.c0 = p->cycle;
        p->cycle = p->cpu.c0 + cpu_step(p, &pi);   /* (an I/O access sets p->cycle to its own cycle) */
    }
    return p->cycle;
}

/* ----------------------------------------------------------------------------- RSID --- */
/* the KERNAL's place: its interrupt entries and exits (the real ROM's code at the real
 * addresses, minus what a tune does not hear: the jiffy clock, the keyboard scan, RESTORE),
 * RTS anywhere else */
static const struct { uint16_t ad; uint8_t n; uint8_t b[19]; } kernal[] = {
    {0xEA31, 3, {0x4C, 0x7E, 0xEA}},                                   /* IRQ: JMP $EA7E */
    {0xEA7E, 9, {0xAD, 0x0D, 0xDC, 0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40}},   /* ack CIA 1; $EA81 */
    {0xFE43, 4, {0x78, 0x6C, 0x18, 0x03}},                             /* NMI: SEI, JMP ($0318) */
    {0xFE47, 11, {0x48, 0x8A, 0x48, 0x98, 0x48, 0xAD, 0x0D, 0xDD, 0x4C, 0xBC, 0xFE}},
    {0xFE66, 3, {0x4C, 0x81, 0xEA}},                                   /* BRK: out as an IRQ */
    {0xFEBC, 6, {0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40}},
    {0xFF48, 19, {0x48, 0x8A, 0x48, 0x98, 0x48, 0xBA, 0xBD, 0x04, 0x01, 0x29, 0x10,
                  0xF0, 0x03, 0x6C, 0x16, 0x03, 0x6C, 0x14, 0x03}},     /* IRQ / BRK entry */
    {0xFFFA, 6, {0x43, 0xFE, 0xE2, 0xFC, 0x48, 0xFF}},                 /* NMI, RESET, IRQ */
};

static uint8_t kernal_rd(uint16_t ad)
{
    uint32_t i;
    for (i = 0; i < sizeof kernal / sizeof kernal[0]; i++)
        if ((uint16_t)(ad - kernal[i].ad) < kernal[i].n)
            return kernal[i].b[ad - kernal[i].ad];
    return 0x60;
}

void psid_kernal(uint8_t rom[8192])
{
    uint32_t i;
    for (i = 0; i < 8192; i++)
        rom[i] = kernal_rd((uint16_t)(0xE000 + i));
}

static uint8_t cia_rd(psid_t *p, cia_t *c, uint32_t r)
{
    uint8_t v;
    switch (r) {
    case 0x0: return c == p->cia ? 0xff : c->pra;          /* no keys, no joystick */
    case 0x1: return 0xff;
    case 0x4: return (uint8_t)c->ta;
    case 0x5: return (uint8_t)(c->ta >> 8);
    case 0x6: return (uint8_t)c->tb;
    case 0x7: return (uint8_t)(c->tb >> 8);
    case 0xD:
        v = (uint8_t)(c->icr | ((c->icr & c->mask) ? 0x80 : 0));
        c->icr = 0;
        return v;
    case 0xE: return c->cra;
    case 0xF: return c->crb;
    default: return 0;
    }
}

static void cia_wr(cia_t *c, uint32_t r, uint8_t v)
{
    switch (r) {
    case 0x0: c->pra = v; break;
    case 0x4: c->la = (uint16_t)((c->la & 0xff00) | v); break;
    case 0x5:
        c->la = (uint16_t)((c->la & 0x00ff) | v << 8);
        if (!(c->cra & 1))                          /* a stopped timer loads the latch */
            c->ta = c->la;
        break;
    case 0x6: c->lb = (uint16_t)((c->lb & 0xff00) | v); break;
    case 0x7:
        c->lb = (uint16_t)((c->lb & 0x00ff) | v << 8);
        if (!(c->crb & 1))
            c->tb = c->lb;
        break;
    case 0xD:
        if (v & 0x80)
            c->mask |= v & 0x1f;
        else
            c->mask &= (uint8_t)~v;
        break;
    case 0xE:
        if (v & 0x10)                               /* force load (a strobe) */
            c->ta = c->la;
        c->cra = v & 0xef;
        break;
    case 0xF:
        if (v & 0x10)
            c->tb = c->lb;
        c->crb = v & 0xef;
        break;
    default: break;
    }
}

static void cia_tick(cia_t *c);

/* the IRQ and NMI lines as the chips are at cycle t (t: when they changed, for the CPU's poll) */
static void c64_lines(psid_t *p, uint32_t t)
{
    int irq = (p->cia[0].icr & p->cia[0].mask) || (p->d019 & p->d01a);
    int nmi = (p->cia[1].icr & p->cia[1].mask) != 0;
    if (irq && !p->irq_line)
        p->irq_t = (int32_t)t;
    p->irq_line = (uint8_t)irq;
    if (nmi && !p->nmi_line) {
        p->nmi_pend = 1;
        p->nmi_t = (int32_t)t;
    }
    p->nmi_line = (uint8_t)nmi;
}

/* The chips run behind the CPU: the VIC's raster and the CIAs' timers step to cycle t when the CPU
 * looks at them, or at the next cycle anything may change (c64_run). A cycle with something in it
 * (a new line, a timer's underflow) steps in full and updates the interrupt lines; the quiet ones
 * between only count, in one go. Cycle for cycle the same as stepping them every cycle. */
static void chips_to(psid_t *p, uint32_t t)
{
    while (p->done <= t) {
        uint32_t m = t + 1u - p->done, q = 62u - p->lcyc, i;
        for (i = 0; i < 2; i++) {
            const cia_t *c = &p->cia[i];
            if ((c->cra & 1) && c->ta < q)
                q = c->ta;
            if ((c->crb & 0x61) == 0x01 && c->tb < q)
                q = c->tb;
        }
        if (q > m)
            q = m;
        p->lcyc = (uint16_t)(p->lcyc + q);
        for (i = 0; i < 2; i++) {
            cia_t *c = &p->cia[i];
            if (c->cra & 1)
                c->ta = (uint16_t)(c->ta - q);
            if ((c->crb & 0x61) == 0x01)
                c->tb = (uint16_t)(c->tb - q);
        }
        p->done += q;
        if (p->done > t)
            break;
        if (++p->lcyc == 63) {            /* the cycle with something in it */
            p->lcyc = 0;
            if (++p->line == 312)
                p->line = 0;
            if (p->line == p->raster_cmp)
                p->d019 |= 1;
        }
        cia_tick(&p->cia[0]);
        cia_tick(&p->cia[1]);
        c64_lines(p, p->done);
        p->done++;
    }
}

static int bad_line(const psid_t *p)        /* the VIC takes the bus on this line (the CPU's reads in cycles 11..53) */
{
    return (p->d011 & 0x10) && p->line >= 0x30 && p->line <= 0xf7 && (p->line & 7) == (p->d011 & 7);
}

/* the chips at cycle t: the first cycle after it in which something may change */
static uint32_t next_event(const psid_t *p, uint32_t t)
{
    uint32_t q = 62u - p->lcyc, i;
    for (i = 0; i < 2; i++) {
        const cia_t *c = &p->cia[i];
        if ((c->cra & 1) && c->ta < q)
            q = c->ta;
        if ((c->crb & 0x61) == 0x01 && c->tb < q)
            q = c->tb;
    }
    return t + 1u + q;
}

static uint8_t c64_rd(psid_t *p, uint16_t ad)
{
    uint8_t b = p->mem[1] & 7;
    if (ad >= 0xD000) {
        if (ad >= 0xE000)
            return (b & 2) ? kernal_rd(ad) : p->mem[ad];
        if ((b & 3) && (b & 4)) {
            if (ad < 0xD400) {
                chips_to(p, p->cycle);
                p->dirty = 1;
                switch (ad & 0x3f) {
                case 0x11: return (uint8_t)((p->d011 & 0x7f) | (p->line >> 1 & 0x80));
                case 0x12: return (uint8_t)p->line;
                case 0x19: return (uint8_t)(p->d019 | 0x70 | ((p->d019 & p->d01a) ? 0x80 : 0));
                case 0x1a: return (uint8_t)(p->d01a | 0xf0);
                default: return p->vic[ad & 0x3f];
                }
            }
            if (ad < 0xD800) {
                switch (ad & 0x1f) {
                case 0x19: case 0x1a: return 0xff;                 /* paddles */
                case 0x1b:                                         /* OSC3: a running random byte */
                    p->rnd = p->rnd * 1103515245u + 12345u;
                    return (uint8_t)(p->rnd >> 16);
                default: return 0;
                }
            }
            if (ad < 0xDC00)
                return 0;                                          /* colour RAM */
            if (ad < 0xDE00) {
                uint8_t v;
                chips_to(p, p->cycle);
                p->dirty = 1;
                v = cia_rd(p, &p->cia[ad >> 8 & 1], ad & 0xfu);
                c64_lines(p, p->cycle);           /* (an ICR read acknowledges) */
                return v;
            }
            return 0;
        }
    }
    return p->mem[ad];
}

static void c64_wr(psid_t *p, uint16_t ad, uint8_t v)
{
    uint8_t b = p->mem[1] & 7;
    if (ad >= 0xD000 && ad < 0xE000 && (b & 3) && (b & 4)) {
        if (ad < 0xD400) {
            uint16_t old;
            chips_to(p, p->cycle);
            p->dirty = 1;
            old = p->raster_cmp;
            switch (ad & 0x3f) {
            case 0x11:
                p->d011 = v;
                p->raster_cmp = (uint16_t)((p->raster_cmp & 0xff) | (v & 0x80) << 1);
                break;
            case 0x12: p->raster_cmp = (uint16_t)((p->raster_cmp & 0x100) | v); break;
            case 0x19: p->d019 &= (uint8_t)~v & 0x0f; break;
            case 0x1a: p->d01a = v & 0x0f; break;
            default: p->vic[ad & 0x3f] = v; break;
            }
            if (p->raster_cmp != old && p->raster_cmp == p->line)  /* moved onto this line */
                p->d019 |= 1;
            c64_lines(p, p->cycle);
        } else if (ad < 0xD800) {
            p->wr(p->user, p->cycle - 1u, ad & 0x1fu, v);   /* (the CPU is a cycle ahead: rsid_start) */
        } else if (ad >= 0xDC00 && ad < 0xDE00) {
            chips_to(p, p->cycle);
            p->dirty = 1;
            cia_wr(&p->cia[ad >> 8 & 1], ad & 0xfu, v);
            c64_lines(p, p->cycle);
        }
        return;
    }
    p->mem[ad] = v;
}

static void cia_tick(cia_t *c)
{
    int under = 0;
    if (c->cra & 1) {
        if (c->ta)
            c->ta--;
        else {
            c->ta = c->la;
            c->icr |= 1;
            under = 1;
            if (c->cra & 8)
                c->cra &= 0xfe;
        }
    }
    if (c->crb & 1) {
        int count = (c->crb & 0x60) == 0 ? 1 : (c->crb & 0x60) == 0x40 ? under : 0;
        if (count) {
            if (c->tb)
                c->tb--;
            else {
                c->tb = c->lb;
                c->icr |= 2;
                if (c->crb & 8)
                    c->crb &= 0xfe;
            }
        }
    }
}

/* the CPU's accesses from $D000 up, at their cycle (player/cpu6510.h) */
static uint8_t cpu_rd_io(psid_t *p, uint16_t ad, uint32_t cycle)
{
    p->cycle = cycle;
    return p->info.rsid ? c64_rd(p, ad) : bus_read(p, ad);
}

static void cpu_wr_io(psid_t *p, uint16_t ad, uint8_t v, uint32_t cycle)
{
    p->cycle = cycle;
    if (p->info.rsid)
        c64_wr(p, ad, v);
    else
        bus_write(p, ad, v);
}

/* the C64 for n cycles: the CPU an instruction at a time, the chips behind it (chips_to), caught
 * up at the cycles anything may change (next_event) and when the CPU touches them. Between
 * instructions: a bad line's stop (the CPU waits to cycle 55, as its reads would), then the
 * interrupts (the 6502 polls in an instruction's next to last cycle: a line up 2 cycles before the
 * boundary, and IRQ with the I flag as it stood then), then a JMP to itself skipped up to the next
 * change. An instruction that runs past n carries over into the next run. */
#ifndef CPU_POLL
#define CPU_POLL 2                        /* a line up this many cycles before a boundary is taken there */
#endif
static void c64_run(psid_t *p, uint32_t n)
{
    uint32_t t;
    uint8_t pi;
    p->cycle = p->carry;
    while ((t = p->cycle) < n) {
        uint16_t pc;
        if (t >= p->next_ev || p->dirty) {
            chips_to(p, t);
            p->dirty = 0;
            if (bad_line(p) && p->lcyc < 54) {       /* (a line starts with this: the CPU's reads check it) */
                p->stall_at = p->lcyc < 11 ? t + 11u - p->lcyc : t;   /* (BA low from cycle 11, as */
                p->stall_end = t + 54u - p->lcyc;                   /* libsidplayfp's VIC has it) */
            } else {
                p->stall_at = 0xFFFFFFFFu;
            }
            p->next_ev = next_event(p, t);
        }
        if (p->cpu.jam) {
            p->cycle = n;
            break;
        }
        if (p->nmi_pend && p->nmi_t + CPU_POLL <= (int32_t)t) {
            p->nmi_pend = 0;
            p->cpu.c0 = t;
            p->cycle = t + cpu_interrupt(p, 0xfffa, 0);
            p->poll_i = F_I;
            continue;
        }
        if (p->irq_line && !p->poll_i && p->irq_t + CPU_POLL <= (int32_t)t) {
            p->cpu.c0 = t;
            p->cycle = t + cpu_interrupt(p, 0xfffe, 0);
            p->poll_i = F_I;
            continue;
        }
        pc = p->cpu.pc;
        if (pc < 0xCFFDu && p->mem[pc] == 0x4c && p->mem[pc + 1u] == (uint8_t)pc && p->mem[pc + 2u] == pc >> 8 &&
            !p->nmi_pend && !(p->irq_line && !p->poll_i)) {   /* JMP to itself: nothing till the next change */
            uint32_t to = p->next_ev < n ? p->next_ev : n, k;
            if (p->stall_at < to)
                to = p->stall_at;                 /* (a bad line stops the loop too: run that JMP) */
            k = to > t + 3u ? (to - t) / 3u : 0;  /* whole JMPs that end by then: its phase as it would run */
            if (k) {
                p->cycle = t + 3u * k;
                continue;
            }
        }
        p->cpu.c0 = t;
        p->cycle = t + cpu_step(p, &pi);
        p->poll_i = pi;
    }
    if (p->done < n)                      /* the chips through the run's end; past it is the next run's */
        chips_to(p, n - 1u);
    p->done -= n;
    p->carry = p->cycle - n;
    p->next_ev = p->next_ev > n ? p->next_ev - n : 0;
    p->irq_t -= (int32_t)n;
    p->nmi_t -= (int32_t)n;
    if (p->stall_at != 0xFFFFFFFFu) {
        p->stall_at = p->stall_at > n ? p->stall_at - n : 0;
        p->stall_end = p->stall_end > n ? p->stall_end - n : 0;
    }
    p->cycle = n;
}

/* RSID: the C64 as libsidplayfp's driver leaves it for init: CIA 1 timer A at 50 Hz with its
 * interrupt on, the raster compare at line 311, the KERNAL's vectors, $01 = $37, interrupts
 * enabled, init's return address an idle loop (JMP to itself) in RAM the tune leaves free */
static void rsid_start(psid_t *p, uint32_t song, uint16_t idle)
{
    uint8_t s = 0xff;
    cia_t *c = &p->cia[0];
    memset(p->cia, 0, sizeof p->cia);
    p->cia[0].ta = p->cia[0].tb = p->cia[0].la = p->cia[0].lb = 0xffff;    /* as they power up */
    p->cia[1].ta = p->cia[1].tb = p->cia[1].la = p->cia[1].lb = 0xffff;
    c->la = 0x4025;
    c->cra = 0x01;
    c->mask = 0x01;
    p->d011 = 0x9b;
    p->raster_cmp = 0x137;
    p->d019 = p->d01a = 0;
    /* the chips a cycle before line 0 cycle 0 (timer A a count up): the CPU's first cycle, init's
     * opcode fetch, is the chips' -1, and its access in cycle k sees them after k ticks, as the C64
     * is when it calls init with its next opcode fetched (the SID writes stamped a cycle back) */
    c->ta = 0x4026;
    p->line = 311;
    p->lcyc = 62;
    p->done = p->next_ev = p->carry = 0;
    p->stall_at = 0xFFFFFFFFu;
    p->irq_line = p->nmi_line = p->nmi_pend = 0;
    p->irq_t = p->nmi_t = 0;
    p->dirty = 1;
    p->mem[0] = 0x2f;
    p->mem[1] = 0x37;
    p->mem[0x2a6] = 1;                    /* PAL */
    p->mem[0x314] = 0x31; p->mem[0x315] = 0xea;
    p->mem[0x316] = 0x66; p->mem[0x317] = 0xfe;
    p->mem[0x318] = 0x47; p->mem[0x319] = 0xfe;
    p->mem[idle] = 0x4c;
    p->mem[(uint16_t)(idle + 1)] = (uint8_t)idle;
    p->mem[(uint16_t)(idle + 2)] = (uint8_t)(idle >> 8);
    p->mem[0x100 + s--] = (uint8_t)((idle - 1) >> 8);   /* init's RTS lands on the loop */
    p->mem[0x100 + s--] = (uint8_t)(idle - 1);
    p->cpu.s = s;
    p->cpu.p = 0x20;                      /* interrupts on */
    p->poll_i = 0;
    p->cpu.a = (uint8_t)(song - 1);
    p->cpu.x = p->cpu.y = 0;
    p->cpu.pc = p->info.init;
    p->cpu.jam = 0;
}

static uint16_t be16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }

static void str32(char *d, const uint8_t *s)
{
    memcpy(d, s, 32);
    d[32] = 0;
}

int psid_load(psid_t *p, const uint8_t *b, uint32_t n, psid_write_fn wr, void *user)
{
    uint16_t ver, flags = 0;
    uint32_t off, len;
    if (n < 0x76 || (memcmp(b, "PSID", 4) && memcmp(b, "RSID", 4)))
        return -1;
    memset(p, 0, sizeof *p);
    p->wr = wr;
    p->user = user;
    p->cia_latch = 0x4025;
    p->rnd = 0x1234567u;
    ver = be16(b + 4);
    off = be16(b + 6);
    if (off < 0x76 || off > n)              /* the data inside the file, after the header (#63) */
        return -1;
    p->info.load = be16(b + 8);
    p->info.init = be16(b + 10);
    p->info.play = be16(b + 12);
    p->info.songs = be16(b + 14);
    p->info.start = be16(b + 16);
    p->info.speed = (uint32_t)be16(b + 18) << 16 | be16(b + 20);
    str32(p->info.name, b + 0x16);
    str32(p->info.author, b + 0x36);
    str32(p->info.released, b + 0x56);
    if (ver >= 2 && n >= 0x7c)
        flags = be16(b + 0x76);
    p->info.model = ((flags >> 4) & 3) == 2 ? 1 : 0;
    p->info.rsid = b[0] == 'R';
    if (p->info.rsid && (flags & 2))
        return -1;                        /* a BASIC program: needs the BASIC ROM */
    if (!p->info.load) {
        if (n - off < 2)
            return -1;
        p->info.load = (uint16_t)(b[off] | b[off + 1] << 8);
        off += 2;
    }
    if (!p->info.init)
        p->info.init = p->info.load;
    len = n - off;
    if (len > 65536u - p->info.load)
        len = 65536u - p->info.load;
    memcpy(p->mem + p->info.load, b + off, len);
    p->mem[1] = 0x37;
    if (p->info.rsid) {
        /* the idle loop: in the page the header leaves free, else (as libsidplayfp places its
         * driver) the first page from $0400 that is neither the tune nor under BASIC */
        uint32_t pg = ver >= 2 && n >= 0x7c ? b[0x78] : 0, lo = p->info.load >> 8, hi = (p->info.load + len - 1) >> 8;
        if (!pg)
            for (pg = 4; pg < 0xd0 && ((pg >= lo && pg <= hi) || (pg >= 0xa0 && pg <= 0xbf)); pg++)
                ;
        p->idle_ad = pg >= 0xd0 ? 0x033c : (uint16_t)(pg << 8);
    }
    return 0;
}

uint32_t psid_init(psid_t *p, uint32_t song)
{
    if (!song)
        song = p->info.start ? p->info.start : 1;
    p->song = song;
    if (p->info.rsid) {
        rsid_start(p, song, p->idle_ad);
        return 0;
    }
    p->cpu.p = 0x24;                      /* I set */
    return call(p, p->info.init, (uint8_t)(song - 1), 0, 5u * PSID_PAL_CLOCK);
}

uint32_t psid_period(const psid_t *p)
{
    if (p->info.rsid)
        return PSID_PAL_FRAME;
    if (p->info.speed >> ((p->song - 1) & 31) & 1) {
        uint32_t t = (uint32_t)p->cia_latch + 1u;
        return t < 1000 ? PSID_PAL_FRAME : t;
    }
    return PSID_PAL_FRAME;
}

uint32_t psid_play(psid_t *p)
{
    uint16_t ad = p->info.play;
    int irq = 0;
    if (p->info.rsid) {
        c64_run(p, PSID_PAL_FRAME);
        return PSID_PAL_FRAME;
    }
    if (!ad) {                            /* the tune installed its own interrupt handler */
        ad = (p->mem[1] & 2) ? (uint16_t)(p->mem[0x314] | p->mem[0x315] << 8)
                             : (uint16_t)(p->mem[0xfffe] | p->mem[0xffff] << 8);
        irq = 1;
    }
    return call(p, ad, 0, irq, 2u * PSID_PAL_FRAME);
}
