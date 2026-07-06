/* main.c — snes-lzss demo: both decode modes of map_lz.h doing the job each
 * one exists for, on real level data.
 *
 *   VRAM-direct mode  (map_lz_load_vram, MAP_LZ_ENABLE_VRAM)
 *       The terrain tile GRAPHICS (341 4bpp tiles, 10,912 B raw / 1,179 B
 *       compressed) decode straight into VRAM through the $2118/$2119 data
 *       port. Write-only data — the CPU never reads tiles back — so it only
 *       costs the 4,096-byte g_lz_window ring, not a full-size WRAM shadow.
 *
 *   WRAM mode  (map_lz_load — the default)
 *       The level TILEMAP (240x28 tiles, 13,440 B raw / 1,167 B compressed)
 *       decodes into a WRAM shadow at $7E:2000. This one MUST stay
 *       CPU-readable: the whole 240-column level cannot fit the 64-column
 *       hardware tilemap, so as you scroll, columns are DMA-streamed from the
 *       shadow into VRAM on the fly. The shadow IS what makes a level larger
 *       than the hardware map possible — that is why the WRAM mode exists.
 *
 * Controls: D-PAD left/right pans across the full 1,920-pixel level (clamped
 * to the map bounds). The map is exactly one screen tall (28 tiles = 224 px),
 * so there is nothing to scroll vertically.
 *
 * How the column streaming works
 * ------------------------------
 * The shadow tilemap is stored COLUMN-MAJOR: column c's 28 words start at
 * byte offset c*28*2. The hardware BG1 map is 64x32 tiles (two side-by-side
 * 32x32 screens: columns 0..31 at word $6000, columns 32..63 at $6400), and
 * the PPU wraps it every 512 px. We keep a 64-column window of the level
 * resident: world column c lives in hardware slot c & 63. Writing one column
 * is a single 56-byte DMA from the shadow ($7E) to the VRAM port with
 * VMAIN=$81 (word increment 32 = one map row), which walks straight DOWN the
 * 32-wide screen. Each frame the camera moves, the columns entering the
 * window on the leading edge are streamed over the slots leaving it on the
 * trailing edge (at 2 px/frame that is at most one 56-byte DMA per frame,
 * done during vblank). BG1HOFS is just the camera x — the 64-wide map wraps
 * naturally.
 */
#include <stdint.h>

/* ---- assets (data.s / assets/) ------------------------------------------- */
extern const unsigned char demo_map_lz[], demo_map_lz_end[];
extern const unsigned char demo_chr_lz[], demo_chr_lz_end[];

#define MAP_COLS   240              /* level width in 8px tile columns  */
#define MAP_ROWS   28               /* level height in 8px tile rows    */
#define MAP_RAWLEN (MAP_COLS * MAP_ROWS * 2)
#define CHR_RAWLEN 10912u           /* 341 4bpp tiles * 32 bytes        */
#define ROM_BANK   0x80             /* blobs' DMA bank: the LoROM FastROM
                                       mirror of the single 32KB bank   */

/* wild_ride BG palette: 10 BGR555 colours for CGRAM 0..9 (colour 0 = backdrop).
 * Sample art from the SNES game the codec was written for. */
static const uint8_t bg_pal[20] = {
    4,21, 120,42, 113,21, 168,12, 113,21, 181,86, 140,49, 35,103, 63,35, 189,123,
};

/* ---- VRAM layout ----------------------------------------------------------
 * word $2000  BG1 character base (BG12NBA=$02). Tile 0 is left blank (sky);
 *             the terrain CHR decodes to $2010 = tile 1, matching the
 *             tilemap's 1-based tile indices.
 * word $6000  BG1 tilemap, 64x32 (BG1SC=$61): screen 0 at $6000, screen 1
 *             at $6400.
 * ------------------------------------------------------------------------- */
#define CHR_BASE_W 0x2000
#define MAP_BASE_W 0x6000

