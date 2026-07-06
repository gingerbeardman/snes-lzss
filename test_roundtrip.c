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

#include "map_lz.h"

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

        int ok = (memcmp(out, raw, (size_t)rawlen) == 0);
        printf("  %-20s raw %6ld  lz %6ld  (%5.1f%%)  %s\n",
               argv[a], rawlen, lzlen, 100.0 * lzlen / rawlen,
               ok ? "OK" : "MISMATCH");

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

    printf("  %-20s raw %6ld  lz %6ld  (%5.1f%%)  %s\n",
           "TOTAL", tot_raw, tot_lz, 100.0 * tot_lz / tot_raw,
           fails ? "FAIL" : "ALL OK");
    return fails ? 1 : 0;
}
