/* player/cpu6510.h - an NMOS 6510 that runs an instruction at a time (included by player/psid.c).
 *
 * floooh's m6502 (ext/chips) runs a cycle at a time, a dispatch per clock: on the FM-1 a C64 tune
 * spent most of its time there (an RSID ~80 %, fm1-emu's profile, 2026-10-24). This runs a whole
 * instruction per dispatch and adds its cycles, as JaC64 and VICE's x64 do. Accesses keep their
 * cycle: every read and write names its cycle in the instruction (k, 0 = the opcode fetch), and one
 * that leaves RAM ($D000 up: I/O, ROM) goes to the bus at cpu.c0 + k, so the VIC, the CIAs and the
 * SID see the cycle the real CPU uses (a store's write, a read-modify-write's two writes, the dummy
 * reads of an indexed access that crosses a page). Below $D000 (RAM on any C64 bank setting the
 * player knows) an access is a plain array access.
 *
 * All 256 opcodes: the documented ones, decimal mode as the NMOS chip computes it, and the
 * undocumented ones tunes use (SLO RLA SRE RRA SAX LAX DCP ISC ANC ALR ARR SBX LAS SHA SHX SHY TAS
 * ANE LXA, the NOPs that read, JAM). The unstable ones (ANE, LXA: magic 0xEE; the SH* "& H+1" and
 * their page-cross address) as VICE has them.
 *
 * A bad line (psid.c: p->stall_at .. p->stall_end, the VIC's cycles 11..53) stops the CPU at its
 * first read in it, to the end (its writes go on, as the 6510's RDY lets them): every read checks,
 * the reads of an instruction's internal cycles at its end (cpu_step), and the accesses after the
 * stop happen that much later.
 *
 * The bus (psid.c): cpu_rd_io(p, ad, cycle) / cpu_wr_io(p, ad, v, cycle) for $D000 up; the CPU's
 * state is p->cpu. Interrupts are psid.c's, through cpu_interrupt.
 * SPDX-License-Identifier: BSD-2-Clause */

#ifndef CPU6510_TYPES                     /* (psid.c includes it twice: the type for its struct, then the code) */
#define CPU6510_TYPES
typedef struct {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint8_t jam;                          /* a JAM opcode: the CPU stopped (until init again) */
    uint32_t c0;                          /* the instruction's first cycle */
    uint32_t d;                           /* the cycles it has waited for the VIC so far (a bad line) */
} cpu6510_t;

#define F_C 0x01u
#define F_Z 0x02u
#define F_I 0x04u
#define F_D 0x08u
#define F_B 0x10u
#define F_U 0x20u
#define F_V 0x40u
#define F_N 0x80u
#endif

#ifndef CPU6510_TYPES_ONLY
#ifndef CPU6510_TRACE
#define CPU6510_TRACE(p) ((void)0)        /* (a test's hook: an instruction or an interrupt starts) */
#endif

static uint8_t cpu_rd_io(psid_t *p, uint16_t ad, uint32_t cycle);
static void cpu_wr_io(psid_t *p, uint16_t ad, uint8_t v, uint32_t cycle);

#define C (&p->cpu)
#define AT(k) (C->c0 + C->d + (k))           /* the cycle of the instruction's access k */
/* a read at k in a bad line's stop: the CPU waits to its end */
static inline int cpu_stall(psid_t *p, uint32_t at)
{
    if (at < p->stall_end)
        C->d += p->stall_end - at;
    p->stall_at = 0xFFFFFFFFu;
    return 0;
}
#define STALL(k) ((void)(AT(k) >= p->stall_at ? cpu_stall(p, AT(k)) : 0))
/* a read: inline the common case (RAM, no stop for the VIC), the rest out of line (small code) */
static __attribute__((noinline)) uint8_t cpu_rd_slow(psid_t *p, uint16_t ad, uint32_t k)
{
    STALL(k);
    return ad < 0xD000u ? p->mem[ad] : cpu_rd_io(p, ad, AT(k));
}
static __attribute__((noinline)) void cpu_wr_slow(psid_t *p, uint16_t ad, uint8_t v, uint32_t k)
{
    cpu_wr_io(p, ad, v, AT(k));
}
#define RD(ad, k) ((uint16_t)(ad) < 0xD000u && AT(k) < p->stall_at ? p->mem[(uint16_t)(ad)] : cpu_rd_slow(p, (uint16_t)(ad), (k)))
#define WR(ad, v, k) do { uint16_t ad_ = (uint16_t)(ad); uint8_t v_ = (uint8_t)(v);                  \
        if (ad_ < 0xD000u) p->mem[ad_] = v_; else cpu_wr_slow(p, ad_, v_, (k)); } while (0)
