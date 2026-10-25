// dock/resid_glue.cc - reSID 0.16 (Dag Lem, GPL-2.0-or-later, ext/resid-0.16) for FM-1 SID as an
// fm1-dock module: firmware/resid_glue.cc without an allocator and without static constructors
// (a module has nobody to run them: the chip is made in rs_init by placement new; fast sampling,
// the only mode used, allocates nothing), as fm1-dock's modules/sid/resid_glue.cc.
// SPDX-License-Identifier: GPL-3.0-only
#include "../ext/resid-0.16/sid.h"

void *operator new(__SIZE_TYPE__) { return 0; }
void *operator new[](__SIZE_TYPE__) { return 0; }
void operator delete(void *) {}
void operator delete[](void *) {}
inline void *operator new(__SIZE_TYPE__, void *p) { return p; }
extern "C" void __cxa_pure_virtual() { for (;;) {} }

static SID *chip;
static unsigned char store[sizeof(SID)] __attribute__((aligned(8)));

extern "C" void rs_init(int model, double clock, double fs)
{
    chip = new (store) SID();
    chip->set_chip_model(model ? MOS8580 : MOS6581);
    chip->enable_filter(true);
    chip->enable_external_filter(true);
    chip->set_sampling_parameters(clock, SAMPLE_FAST, fs);
}

extern "C" void rs_write(int reg, int val) { chip->write(reg, val); }

// clock up to *cycles, producing at most n samples; *cycles is what is left
extern "C" int rs_clock(int *cycles, short *buf, int n)
{
    cycle_count dt = *cycles;
    int got = chip->clock(dt, buf, n);
    *cycles = dt;
    return got;
}

// the voices' envelope levels (0..255), for the display
extern "C" void rs_env(unsigned char env[3])
{
    SID::State st = chip->read_state();
    env[0] = st.envelope_counter[0];
    env[1] = st.envelope_counter[1];
    env[2] = st.envelope_counter[2];
}
