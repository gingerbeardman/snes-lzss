/* test_roundtrip.c — Host round-trip gate for the LZSS codec.
 *
 * For each file pair <prefix>.raw (original) + <prefix>.lz (compressed), this
 * program feeds the compressed bytes to the C decoder in map_lz.h and asserts
 * the output is byte-identical to the original.  The same decoder code runs on
 * the SNES; passing here proves the encoder/decoder pair is correct.
 *
 * Build and run (against the shipped sample):
 *   cc -std=c99 -o test_roundtrip test_roundtrip.c
 *   ./test_roundtrip testdata wild_ride
 *
 * Or compress a file yourself and test it:
 *   python3 lz_compress.py encode myfile.bin testdata/myfile.lz
 *   cp myfile.bin testdata/myfile.raw
 *   ./test_roundtrip testdata myfile
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_LZ_ENABLE_VRAM    /* also compile + test the VRAM-direct ring path */
#include "map_lz.h"

/* Required definition for VRAM-direct mode (the WRAM sliding-window ring). On
 * the SNES this lives in low WRAM; on host it is a plain array. */
uint8_t g_lz_window[LZ_RING];

static uint8_t *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*n ? (size_t)*n : 1);
    if (!buf) { fprintf(stderr, "out of memory\n"); exit(2); }
    if (fread(buf, 1, (size_t)*n, f) != (size_t)*n) {
        fprintf(stderr, "short read %s\n", path); exit(2);
    }
    fclose(f);
    return buf;
}

/* ---------------------------------------------------------------------------
 * Crafted edge-case gate for the VRAM-direct ring buffer.
 *
 * Hand-emits an LZSS stream (bit-exact to lz_compress.py's format) while
 * building the expected output by the format's own definition (out[i]=out[i-off]).
 * Deliberately exercises: off == 4096 (the max back-reference / ring-span
 * boundary, both ring-aligned and not), max-length 273 matches, and overlapping
 * matches (off 1 and 3) that wrap the 4096-byte ring across k*4096 boundaries.
 * Then decodes via BOTH host paths (default WRAM-shadow + VRAM ring) and asserts
 * both equal the expected buffer.
 * ------------------------------------------------------------------------- */
#define CBUF 20000
static uint8_t g_comp[CBUF];
static uint8_t g_exp[CBUF];
static size_t  g_clen, g_elen;
static long    g_flag_pos; static int g_flag_bit;

static void c_tok(int is_lit) {
    if (g_flag_bit == 8) { g_flag_pos = (long)g_clen; g_comp[g_clen++] = 0; g_flag_bit = 0; }
    if (is_lit) g_comp[g_flag_pos] |= (uint8_t)(1 << g_flag_bit);
    g_flag_bit++;
}
static void do_lit(uint8_t b) {          /* emit literal into stream + expected */
    c_tok(1); g_comp[g_clen++] = b;
    g_exp[g_elen++] = b;
}
static void do_match(int off, int len) { /* emit match into stream + expected */
    int om1 = off - 1, k;
    c_tok(0);
    g_comp[g_clen++] = (uint8_t)(om1 & 0xFF);
    if (len <= 17) {
        g_comp[g_clen++] = (uint8_t)(((om1 >> 8) & 0x0F) | ((len - 3) << 4));
    } else {
        g_comp[g_clen++] = (uint8_t)(((om1 >> 8) & 0x0F) | 0xF0);
        g_comp[g_clen++] = (uint8_t)(len - 18);
    }
    for (k = 0; k < len; k++) { g_exp[g_elen] = g_exp[g_elen - off]; g_elen++; }
}

