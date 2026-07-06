/* map_lz.h — LZSS decoder for SNES tilemaps (and general byte streams up to 64 KB).
 *
 * This header compiles in two environments:
 *
 *   Host (any C99 compiler):
 *     #include <stdint.h>
 *     #include <string.h>
 *     #include "map_lz.h"
 *
 *     uint8_t out[RAWLEN];
 *     map_lz_load_host(compressed_bytes, out, RAWLEN);
 *
 *   SNES / llvm-mos (65816):
 *     Define these before including the header (or in your project header):
 *
 *       // Tag every decoder function with the ROM bank attribute.
 *       // E.g., to place the decoder in bank $0C:
 *       #define LZ_CODE __attribute__((section(".text.bank0c")))
 *
 *       // Read one byte from the compressed source (called lz_src_next).
 *       // The built-in SNES implementation reads from a DMA-staged scratch buffer
 *       // in low WRAM; see the SNES section below.
 *
 *     Then define before including:
 *       void  wram_port_at(uint16_t addr);  // set $2181/$2182 WRAM address port
 *       void  dma_rom_to_wram(const unsigned char *blob, uint8_t bank,
 *                             uint16_t wram_addr, uint16_t len);
 *       extern uint8_t g_lz_scratch[];      // low-WRAM staging buffer
 *       extern uint8_t g_map_shadow[];      // low-WRAM decoded-output shadow
 *       (see Integration notes in README.md for sizes and declarations)
 *
 *     Then call:
 *       map_lz_load(blob_ptr, rom_bank, compressed_len, raw_len);
 *
 * Stream format
 * -------------
 * A flag byte precedes every group of 8 tokens (bit 0 = first token;
 * 1 = literal, 0 = match), followed by the token bytes:
 *
 *   literal:  1 raw byte.
 *
 *   match:    b0 = lo8(off - 1)
 *             b1 = hi4(off - 1) | (lenN << 4)
 *             off  = distance back into the OUTPUT, 1..4096 (the sliding window
 *                    IS the decoded output — no separate ring buffer needed)
 *             lenN 0..14 -> len 3..17
 *             lenN 15    -> one extra byte follows; len = 18 + ext (18..273)
 *
 * Matches may overlap (off < len); the decoder replicates the period-off pattern
 * by clamping each copy to off bytes (see lz_read_at / lz_write below).
 */

#ifndef MAP_LZ_H
#define MAP_LZ_H

#ifndef LZ_CODE
#define LZ_CODE          /* define to e.g. __attribute__((section(".text.bank0c"))) */
#endif

#define LZ_TMP 32        /* match-copy staging chunk (bytes); must be <= off max = 4096 */

static uint16_t lz_o;           /* write head: bytes decoded so far */
static uint8_t  lz_tmp[LZ_TMP];

/* =========================================================================
 * SNES (llvm-mos / __mos__) implementation
 * =========================================================================
 * Source data is staged from ROM into a low-WRAM scratch buffer via DMA
 * (so compressed blobs larger than the scratch decode correctly).  Output
 * goes to a WRAM shadow via the $2180 WRAM data register (auto-increment),
 * so the decoder never C-addresses the shadow (avoiding llvm-mos near-addr
 * confusion when DBR != $00).
 * ========================================================================= */
#ifdef __mos__

static const unsigned char *lz_blob;   /* pointer to compressed blob (16-bit addr in lz_bank) */
static uint8_t  lz_bank;               /* ROM bank that holds the blob */
static uint16_t lz_clen;               /* compressed length in bytes */
static uint16_t lz_soff;               /* bytes of blob staged so far */
static uint16_t lz_si, lz_silen;       /* scratch read cursor / fill level */

LZ_CODE static uint8_t lz_src_next(void) {
    if (lz_si == lz_silen) {
        /* Refill g_lz_scratch from ROM via DMA. */
        uint16_t n = (uint16_t)(lz_clen - lz_soff);
        if (n > sizeof g_lz_scratch) n = sizeof g_lz_scratch;
        dma_rom_to_wram(lz_blob + lz_soff, lz_bank, (uint16_t)g_lz_scratch, n);
        lz_soff += n; lz_silen = n; lz_si = 0;
    }
    return g_lz_scratch[lz_si++];
}

/* Read shadow[pos..pos+n) into lz_tmp via the WRAM port (auto-increment). */
LZ_CODE static void lz_read_at(uint16_t pos, uint8_t n) {
    uint8_t i;
    wram_port_at((uint16_t)((uint16_t)g_map_shadow + pos));
    for (i = 0; i < n; i++) lz_tmp[i] = *(volatile uint8_t *)0x2180;
}

/* Write src[0..n) to shadow[lz_o..) via the WRAM port. */
LZ_CODE static void lz_write(const uint8_t *src, uint8_t n) {
    uint8_t i;
    wram_port_at((uint16_t)((uint16_t)g_map_shadow + lz_o));
    for (i = 0; i < n; i++) *(volatile uint8_t *)0x2180 = src[i];
    lz_o += n;
}