/* a dummy read: only an I/O register can tell (a CIA's ICR read acknowledges); a read for the VIC too */
#define DUMMY(ad, k) do { if ((uint16_t)(ad) >= 0xD000u || AT(k) >= p->stall_at) (void)cpu_rd_slow(p, (uint16_t)(ad), (k)); } while (0)
#define ZRD(ad, k) (STALL(k), p->mem[(uint8_t)(ad)])  /* a zero page pointer's byte */
#define PUSH(v) (p->mem[0x100u | C->s--] = (uint8_t)(v))
#define PULL(k) (STALL(k), p->mem[0x100u | ++C->s])

static inline void cpu_nz(psid_t *p, uint8_t v) { C->p = (uint8_t)((C->p & ~(F_N | F_Z)) | (v & F_N) | (v ? 0 : F_Z)); }

static __attribute__((noinline)) void cpu_adc(psid_t *p, uint8_t v)
{
    uint32_t a = C->a, c = C->p & F_C;
    if (!(C->p & F_D)) {
        uint32_t t = a + v + c;
        C->p = (uint8_t)((C->p & ~(F_C | F_V)) | (t > 0xffu ? F_C : 0) | ((~(a ^ v) & (a ^ t) & 0x80u) ? F_V : 0));
        C->a = (uint8_t)t;
        cpu_nz(p, C->a);
    } else {                              /* NMOS decimal (VICE's): N, V from the half-adjusted sum, Z binary */
        uint32_t t = (a & 0xfu) + (v & 0xfu) + c;
        if (t > 0x9u)
            t += 6u;
        t = t <= 0x0fu ? (t & 0xfu) + (a & 0xf0u) + (v & 0xf0u) : (t & 0xfu) + (a & 0xf0u) + (v & 0xf0u) + 0x10u;
        C->p = (uint8_t)(C->p & ~(F_N | F_Z | F_V | F_C));
        if (!((a + v + c) & 0xffu)) C->p |= F_Z;
        if (t & 0x80u) C->p |= F_N;
        if (((a ^ t) & 0x80u) && !((a ^ v) & 0x80u)) C->p |= F_V;
        if ((t & 0x1f0u) > 0x90u) t += 0x60u;
        if ((t & 0xff0u) > 0xf0u) C->p |= F_C;
        C->a = (uint8_t)t;
    }
}

static __attribute__((noinline)) void cpu_sbc(psid_t *p, uint8_t v)
{
    uint32_t a = C->a, b = (C->p & F_C) ? 0u : 1u, t = a - v - b;
    C->p = (uint8_t)((C->p & ~(F_C | F_V)) | (t < 0x100u ? F_C : 0) | (((a ^ t) & (a ^ v) & 0x80u) ? F_V : 0));
    cpu_nz(p, (uint8_t)t);
    if (C->p & F_D) {                     /* NMOS decimal (VICE's): the flags binary */
        uint32_t d = (a & 0xfu) - (v & 0xfu) - b;
        d = (d & 0x10u) ? ((d - 6u) & 0xfu) | ((a & 0xf0u) - (v & 0xf0u) - 0x10u) : (d & 0xfu) | ((a & 0xf0u) - (v & 0xf0u));
        if (d & 0x100u) d -= 0x60u;
        C->a = (uint8_t)d;
    } else {
        C->a = (uint8_t)t;
    }
}