static int run_crafted(void) {
    g_clen = g_elen = 0; g_flag_pos = -1; g_flag_bit = 8;

    /* Phase 1: 4096 distinct-ish literals fill the whole window. */
    for (int i = 0; i < 4096; i++) do_lit((uint8_t)((i * 7 + 13) & 0xFF));
    /* Phase 2: off == 4096 (ring-aligned: src slot == dst slot), max len 273. */
    do_match(4096, 273);
    /* Phase 3: literals up to just before the 8192 ring boundary. */
    while (g_elen < 8100) do_lit((uint8_t)((g_elen * 3 + 1) & 0xFF));
    /* Phase 4: off==1 RLE (period-1 overlap) crossing the 8192 boundary. */
    do_match(1, 200);
    /* Phase 5: off==3 (period-3 overlap), long, also past the boundary. */
    do_match(3, 273);
    /* Phase 6: off==4096 again but NOT ring-aligned (dst slot != src slot). */
    do_lit(0x5A);
    do_match(4096, 100);
    /* Phase 7: a couple of short matches + tail literal for an even length. */
    do_match(2, 3);
    do_lit(0xC3);
    if (g_elen & 1) do_lit(0x00);   /* keep rawlen even (VRAM-mode contract) */

    uint16_t rawlen = (uint16_t)g_elen;
    static uint8_t out_wram[CBUF], out_vram[CBUF];
    memset(out_wram, 0xA5, sizeof out_wram);
    memset(out_vram, 0x5A, sizeof out_vram);

    map_lz_load_host(g_comp, out_wram, rawlen);        /* default WRAM-shadow path */
    map_lz_load_vram_host(g_comp, out_vram, rawlen);   /* VRAM-direct ring path    */

    int ok_wram = (memcmp(out_wram, g_exp, rawlen) == 0);
    int ok_vram = (memcmp(out_vram, g_exp, rawlen) == 0);
    int ok_agree = (memcmp(out_wram, out_vram, rawlen) == 0);

    printf("  %-20s raw %6u  lz %6zu  (%5.1f%%)  WRAM %s / VRAM-ring %s\n",
           "crafted-edges", rawlen, g_clen, 100.0 * g_clen / rawlen,
           ok_wram ? "OK" : "MISMATCH", ok_vram ? "OK" : "MISMATCH");
    if (!ok_wram || !ok_vram || !ok_agree) {
        for (uint16_t i = 0; i < rawlen; i++) {
            if (out_vram[i] != g_exp[i]) {
                printf("    VRAM first diff at %u: got %02X exp %02X\n", i, out_vram[i], g_exp[i]);
                break;
            }
        }
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <datadir> <prefix> [<prefix>...]\n", argv[0]);
        return 2;
    }
    int fails = 0;
    long tot_raw = 0, tot_lz = 0;

    for (int a = 2; a < argc; a++) {
        char path[1024];
        long rawlen, lzlen;

        snprintf(path, sizeof path, "%s/%s.raw", argv[1], argv[a]);
        uint8_t *raw = slurp(path, &rawlen);

        snprintf(path, sizeof path, "%s/%s.lz", argv[1], argv[a]);
        uint8_t *lz = slurp(path, &lzlen);

        if (rawlen > 0xFFFF) {
            fprintf(stderr, "%s: rawlen %ld exceeds uint16 (65535)\n", argv[a], rawlen);
            return 2;
        }

        uint8_t *out = malloc((size_t)rawlen);
        if (!out) { fprintf(stderr, "out of memory\n"); return 2; }
        memset(out, 0xA5, (size_t)rawlen);   /* poison: catch unwritten bytes */

        map_lz_load_host(lz, out, (uint16_t)rawlen);

        /* Also exercise the VRAM-direct ring path on real sample data. */
        uint8_t *outv = malloc((size_t)rawlen);
        if (!outv) { fprintf(stderr, "out of memory\n"); return 2; }
        memset(outv, 0x5A, (size_t)rawlen);
        map_lz_load_vram_host(lz, outv, (uint16_t)rawlen);
        int ok_v = (memcmp(outv, raw, (size_t)rawlen) == 0);

        int ok = (memcmp(out, raw, (size_t)rawlen) == 0) && ok_v;
        printf("  %-20s raw %6ld  lz %6ld  (%5.1f%%)  %s%s\n",
               argv[a], rawlen, lzlen, 100.0 * lzlen / rawlen,
               ok ? "OK" : "MISMATCH", ok_v ? "" : " (VRAM-ring MISMATCH)");
        free(outv);

        if (!ok) {
            fails++;
            for (long i = 0; i < rawlen; i++) {
                if (out[i] != raw[i]) {
                    printf("    first diff at byte %ld: decoded %02X, expected %02X\n",
                           i, out[i], raw[i]);
                    break;
                }
            }
        }

        tot_raw += rawlen;
        tot_lz  += lzlen;
        free(raw); free(lz); free(out);
    }

    fails += run_crafted();

    printf("  %-20s raw %6ld  lz %6ld  (%5.1f%%)  %s\n",
           "TOTAL", tot_raw, tot_lz, 100.0 * tot_lz / tot_raw,
           fails ? "FAIL" : "ALL OK");
    return fails ? 1 : 0;
}
