/* tools/combtab.c - src/sid.c's combined-waveform tables as a C header of constants, for a build
 * whose RAM cannot spare their 64 KB (-DSID_COMB_CONST: fm1-dock's module, fm1-dock/):
 *   cc -O2 -Isrc -o build/combtab tools/combtab.c && build/combtab > build/sid_comb.h
 * The tables are comb_build's own, computed here as on the FM-1, packed to 12 bits (their values).
 * SPDX-License-Identifier: BSD-2-Clause */
#include <stdio.h>
#include "../src/sid.c"

int main(void)
{
    uint32_t m, t, i;
    comb_build(SID_6581);
    comb_build(SID_8580);
    printf("/* made by tools/combtab.c from src/sid.c's comb_build: do not edit. 12 bits an entry: entries\n"
           " * 2k, 2k + 1 in bytes 3k (low 8 of 2k), 3k + 1 (high 4 of 2k, low 4 of 2k + 1), 3k + 2 */\n");
    printf("static const uint8_t comb_tab12[2][4][6144] = {\n");
    for (m = 0; m < 2; m++) {
        printf(" {\n");
        for (t = 0; t < 4; t++) {
            printf("  {");
            for (i = 0; i < 4096; i += 2) {
                uint32_t a = comb_tab[m][t][i], b = comb_tab[m][t][i + 1];
                printf("%s%u,%u,%u,", i % 16 ? "" : "\n   ", a & 0xffu, (a >> 8) | (b & 15u) << 4, b >> 4);
            }
            printf("\n  },\n");
        }
        printf(" },\n");
    }
    printf("};\nstatic const uint8_t comb_ready[2] = {1, 1};\n");
    return 0;
}
