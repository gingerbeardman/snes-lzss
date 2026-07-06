# snes-lzss demo ROM

A small SNES ROM that shows **both decode modes of `map_lz.h` doing the job
each one exists for**, on real level data:

![demo screenshot](../docs/demo_screenshot.png)

| Data | Mode | Why |
|------|------|-----|
| Terrain tile graphics (341 4bpp tiles, 10,912 B raw → 1,179 B) | **VRAM-direct** (`map_lz_load_vram`) | Write-only — decoded straight through the $2118/$2119 port; costs only the 4 KB `g_lz_window` ring, no shadow |
| Level tilemap (240×28 tiles, 13,440 B raw → 1,167 B) | **WRAM** (`map_lz_load`) | Must stay CPU-readable: the 240-column level cannot fit the 64-column hardware tilemap, so columns are DMA-streamed from the WRAM shadow into VRAM as you scroll |

## Running it

`snes-lzss-demo.smc` is prebuilt (64 KB HiROM) — load it in any SNES emulator
(Mesen 2, bsnes, snes9x, RetroArch) or on a flashcart. No toolchain needed.

**Controls:** D-PAD **left/right** pans across the full 1,920-pixel level,
clamped to the map bounds. (The map is exactly one screen tall — 28 tiles =
224 px — so there is nothing to scroll vertically.)

## What it demonstrates

The point of the demo is the scrolling. The hardware BG1 tilemap is only
64×32 tiles; the level is 240 columns wide. The demo keeps a sliding 64-column
window of the level resident in VRAM: every time the camera crosses a tile
boundary, the column entering the window is DMA'd (one 56-byte transfer,
during vblank, `VMAIN=$81` so the words walk down the screen column) **from
the CPU-readable WRAM shadow** that `map_lz_load` decoded at boot. That shadow
is exactly what the WRAM mode buys you — with a VRAM-only decoder (PVSnesLib
LZ10 style) the decoded map would be gone the moment you needed to re-stream
a column. The tile graphics take the opposite trade: nothing ever reads them
back, so `map_lz_load_vram` streams them straight to VRAM and skips the
full-size shadow entirely.

`main.c` is deliberately small and commented — it doubles as the integration
reference for both modes (the `wram_port_at` / `dma_rom_to_wram` hooks, the
`g_lz_scratch` / `g_map_shadow` / `g_lz_window` buffers, and a working column
streamer).

## Building from source

Requires the [llvm-mos](https://llvm-mos.org) SDK — current releases include
the 65816 (`-mcpu=mosw65816`) target used here. The rest of the repository
stays plain C99 + Python; only this demo needs the cross-toolchain.

```sh
# mos-common-clang on PATH, or point LLVM_MOS at the SDK root:
LLVM_MOS=/path/to/llvm-mos ./build.sh
```

Files:

| File | Purpose |
|------|---------|
| `main.c` | Demo logic: init, both decodes, column streamer, D-PAD scroll |
| `boot.s` | Reset stub (emulation mode, PBR=$C0 / DBR=$00 discipline) |
| `data.s` | `.incbin`s the two compressed blobs into ROM |
| `demo.ld` | One-64KB-HiROM-bank linker script (code low half, rodata high half) |
| `assets/wild_ride_map.lz` | Compressed tilemap (same data as `../testdata/wild_ride.lz`) |
| `assets/wild_ride_chr.lz` | Compressed terrain CHR (`lz_compress.py` output) |
| `build.sh` | Build script (needs llvm-mos) |
| `snes-lzss-demo.smc` | Prebuilt ROM |

The build was verified headless in Mesen 2 (boot state, VRAM checksum of the
VRAM-direct CHR decode, and byte-compares of freshly streamed tilemap columns
against the WRAM shadow while injecting D-PAD input).

## Art credit

The level art (terrain tiles, palette, and the wild_ride tilemap) is sample
data from the SNES homebrew game this codec was originally written for, used
here with the author's permission — the same level that ships in `testdata/`
and is pictured in the main README.
