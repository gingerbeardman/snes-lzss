# snes-lzss

A compact LZSS compressor (Python) and decoder (C header) for SNES tilemap data — and general byte streams up to 65535 bytes. Originally written for a SNES homebrew game to compress seven test BG tilemaps from 81,376 bytes down to 4,163 bytes (5.1% of original size). Beat deflate and huffmunch in a bake-off on this data and requires zero runtime RAM for a ring buffer — the sliding window is the decoded output itself.

The shipped sample (`testdata/wild_ride.*`) is one of those seven maps — a 240×28-tile SNES BG1 tilemap (13,440 bytes raw → 1,167 bytes compressed, 8.7%):

![wild_ride level tilemap](docs/wild_ride_tilemap.png)

## Files

| File | Purpose |
|------|---------|
| `lz_compress.py` | Compressor CLI + importable library (Python 3, no dependencies) |
| `map_lz.h` | Decoder header — compiles on host (plain C99) and on SNES (llvm-mos/65816) |
| `test_roundtrip.c` | Host round-trip test (`cc -std=c99 -o test_roundtrip test_roundtrip.c`) |
| `testdata/wild_ride.raw` | Sample: a real SNES BG tilemap (13,440 bytes) |
| `testdata/wild_ride.lz` | The same tilemap compressed (1,167 bytes) |
| `docs/wild_ride_tilemap.png` | The sample tilemap rendered to PNG |

## Quick start

```sh
# Compress a file
python3 lz_compress.py encode input.bin output.lz

# Self-check (compress + decompress + compare)
python3 lz_compress.py check input.bin

# Build and run the C round-trip test on the shipped sample
cc -std=c99 -o test_roundtrip test_roundtrip.c
./test_roundtrip testdata wild_ride
```

## Stream format

```
┌──────────┬────────────────── 8 tokens (variable length) ──────────────────┐
│ flag byte│ token₀  token₁  …  token₇                                      │
└──────────┴─────────────────────────────────────────────────────────────────┘
  flag bit n = 1 → token n is a LITERAL (1 byte)
  flag bit n = 0 → token n is a MATCH   (2 or 3 bytes)
```

### Literal token
One raw byte copied to output.

### Match token (2 bytes, or 3 if length extension needed)
```
b0 = lo8(off - 1)
b1 = hi4(off - 1) | (lenN << 4)
```

- `off` — distance back into the decoded output, 1..4096  
  (the sliding window **is** the output; no separate ring buffer needed)
- `lenN` 0..14 → `len = lenN + 3` (3..17 bytes)
- `lenN` == 15 → a third byte follows: `len = 18 + ext_byte` (18..273 bytes)

Overlapping matches (`off < len`) are valid and replicate the period-`off` pattern — useful for runs of repeating tiles.

## Measured results

Tested on seven SNES BG tilemaps from a real game (2-byte tile-index words, column-major):

| Level | Raw (bytes) | Compressed | Ratio |
|-------|-------------|------------|-------|
| wild_ride (shipped sample) | 13440 | 1167 | 8.7% |
| obstacles | 13440 | 448 | 3.3% |
| mechanisms | 8512 | 211 | 2.5% |
| rolling_hills | 8064 | 919 | 11.4% |
| contraptions | 6720 | 183 | 2.7% |
| kitchen_sink | 14336 | 904 | 6.3% |
| deep_descent | 16864 | 331 | 2.0% |
| **TOTAL** | **81376** | **4163** | **5.1%** |

All seven were confirmed byte-perfect through the C decoder by the round-trip test; only `wild_ride` ships in this repo as sample data. The codec was chosen over zlib/deflate and huffmunch after a bake-off on this dataset; the other algorithms achieved similar or worse ratios while requiring more ROM or RAM for the decoder.

## Encoder algorithm

Greedy-with-lazy LZSS. Hash chains on 3-byte prefixes with exhaustive search within the 4096-byte window. Lazy evaluation: after finding a match at position `i`, the encoder checks whether a match starting at `i+1` is longer; if so, it emits a literal for `i` and re-tries from `i+1`. This typically recovers 2–5% over pure greedy on structured data.

## Decoder integration

### Host (any C99 project)

```c
#include <stdint.h>
#include <string.h>
#include "map_lz.h"

uint8_t out[RAW_LEN];
map_lz_load_host(compressed_bytes, out, RAW_LEN);
```

### SNES / llvm-mos (65816)

The decoder reads the compressed source from a low-WRAM staging buffer (populated via DMA from ROM), and writes the output through the SNES $2180 WRAM data port (auto-increment). This avoids needing a 4096-byte ring buffer in RAM and sidesteps the llvm-mos "near-addressing" issue when `DBR != $00`.

**Required definitions** (before `#include "map_lz.h"`):

```c
// Place decoder functions in your chosen ROM bank (load-time only, cold code).
// Example: bank $0C
#define LZ_CODE __attribute__((section(".text.bank0c")))

// Set the WRAM address port ($2181 lo, $2182 hi).
void wram_port_at(uint16_t addr);       // typically a 2-cycle helper

// DMA one chunk of compressed data from ROM into low WRAM.
void dma_rom_to_wram(const unsigned char *blob, uint8_t rom_bank,
                     uint16_t wram_addr, uint16_t len);

// Staging buffer in low WRAM (size ≥ largest single compressed blob).
// 512 bytes is enough for these tilemaps; 1024 is comfortable.
extern uint8_t g_lz_scratch[512];      // low WRAM, 16-bit address

// Output shadow in low WRAM (size = largest raw tilemap you will decode).
extern uint8_t g_map_shadow[RAW_MAX];  // low WRAM, 16-bit address
```

**Call site:**

```c
// blob     — pointer to compressed data in ROM (16-bit address in rom_bank)
// rom_bank — which ROM bank byte ($04, $05, …)
// complen  — byte length of compressed data
// rawlen   — byte length of the original (uncompressed) data
map_lz_load(blob, rom_bank, complen, rawlen);
// g_map_shadow now contains the decoded tilemap; stream it to VRAM via DMA.
```

**ROM/CPU footprint (approximate):**  
The decoder (token loop + primitives) is roughly 200–400 bytes of 65816 code at `-Os` with llvm-mos, depending on how aggressively the linker inlines the static helpers. It runs only at level-load time (no per-frame overhead) and fits comfortably in a dedicated 32 KB ROM bank with room to spare.

## License

MIT — see `LICENSE`.