/* ---- registers ------------------------------------------------------------ */
#define R8(a)  (*(volatile uint8_t  *)(a))
#define R16(a) (*(volatile uint16_t *)(a))
#define INIDISP  R8(0x2100)
#define BGMODE   R8(0x2105)
#define BG1SC    R8(0x2107)
#define BG12NBA  R8(0x210B)
#define BG1HOFS  R8(0x210D)
#define BG1VOFS  R8(0x210E)
#define VMAIN    R8(0x2115)
#define VMADD    R16(0x2116)
#define VMDATAL  R8(0x2118)
#define VMDATAH  R8(0x2119)
#define CGADD    R8(0x2121)
#define CGDATA   R8(0x2122)
#define TM       R8(0x212C)
#define NMITIMEN R8(0x4200)
#define MDMAEN   R8(0x420B)
#define HVBJOY   R8(0x4212)
#define JOY1H    R8(0x4219)
#define DMAP0    R8(0x4300)
#define BBAD0    R8(0x4301)
#define A1T0     R16(0x4302)
#define A1B0     R8(0x4304)
#define DAS0     R16(0x4305)

/* ---- map_lz.h integration ------------------------------------------------- */
#define LZ_CODE                       /* single flat bank: no section tag */

static void wram_port_at(uint16_t addr);
static void dma_rom_to_wram(const unsigned char *blob, uint8_t rom_bank,
                            uint16_t wram_addr, uint16_t len);

uint8_t g_lz_scratch[1024];           /* low-WRAM DMA staging for the source  */
extern uint8_t g_map_shadow[];        /* $7E:2000 shadow (port/DMA only)      */

#define MAP_LZ_ENABLE_VRAM
#include "../map_lz.h"

uint8_t g_lz_window[LZ_RING];         /* VRAM mode's 4KB ring (low WRAM)      */

/* The decoded-tilemap shadow. Lives in extended WRAM ($7E:2000, demo.ld) and
 * is NEVER indexed as a C array — the decoder writes it through the $2180
 * WRAM port and the column streamer reads it by DMA, both with an explicit
 * $7E bank. (llvm-mos would near-address a bank-$7E symbol under the running
 * DBR — $80 here — and hit the wrong bank.) */
__attribute__((section(".bank_7e_bss"))) uint8_t g_map_shadow[MAP_RAWLEN];

/* Set the $2180 WRAM data-port address (WMADDH=0 -> bank $7E). */
static void wram_port_at(uint16_t addr) {
    R8(0x2181) = (uint8_t)(addr & 0xFF);
    R8(0x2182) = (uint8_t)(addr >> 8);
    R8(0x2183) = 0x00;
}

/* DMA one chunk of a compressed blob from ROM into g_lz_scratch (via $2180). */
static void dma_rom_to_wram(const unsigned char *blob, uint8_t rom_bank,
                            uint16_t wram_addr, uint16_t len) {
    wram_port_at(wram_addr);
    DMAP0 = 0x00;                     /* CPU->PPU, 1 register, increment A */
    BBAD0 = 0x80;                     /* B-bus $2180 (WMDATA)              */
    A1T0  = (uint16_t)blob;           /* 16-bit half; bank passed explicitly */
    A1B0  = rom_bank;
    DAS0  = len;
    MDMAEN = 0x01;
}

/* ---- column streaming (the WRAM mode's payoff) ----------------------------
 * DMA world column `col` (28 words) from the shadow into its hardware slot.
 * VMAIN=$81: the word address steps +32 after each word, walking down one
 * 32-tile-wide screen column. Call only during vblank / forced blank.       */
static void stream_col(uint16_t col) {
    uint16_t slot = col & 63;
    VMAIN = 0x81;
    VMADD = (slot < 32) ? (uint16_t)(MAP_BASE_W + slot)
                        : (uint16_t)(MAP_BASE_W + 0x400 + (slot - 32));
    DMAP0 = 0x01;                     /* 2 registers ($2118/$2119), incr A */
    BBAD0 = 0x18;
    A1T0  = (uint16_t)(0x2000 + col * (MAP_ROWS * 2));  /* shadow, in $7E  */
    A1B0  = 0x7E;
    DAS0  = MAP_ROWS * 2;
    MDMAEN = 0x01;
}

/* ---- verification hooks (read by the headless Mesen checker) -------------- */
volatile uint16_t g_state;            /* 0xA11E once the main loop is live */
volatile uint16_t g_camx;             /* current camera x (mirror)         */
volatile uint16_t g_winlo;            /* left edge of the streamed window  */