static inline void cpu_cmp(psid_t *p, uint8_t r, uint8_t v)
{
    uint32_t t = (uint32_t)r - v;
    C->p = (uint8_t)((C->p & ~F_C) | (t < 0x100u ? F_C : 0));
    cpu_nz(p, (uint8_t)t);
}

static inline uint8_t cpu_asl(psid_t *p, uint8_t v) { C->p = (uint8_t)((C->p & ~F_C) | (v >> 7)); v = (uint8_t)(v << 1); cpu_nz(p, v); return v; }
static inline uint8_t cpu_lsr(psid_t *p, uint8_t v) { C->p = (uint8_t)((C->p & ~F_C) | (v & 1u)); v >>= 1; cpu_nz(p, v); return v; }
static inline uint8_t cpu_rol(psid_t *p, uint8_t v) { uint8_t c = C->p & F_C; C->p = (uint8_t)((C->p & ~F_C) | (v >> 7)); v = (uint8_t)(v << 1 | c); cpu_nz(p, v); return v; }
static inline uint8_t cpu_ror(psid_t *p, uint8_t v) { uint8_t c = (uint8_t)((C->p & F_C) << 7); C->p = (uint8_t)((C->p & ~F_C) | (v & 1u)); v = (uint8_t)(v >> 1 | c); cpu_nz(p, v); return v; }

static inline uint8_t cpu_inc(psid_t *p, uint8_t v) { v++; cpu_nz(p, v); return v; }
static inline uint8_t cpu_dec(psid_t *p, uint8_t v) { v--; cpu_nz(p, v); return v; }
static inline uint8_t cpu_slo(psid_t *p, uint8_t v) { v = cpu_asl(p, v); C->a |= v; cpu_nz(p, C->a); return v; }
static inline uint8_t cpu_rla(psid_t *p, uint8_t v) { v = cpu_rol(p, v); C->a &= v; cpu_nz(p, C->a); return v; }
static inline uint8_t cpu_sre(psid_t *p, uint8_t v) { v = cpu_lsr(p, v); C->a ^= v; cpu_nz(p, C->a); return v; }
static inline uint8_t cpu_rra(psid_t *p, uint8_t v) { v = cpu_ror(p, v); cpu_adc(p, v); return v; }
static inline uint8_t cpu_dcp(psid_t *p, uint8_t v) { v--; cpu_cmp(p, C->a, v); return v; }
static inline uint8_t cpu_isc(psid_t *p, uint8_t v) { v++; cpu_sbc(p, v); return v; }

/* push PC and P, jump through a vector (an interrupt's 7 cycles); returns the cycles */
static uint32_t cpu_interrupt(psid_t *p, uint16_t vec, uint8_t b)
{
    if (!b) {                             /* (BRK: cpu_step has begun the instruction) */
        CPU6510_TRACE(p);
        C->d = 0;
        STALL(0);
    }
    PUSH(C->pc >> 8);
    PUSH(C->pc);
    PUSH((C->p & ~F_B) | F_U | b);
    C->p |= F_I;
    C->pc = (uint16_t)(RD(vec, 5) | RD((uint16_t)(vec + 1), 6) << 8);
    return b ? 7u : 7u + C->d;
}

/* one instruction at C->c0 (the caller sets it); returns its cycles. *poll_i: the I flag the
 * interrupt poll during it saw (CLI / SEI / PLP change it after the poll) */