#else
/* =========================================================================
 * Host (plain C) implementation — direct memory reads/writes.
 * ========================================================================= */
static const uint8_t *lz_blob;
static uint16_t       lz_si;
static uint8_t       *lz_out;

static uint8_t lz_src_next(void) { return lz_blob[lz_si++]; }
static void lz_read_at(uint16_t pos, uint8_t n) { memcpy(lz_tmp, lz_out + pos, n); }
static void lz_write(const uint8_t *src, uint8_t n) {
    memcpy(lz_out + lz_o, src, n);
    lz_o += n;
}
#endif /* __mos__ */

/* =========================================================================
 * Shared token loop — identical on SNES and host.
 * ========================================================================= */
LZ_CODE static void map_lz_decode(uint16_t rawlen) {
    uint8_t flags = 0, nbits = 0;
    lz_o = 0;
    while (lz_o < rawlen) {
        if (!nbits) { flags = lz_src_next(); nbits = 8; }
        if (flags & 1) {
            uint8_t b = lz_src_next();
            lz_write(&b, 1);
        } else {
            uint8_t b0 = lz_src_next(), b1 = lz_src_next();
            uint16_t off = (uint16_t)(((uint16_t)(b1 & 0x0F) << 8) | b0) + 1;
            uint16_t len = (uint16_t)(b1 >> 4);
            len = (len == 15)
                ? (uint16_t)(18 + lz_src_next())
                : (uint16_t)(len + 3);
            while (len) {
                uint16_t n = len;
                if (n > off)    n = off;    /* clamp to period — overlap-safe copy */
                if (n > LZ_TMP) n = LZ_TMP;
                lz_read_at((uint16_t)(lz_o - off), (uint8_t)n);
                lz_write(lz_tmp, (uint8_t)n);
                len -= n;
            }
        }
        flags >>= 1; nbits--;
    }
}

/* =========================================================================
 * Public entry points
 * ========================================================================= */

#ifdef __mos__
/**
 * Decode a LZSS-compressed blob from ROM into the WRAM shadow.
 *
 * @param blob      Pointer to compressed data (16-bit address in rom_bank).
 * @param rom_bank  ROM bank byte ($04..$0B, etc.) where blob lives.
 * @param complen   Length of compressed data in bytes.
 * @param rawlen    Expected length of decompressed data in bytes (max 65535).
 */
LZ_CODE static void map_lz_load(const unsigned char *blob, uint8_t rom_bank,
                                 uint16_t complen, uint16_t rawlen) {
    lz_blob = blob; lz_bank = rom_bank; lz_clen = complen;
    lz_soff = 0; lz_si = 0; lz_silen = 0;
    map_lz_decode(rawlen);
}
#else
/**
 * Decode a LZSS-compressed blob into a plain memory buffer (host only).
 *
 * @param blob    Pointer to compressed data produced by lz_compress.py.
 * @param out     Output buffer of at least rawlen bytes.
 * @param rawlen  Expected length of decompressed data in bytes (max 65535).
 */
static void map_lz_load_host(const uint8_t *blob, uint8_t *out, uint16_t rawlen) {
    lz_blob = blob; lz_si = 0; lz_out = out;
    map_lz_decode(rawlen);
}
#endif

/* =========================================================================
 * VRAM-direct mode (optional) — #define MAP_LZ_ENABLE_VRAM before including.
 * =========================================================================
 * For WRITE-ONLY tile graphics, streaming the decoded bytes straight to VRAM
 * avoids the full-size WRAM shadow of the default mode (which needs RAW_MAX
 * bytes so the decoded map stays CPU-readable). Instead this mode keeps only a
 * 4096-byte sliding-window RING buffer in WRAM — the largest back-reference the
 * format allows (off 1..4096) — and every decoded byte is written to BOTH the
 * ring (for future back-references) AND the VRAM data port.
 *
 * Back-references are plain C array reads from the ring (faster than the default
 * mode's $2180 port-seek reads). The decode is byte-at-a-time, which makes
 * overlapping matches (off < len) and ring wraparound correct by construction:
 * a byte is always read from ring[(o - off) & 4095] AFTER the previous byte was
 * written, and off <= 4096 guarantees the referenced byte is still in the ring.
 *
 * REQUIRED (in addition to the default-mode SNES requirements):
 *   extern uint8_t g_lz_window[4096];   // WRAM ring, 16-bit address (DBR=$00)
 *
 * SNES VRAM port contract (set up by map_lz_load_vram):
 *   $2115 VMAIN  = 0x80  (address increments AFTER the high-byte write)
 *   $2116/$2117 VMADD    = vram_word_addr (WORD address, not byte)
 *   $2118 VMDATAL / $2119 VMDATAH  — alternating low/high writes stream words
 * The running byte count's LSB selects the port (even byte -> VMDATAL, odd byte
 * -> VMDATAH), so a full L/H pair advances the VRAM word address by one.
 *
 * MUST run under FORCED BLANK (INIDISP $2100 bit 7 = 1) or during vblank —
 * VRAM is only writable then.
 *
 * rawlen is BYTES and should be EVEN (tiles/tilemaps always are). If odd, the
 * final byte is written to VMDATAL with no matching VMDATAH, so the VRAM word
 * address does not advance for that last partial word and that word's high byte
 * keeps its previous VRAM contents. Pad to an even length to avoid this.
 * ========================================================================= */