/* ---- helpers --------------------------------------------------------------- */
static void wait_vblank(void) {       /* wait for the NEXT vblank start edge */
    while (HVBJOY & 0x80) { }
    while (!(HVBJOY & 0x80)) { }
}

#define SPEED      2                  /* pan speed, px/frame                  */
#define WIN_MARGIN 8                  /* columns kept behind the camera edge  */
#define CAM_MAX    (MAP_COLS * 8 - 256)          /* 1664: right scroll bound  */
#define WIN_MAX    ((uint16_t)(MAP_COLS - 64))   /* 176:  last window origin  */

#ifndef BOOT_CAMX                     /* initial camera x; override with e.g. */
#define BOOT_CAMX 0                   /* -DBOOT_CAMX=1296 (the README shot)   */
#endif

/* Window origin for a given camera x (shared by boot prime + the scroller). */
static uint16_t win_for(uint16_t cx) {
    uint16_t want = cx >> 3;
    want = (want > WIN_MARGIN) ? (uint16_t)(want - WIN_MARGIN) : 0;
    return (want > WIN_MAX) ? WIN_MAX : want;
}

int main(void) {
    uint16_t i, camx = BOOT_CAMX, winLo;

    INIDISP = 0x80;                   /* forced blank: VRAM/CGRAM writable  */

    /* Full PPU register init — canonical SNES startup table.
     * Hardware /RESET does NOT zero PPU registers ($2100-$213F); they hold
     * whatever was written by the last program (or are undefined on power-on).
     * Writing them all here (under forced blank) gives a deterministic slate.
     *
     * INIDISP ($2100) is already set above; BGMODE/BG1SC/BG12NBA/TM and the
     * VRAM/CGRAM/OAM addresses are set further below.  Everything else that
     * could corrupt a plain BG1 picture is silenced here.                   */
    R8(0x420C) = 0x00;  /* HDMAEN   – no stray HDMA channels                */
    R8(0x2133) = 0x00;  /* SETINI   – no interlace / overscan / hi-res / EXTBG */
    R8(0x2101) = 0x00;  /* OBSEL    – OBJ tile base $0000, 8x8 size         */
    R8(0x2102) = 0x00;  /* OAMADDL  – OAM address low                       */
    R8(0x2103) = 0x00;  /* OAMADDH  – OAM address high + priority           */
    R8(0x2106) = 0x00;  /* MOSAIC   – no mosaic                             */
    R8(0x2108) = 0x00;  /* BG2SC    – BG2 tilemap addr                      */
    R8(0x2109) = 0x00;  /* BG3SC    – BG3 tilemap addr                      */
    R8(0x210A) = 0x00;  /* BG4SC    – BG4 tilemap addr                      */
    R8(0x210C) = 0x00;  /* BG34NBA  – BG3/4 character addr                  */
    R8(0x210F) = 0; R8(0x210F) = 0;   /* BG2HOFS (double-write: lo, hi)     */
    R8(0x2110) = 0; R8(0x2110) = 0;   /* BG2VOFS                            */
    R8(0x2111) = 0; R8(0x2111) = 0;   /* BG3HOFS                            */
    R8(0x2112) = 0; R8(0x2112) = 0;   /* BG3VOFS                            */
    R8(0x2113) = 0; R8(0x2113) = 0;   /* BG4HOFS                            */
    R8(0x2114) = 0; R8(0x2114) = 0;   /* BG4VOFS                            */
    R8(0x2123) = 0x00;  /* W12SEL   – window masks for BG1/BG2 (all off)    */
    R8(0x2124) = 0x00;  /* W34SEL   – window masks for BG3/BG4 (all off)    */
    R8(0x2125) = 0x00;  /* WOBJSEL  – window masks for OBJ/colour (all off) */
    R8(0x2126) = 0x00;  /* WH0      – window 1 left edge                    */
    R8(0x2127) = 0xFF;  /* WH1      – window 1 right edge (full width)      */
    R8(0x2128) = 0x00;  /* WH2      – window 2 left edge                    */
    R8(0x2129) = 0xFF;  /* WH3      – window 2 right edge (full width)      */
    R8(0x212A) = 0x00;  /* WBGLOG   – window BG logic                       */
    R8(0x212B) = 0x00;  /* WOBJLOG  – window OBJ/colour logic               */
    R8(0x212D) = 0x00;  /* TS       – sub-screen layer enable (none)        */
    R8(0x212E) = 0x00;  /* TMW      – main-screen window mask (off)         */
    R8(0x212F) = 0x00;  /* TSW      – sub-screen window mask (off)          */
    R8(0x2130) = 0x30;  /* CGWSEL   – colour math always prevented (bits 5:4=11) */
    R8(0x2131) = 0x00;  /* CGADSUB  – no layers in colour math              */
    R8(0x2132) = 0xE0;  /* COLDATA  – fixed sub-screen colour = black (R+G+B @ 0) */

    BGMODE  = 0x01;                   /* Mode 1                             */
    BG1SC   = 0x61;                   /* map base word $6000, size 64x32    */
    BG12NBA = 0x02;                   /* BG1 char base word $2000           */
    TM      = 0x01;                   /* main screen: BG1 only              */
    BG1HOFS = 0; BG1HOFS = 0;
    BG1VOFS = 0; BG1VOFS = 0;

    /* Blank tile 0 + clear the 64x32 hardware tilemap. */
    VMAIN = 0x80;
    VMADD = CHR_BASE_W;
    for (i = 0; i < 16; i++)    { VMDATAL = 0; VMDATAH = 0; }
    VMADD = MAP_BASE_W;
    for (i = 0; i < 0x800; i++) { VMDATAL = 0; VMDATAH = 0; }

    /* 1. VRAM-direct decode: terrain CHR -> tile 1 onward. Write-only data,
     *    so no shadow — just the 4KB ring. (We are under forced blank.)     */
    map_lz_load_vram(demo_chr_lz, ROM_BANK,
                     (uint16_t)(demo_chr_lz_end - demo_chr_lz),
                     CHR_RAWLEN, CHR_BASE_W + 16);

    /* 2. BG palette -> CGRAM 0..9. */
    CGADD = 0;
    for (i = 0; i < sizeof bg_pal; i++) CGDATA = bg_pal[i];

    /* 3. WRAM decode: tilemap -> $7E:2000 shadow, CPU-readable (well,
     *    DMA-readable) for the lifetime of the level.                       */
    map_lz_load(demo_map_lz, ROM_BANK,
                (uint16_t)(demo_map_lz_end - demo_map_lz), MAP_RAWLEN);

    /* 4. Prime the view: stream the boot window's 64 columns into the
     *    hardware map and point the scroll at the camera.                   */
    winLo = win_for(camx);
    for (i = 0; i < 64; i++) stream_col((uint16_t)(winLo + i));
    BG1HOFS = (uint8_t)camx; BG1HOFS = (uint8_t)(camx >> 8);

    NMITIMEN = 0x01;                  /* auto-joypad on (no NMI needed)     */
    INIDISP  = 0x0F;                  /* display on, full brightness        */
    g_state  = 0xA11E;

    for (;;) {
        uint16_t want, nq = 0, q[4];
        uint8_t pad;

        /* -- active display: read the pad, move the camera, plan columns -- */
        while (HVBJOY & 0x01) { }     /* auto-joypad read in progress       */
        pad = JOY1H;                  /* B Y Sel St Up Dn Lt Rt (bits 7..0) */

        if ((pad & 0x01) && camx < CAM_MAX) camx += SPEED;   /* right */
        if ((pad & 0x02) && camx > 0)       camx -= SPEED;   /* left  */

        /* Slide the 64-column window with the camera (clamped to the map).
         * Entering columns are queued; their DMA waits for vblank.          */
        want = win_for(camx);
        while (winLo < want && nq < 4) { q[nq++] = (uint16_t)(winLo + 64); winLo++; }
        while (winLo > want && nq < 4) { winLo--; q[nq++] = winLo; }

        /* -- vblank: stream queued columns, then set the scroll ----------- */
        wait_vblank();
        for (i = 0; i < nq; i++) stream_col(q[i]);
        BG1HOFS = (uint8_t)camx; BG1HOFS = (uint8_t)(camx >> 8);

        g_camx = camx; g_winlo = winLo;
    }
}