static uint32_t cpu_step(psid_t *p, uint8_t *poll_i)
{
    uint16_t pc = C->pc, ea = 0, base;
    uint8_t op, b1, v, lo, zp;
    uint32_t n = 2, w = 0;                /* w: the cycles at its end that write (no stop for the VIC) */
    *poll_i = C->p & F_I;
    C->d = 0;
    CPU6510_TRACE(p);
    op = RD(pc, 0);
    b1 = RD(pc + 1u, 1);                  /* (every instruction reads the byte after its opcode in cycle 1) */
    switch (op) {
/* the addressing modes: ea, and the cycles before the access (the access at cycle K) */
#define IMM()  (v = b1, C->pc = (uint16_t)(pc + 2u))
#define ZP()   (ea = b1, C->pc = (uint16_t)(pc + 2u))
#define ZPX()  (ea = (uint8_t)(b1 + C->x), C->pc = (uint16_t)(pc + 2u))
#define ZPY()  (ea = (uint8_t)(b1 + C->y), C->pc = (uint16_t)(pc + 2u))
#define ABS()  (ea = (uint16_t)(b1 | RD(pc + 2u, 2) << 8), C->pc = (uint16_t)(pc + 3u))
#define ABI(r) (base = (uint16_t)(b1 | RD(pc + 2u, 2) << 8), ea = (uint16_t)(base + (r)), C->pc = (uint16_t)(pc + 3u))
#define IZX()  (zp = (uint8_t)(b1 + C->x), ea = (uint16_t)(ZRD(zp, 3) | ZRD(zp + 1u, 4) << 8), C->pc = (uint16_t)(pc + 2u))
#define IZY()  (zp = b1, base = (uint16_t)(ZRD(zp, 2) | ZRD(zp + 1u, 3) << 8), ea = (uint16_t)(base + C->y), C->pc = (uint16_t)(pc + 2u))
#define CROSS() ((base ^ ea) & 0xff00u)
#define WRONG() ((uint16_t)((base & 0xff00u) | (ea & 0xffu)))     /* the address before the carry */
/* reads: v, and n the cycles (an indexed read 1 more across a page, its dummy read first) */
#define R_ZP   ZP(); v = RD(ea, 2); n = 3
#define R_ZPX  ZPX(); v = RD(ea, 3); n = 4
#define R_ZPY  ZPY(); v = RD(ea, 3); n = 4
#define R_ABS  ABS(); v = RD(ea, 3); n = 4
#define R_ABX  ABI(C->x); if (CROSS()) { DUMMY(WRONG(), 3); v = RD(ea, 4); n = 5; } else { v = RD(ea, 3); n = 4; }
#define R_ABY  ABI(C->y); if (CROSS()) { DUMMY(WRONG(), 3); v = RD(ea, 4); n = 5; } else { v = RD(ea, 3); n = 4; }
#define R_IZX  IZX(); v = RD(ea, 5); n = 6
#define R_IZY  IZY(); if (CROSS()) { DUMMY(WRONG(), 4); v = RD(ea, 5); n = 6; } else { v = RD(ea, 4); n = 5; }
/* writes: the address and the cycle of the write (an indexed one always reads its wrong address first) */
#define W_ZP(val_)  ZP(); WR(ea, val_, 2); n = 3; w = 1
#define W_ZPX(val_) ZPX(); WR(ea, val_, 3); n = 4; w = 1
#define W_ZPY(val_) ZPY(); WR(ea, val_, 3); n = 4; w = 1
#define W_ABS(val_) ABS(); WR(ea, val_, 3); n = 4; w = 1
#define W_ABX(val_) ABI(C->x); DUMMY(WRONG(), 3); WR(ea, val_, 4); n = 5; w = 1
#define W_ABY(val_) ABI(C->y); DUMMY(WRONG(), 3); WR(ea, val_, 4); n = 5; w = 1
#define W_IZX(val_) IZX(); WR(ea, val_, 5); n = 6; w = 1
#define W_IZY(val_) IZY(); DUMMY(WRONG(), 4); WR(ea, val_, 5); n = 6; w = 1
/* read-modify-write: read at K, the old value written back at K+1, the new one at K+2 */
#define RMW(K, f) do { uint8_t o_ = RD(ea, K); WR(ea, o_, (K) + 1); v = f(p, o_); WR(ea, v, (K) + 2); n = (K) + 3; w = 2; } while (0)
#define M_ZP(f)  ZP(); RMW(2, f)
#define M_ZPX(f) ZPX(); RMW(3, f)
#define M_ABS(f) ABS(); RMW(3, f)
#define M_ABX(f) ABI(C->x); DUMMY(WRONG(), 3); RMW(4, f)
#define M_ABY(f) ABI(C->y); DUMMY(WRONG(), 3); RMW(4, f)
#define M_IZX(f) IZX(); RMW(5, f)
#define M_IZY(f) IZY(); DUMMY(WRONG(), 4); RMW(5, f)
/* the eight addressing modes of a group: a row of the opcode table */
#define READ8(o, body) \
    case o + 0x01: R_IZX; body; break; case o + 0x05: R_ZP; body; break; case o + 0x09: IMM(); body; break; \
    case o + 0x0d: R_ABS; body; break; case o + 0x11: R_IZY; body; break; case o + 0x15: R_ZPX; body; break; \
    case o + 0x19: R_ABY; body; break; case o + 0x1d: R_ABX; body; break;
    READ8(0x00, C->a |= v; cpu_nz(p, C->a))                             /* ORA */
    READ8(0x20, C->a &= v; cpu_nz(p, C->a))                             /* AND */
    READ8(0x40, C->a ^= v; cpu_nz(p, C->a))                             /* EOR */
    READ8(0x60, cpu_adc(p, v))                                          /* ADC */
    READ8(0xa0, C->a = v; cpu_nz(p, v))                                 /* LDA */
    READ8(0xc0, cpu_cmp(p, C->a, v))                                    /* CMP */
    READ8(0xe0, cpu_sbc(p, v))                                          /* SBC */
    case 0xeb: IMM(); cpu_sbc(p, v); break;                             /* SBC (undocumented) */
    /* STA */
    case 0x81: W_IZX(C->a); break; case 0x85: W_ZP(C->a); break; case 0x8d: W_ABS(C->a); break;
    case 0x91: W_IZY(C->a); break; case 0x95: W_ZPX(C->a); break; case 0x99: W_ABY(C->a); break;
    case 0x9d: W_ABX(C->a); break;
    case 0x86: W_ZP(C->x); break; case 0x96: W_ZPY(C->x); break; case 0x8e: W_ABS(C->x); break;   /* STX */
    case 0x84: W_ZP(C->y); break; case 0x94: W_ZPX(C->y); break; case 0x8c: W_ABS(C->y); break;   /* STY */
    /* LDX, LDY, CPX, CPY, BIT */
    case 0xa2: IMM(); C->x = v; cpu_nz(p, v); break; case 0xa6: R_ZP; C->x = v; cpu_nz(p, v); break;
    case 0xb6: R_ZPY; C->x = v; cpu_nz(p, v); break; case 0xae: R_ABS; C->x = v; cpu_nz(p, v); break;
    case 0xbe: R_ABY; C->x = v; cpu_nz(p, v); break;
    case 0xa0: IMM(); C->y = v; cpu_nz(p, v); break; case 0xa4: R_ZP; C->y = v; cpu_nz(p, v); break;
    case 0xb4: R_ZPX; C->y = v; cpu_nz(p, v); break; case 0xac: R_ABS; C->y = v; cpu_nz(p, v); break;
    case 0xbc: R_ABX; C->y = v; cpu_nz(p, v); break;
    case 0xe0: IMM(); cpu_cmp(p, C->x, v); break; case 0xe4: R_ZP; cpu_cmp(p, C->x, v); break;
    case 0xec: R_ABS; cpu_cmp(p, C->x, v); break;
    case 0xc0: IMM(); cpu_cmp(p, C->y, v); break; case 0xc4: R_ZP; cpu_cmp(p, C->y, v); break;
    case 0xcc: R_ABS; cpu_cmp(p, C->y, v); break;
    case 0x24: R_ZP; goto bit; case 0x2c: R_ABS;
    bit: C->p = (uint8_t)((C->p & ~(F_N | F_V | F_Z)) | (v & (F_N | F_V)) | ((C->a & v) ? 0 : F_Z)); break;
    /* shifts and INC / DEC: the accumulator, then memory */
    case 0x0a: C->a = cpu_asl(p, C->a); C->pc = (uint16_t)(pc + 1u); break;
    case 0x4a: C->a = cpu_lsr(p, C->a); C->pc = (uint16_t)(pc + 1u); break;
    case 0x2a: C->a = cpu_rol(p, C->a); C->pc = (uint16_t)(pc + 1u); break;
    case 0x6a: C->a = cpu_ror(p, C->a); C->pc = (uint16_t)(pc + 1u); break;
#define RMW5(o, f) case o + 0x06: M_ZP(f); break; case o + 0x16: M_ZPX(f); break; \
    case o + 0x0e: M_ABS(f); break; case o + 0x1e: M_ABX(f); break;
    RMW5(0x00, cpu_asl) RMW5(0x20, cpu_rol) RMW5(0x40, cpu_lsr) RMW5(0x60, cpu_ror)
    RMW5(0xc0, cpu_dec) RMW5(0xe0, cpu_inc)
    /* the undocumented read-modify-writes: the shift or INC / DEC, then the ALU with the result */
#define URMW(o, f) case o + 0x03: M_IZX(f); break; case o + 0x07: M_ZP(f); break; case o + 0x0f: M_ABS(f); break; \
    case o + 0x13: M_IZY(f); break; case o + 0x17: M_ZPX(f); break; case o + 0x1b: M_ABY(f); break; \
    case o + 0x1f: M_ABX(f); break;
    URMW(0x00, cpu_slo) URMW(0x20, cpu_rla) URMW(0x40, cpu_sre) URMW(0x60, cpu_rra)
    URMW(0xc0, cpu_dcp) URMW(0xe0, cpu_isc)
    /* SAX, LAX */
    case 0x83: W_IZX(C->a & C->x); break; case 0x87: W_ZP(C->a & C->x); break;
    case 0x8f: W_ABS(C->a & C->x); break; case 0x97: W_ZPY(C->a & C->x); break;
    case 0xa3: R_IZX; goto lax; case 0xa7: R_ZP; goto lax; case 0xaf: R_ABS; goto lax;
    case 0xb3: R_IZY; goto lax; case 0xb7: R_ZPY; goto lax; case 0xbf: R_ABY;
    lax: C->a = C->x = v; cpu_nz(p, v); break;
    case 0xab: IMM(); C->a = C->x = (uint8_t)((C->a | 0xeeu) & v); cpu_nz(p, C->a); break;    /* LXA */
    case 0x8b: IMM(); C->a = (uint8_t)((C->a | 0xeeu) & C->x & v); cpu_nz(p, C->a); break;   /* ANE */
    case 0x0b: case 0x2b: IMM(); C->a &= v; cpu_nz(p, C->a); C->p = (uint8_t)((C->p & ~F_C) | (C->a >> 7)); break;  /* ANC */
    case 0x4b: IMM(); C->a = cpu_lsr(p, (uint8_t)(C->a & v)); break;                        /* ALR */
    case 0x6b: {                                                                            /* ARR */
        uint8_t t, r;
        IMM();
        t = (uint8_t)(C->a & v);
        r = (uint8_t)(t >> 1 | (C->p & F_C) << 7);
        if (C->p & F_D) {
            C->p = (uint8_t)((C->p & ~(F_N | F_Z | F_V | F_C)) | ((C->p & F_C) << 7) | (r ? 0 : F_Z) | ((r ^ t) & F_V));
            if ((t & 0xfu) + (t & 0x1u) > 0x5u)
                r = (uint8_t)((r & 0xf0u) | ((r + 6u) & 0xfu));
            if ((t & 0xf0u) + (t & 0x10u) > 0x50u) {
                r = (uint8_t)((r & 0x0fu) | ((r + 0x60u) & 0xf0u));
                C->p |= F_C;
            }
        } else {
            cpu_nz(p, r);
            C->p = (uint8_t)((C->p & ~(F_C | F_V)) | (r & 0x40u ? F_C : 0) | (((r >> 6) ^ (r >> 5)) & 1u ? F_V : 0));
        }
        C->a = r;
        break;
    }
    case 0xcb: {                                                                            /* SBX */
        uint32_t t;
        IMM();
        t = (uint32_t)(C->a & C->x) - v;
        C->x = (uint8_t)t;
        C->p = (uint8_t)((C->p & ~F_C) | (t < 0x100u ? F_C : 0));
        cpu_nz(p, C->x);
        break;
    }
    case 0xbb: R_ABY; C->a = C->x = C->s = (uint8_t)(v & C->s); cpu_nz(p, C->a); break;      /* LAS */
    /* SHA / SHX / SHY / TAS: the value & (the base's high byte + 1); across a page it is the address's
     * high byte too */
#define SH(r, val_) do { uint8_t h_ = (uint8_t)((base >> 8) + 1u), w_ = (uint8_t)((val_) & h_);             \
        DUMMY(WRONG(), r); if (CROSS()) ea = (uint16_t)((ea & 0xffu) | w_ << 8); WR(ea, w_, (r) + 1); w = 1; } while (0)
    case 0x9f: ABI(C->y); SH(3, C->a & C->x); n = 5; break;
    case 0x93: IZY(); SH(4, C->a & C->x); n = 6; break;
    case 0x9e: ABI(C->y); SH(3, C->x); n = 5; break;
    case 0x9c: ABI(C->x); SH(3, C->y); n = 5; break;
    case 0x9b: ABI(C->y); C->s = (uint8_t)(C->a & C->x); SH(3, C->s); n = 5; break;
    /* NOPs: the implied, the immediate, and the ones that read (an I/O register can tell) */
    case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa: case 0xea: C->pc = (uint16_t)(pc + 1u); break;
    case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2: C->pc = (uint16_t)(pc + 2u); break;
    case 0x04: case 0x44: case 0x64: R_ZP; break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4: R_ZPX; break;
    case 0x0c: R_ABS; break;
    case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc: R_ABX; break;
    /* the registers */
    case 0xaa: C->x = C->a; cpu_nz(p, C->x); C->pc = (uint16_t)(pc + 1u); break;   /* TAX */
    case 0x8a: C->a = C->x; cpu_nz(p, C->a); C->pc = (uint16_t)(pc + 1u); break;   /* TXA */
    case 0xa8: C->y = C->a; cpu_nz(p, C->y); C->pc = (uint16_t)(pc + 1u); break;   /* TAY */
    case 0x98: C->a = C->y; cpu_nz(p, C->a); C->pc = (uint16_t)(pc + 1u); break;   /* TYA */
    case 0xba: C->x = C->s; cpu_nz(p, C->x); C->pc = (uint16_t)(pc + 1u); break;   /* TSX */
    case 0x9a: C->s = C->x; C->pc = (uint16_t)(pc + 1u); break;                    /* TXS */
    case 0xe8: C->x++; cpu_nz(p, C->x); C->pc = (uint16_t)(pc + 1u); break;        /* INX */
    case 0xca: C->x--; cpu_nz(p, C->x); C->pc = (uint16_t)(pc + 1u); break;        /* DEX */
    case 0xc8: C->y++; cpu_nz(p, C->y); C->pc = (uint16_t)(pc + 1u); break;        /* INY */
    case 0x88: C->y--; cpu_nz(p, C->y); C->pc = (uint16_t)(pc + 1u); break;        /* DEY */
    /* the flags */
    case 0x18: C->p &= (uint8_t)~F_C; C->pc = (uint16_t)(pc + 1u); break;
    case 0x38: C->p |= F_C; C->pc = (uint16_t)(pc + 1u); break;
    case 0x58: C->p &= (uint8_t)~F_I; C->pc = (uint16_t)(pc + 1u); break;          /* CLI (the poll saw I set) */
    case 0x78: C->p |= F_I; C->pc = (uint16_t)(pc + 1u); break;                    /* SEI (the poll saw it clear) */
    case 0xb8: C->p &= (uint8_t)~F_V; C->pc = (uint16_t)(pc + 1u); break;
    case 0xd8: C->p &= (uint8_t)~F_D; C->pc = (uint16_t)(pc + 1u); break;
    case 0xf8: C->p |= F_D; C->pc = (uint16_t)(pc + 1u); break;
    /* the stack */
    case 0x48: PUSH(C->a); C->pc = (uint16_t)(pc + 1u); n = 3; w = 1; break;                      /* PHA */
    case 0x08: PUSH(C->p | F_B | F_U); C->pc = (uint16_t)(pc + 1u); n = 3; w = 1; break;         /* PHP */
    case 0x68: C->a = PULL(3); cpu_nz(p, C->a); C->pc = (uint16_t)(pc + 1u); n = 4; break;       /* PLA */
    case 0x28: C->p = (uint8_t)((PULL(3) & ~F_B) | F_U); C->pc = (uint16_t)(pc + 1u); n = 4; break;  /* PLP */
    case 0x20:                                                                                    /* JSR */
        lo = b1;
        PUSH((pc + 2u) >> 8);
        PUSH(pc + 2u);
        C->pc = (uint16_t)(lo | RD(pc + 2u, 5) << 8);
        n = 6;
        break;
    case 0x60:                                                                                    /* RTS */
        lo = PULL(3);
        C->pc = (uint16_t)((lo | PULL(4) << 8) + 1u);
        n = 6;
        break;
    case 0x40:                                                                                    /* RTI */
        C->p = (uint8_t)((PULL(3) & ~F_B) | F_U);
        lo = PULL(4);
        C->pc = (uint16_t)(lo | PULL(5) << 8);
        *poll_i = C->p & F_I;                                         /* (RTI's flag counts at once) */
        n = 6;
        break;
    case 0x00:                                                                                    /* BRK */
        C->pc = (uint16_t)(pc + 2u);
        n = cpu_interrupt(p, 0xfffe, F_B);
        break;
    /* jumps and branches */
    case 0x4c: C->pc = (uint16_t)(b1 | RD(pc + 2u, 2) << 8); n = 3; break;            /* JMP */
    case 0x6c:                                                                                    /* JMP () */
        base = (uint16_t)(b1 | RD(pc + 2u, 2) << 8);
        C->pc = (uint16_t)(RD(base, 3) | RD((base & 0xff00u) | ((base + 1u) & 0xffu), 4) << 8);   /* (the page wrap) */
        n = 5;
        break;
#define BR(o, cond) case o: { int8_t d_ = (int8_t)b1; C->pc = (uint16_t)(pc + 2u);                \
        if (cond) { uint16_t t_ = (uint16_t)(C->pc + d_); n = ((t_ ^ C->pc) & 0xff00u) ? 4u : 3u; C->pc = t_; } } break;
    BR(0x10, !(C->p & F_N)) BR(0x30, C->p & F_N) BR(0x50, !(C->p & F_V)) BR(0x70, C->p & F_V)
    BR(0x90, !(C->p & F_C)) BR(0xb0, C->p & F_C) BR(0xd0, !(C->p & F_Z)) BR(0xf0, C->p & F_Z)
    default:                              /* JAM (x2, x12, ... xF2): the CPU stops */
        C->jam = 1;
        n = 2;
        break;
    }
    /* a bad line's stop that began at a read this did not name (an internal cycle's) */
    if (p->stall_at < AT(n - w))
        cpu_stall(p, p->stall_at > AT(0) ? p->stall_at : AT(0));
    return n + C->d;
}
#undef C
#endif