#ifdef MAP_LZ_ENABLE_VRAM

#define LZ_RING 4096                 /* == format max back-reference (off) */
extern uint8_t g_lz_window[LZ_RING]; /* WRAM sliding-window ring (required) */

#ifdef __mos__
/* Emit one decoded byte: into the ring AND alternating VRAM data ports. */
LZ_CODE static void lzv_put(uint8_t b) {
    g_lz_window[lz_o & (LZ_RING - 1)] = b;
    if (lz_o & 1) *(volatile uint8_t *)0x2119 = b;   /* VMDATAH (odd byte) */
    else          *(volatile uint8_t *)0x2118 = b;   /* VMDATAL (even byte) */
    lz_o++;
}
#else
/* Host mirror: the ring is real (so the windowing logic is round-trip tested);
 * lz_out stands in for VRAM, capturing the exact emitted byte stream. */
LZ_CODE static void lzv_put(uint8_t b) {
    g_lz_window[lz_o & (LZ_RING - 1)] = b;
    lz_out[lz_o] = b;
    lz_o++;
}
#endif

/* Shared ring-buffer token loop (identical on SNES and host). */
LZ_CODE static void map_lz_decode_vram(uint16_t rawlen) {
    uint8_t flags = 0, nbits = 0;
    lz_o = 0;
    while (lz_o < rawlen) {
        if (!nbits) { flags = lz_src_next(); nbits = 8; }
        if (flags & 1) {
            lzv_put(lz_src_next());
        } else {
            uint8_t b0 = lz_src_next(), b1 = lz_src_next();
            uint16_t off = (uint16_t)(((uint16_t)(b1 & 0x0F) << 8) | b0) + 1;
            uint16_t len = (uint16_t)(b1 >> 4);
            len = (len == 15)
                ? (uint16_t)(18 + lz_src_next())
                : (uint16_t)(len + 3);
            while (len--) {
                /* off in 1..4096, off <= lz_o, so (lz_o-off)&4095 is the live
                 * ring slot for the referenced byte — overlap/wrap safe. */
                lzv_put(g_lz_window[(uint16_t)(lz_o - off) & (LZ_RING - 1)]);
            }
        }
        flags >>= 1; nbits--;
    }
}

#ifdef __mos__
/**
 * Decode a LZSS-compressed blob from ROM straight to VRAM (write-only graphics).
 * Keeps only a 4096-byte WRAM ring (g_lz_window) instead of a RAW_MAX shadow.
 * MUST be called under forced blank or during vblank.
 *
 * @param blob            Pointer to compressed data (16-bit addr in rom_bank).
 * @param rom_bank        ROM bank byte ($04..$0B, etc.) where blob lives.
 * @param complen         Length of compressed data in bytes.
 * @param rawlen          Expected decompressed length in bytes (even; max 65535).
 * @param vram_word_addr  Destination VRAM WORD address ($0000..$7FFF).
 */
LZ_CODE static void map_lz_load_vram(const unsigned char *blob, uint8_t rom_bank,
                                     uint16_t complen, uint16_t rawlen,
                                     uint16_t vram_word_addr) {
    lz_blob = blob; lz_bank = rom_bank; lz_clen = complen;
    lz_soff = 0; lz_si = 0; lz_silen = 0;
    *(volatile uint8_t *)0x2115 = 0x80;                       /* VMAIN: +1 word after high */
    *(volatile uint8_t *)0x2116 = (uint8_t)(vram_word_addr & 0xFF);  /* VMADDL */
    *(volatile uint8_t *)0x2117 = (uint8_t)(vram_word_addr >> 8);    /* VMADDH */
    map_lz_decode_vram(rawlen);
}
#else
/**
 * Host mirror of map_lz_load_vram: runs the identical ring-buffer decode, with
 * the emitted byte stream captured into `out` (VRAM stand-in) so the windowing
 * logic is round-trip testable off-target.
 *
 * @param blob    Pointer to compressed data produced by lz_compress.py.
 * @param out     Output buffer of at least rawlen bytes (the VRAM stand-in).
 * @param rawlen  Expected length of decompressed data in bytes (max 65535).
 */
static void map_lz_load_vram_host(const uint8_t *blob, uint8_t *out, uint16_t rawlen) {
    lz_blob = blob; lz_si = 0; lz_out = out;
    map_lz_decode_vram(rawlen);
}
#endif

#endif /* MAP_LZ_ENABLE_VRAM */

#endif /* MAP_LZ_H */
