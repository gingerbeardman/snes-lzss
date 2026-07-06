/* demo.ld — single 32KB LoROM FastROM bank linker script for the snes-lzss
 * demo (llvm-mos, -mcpu=mosw65816).
 *
 * Layout: code + rodata + header + vectors all live in one bank's $8000-$FFFF
 * window (ROM file offset $0000-$7FFF). That window is visible BOTH as bank
 * $00 (always SlowROM — where the 16-bit reset vector lands) and via the bank
 * $80 FastROM mirror. boot.s sets MEMSEL ($420D=1) and jumps into the mirror
 * with PBR=$80 / DBR=$80, so instruction fetch AND data reads (rodata, the
 * compressed blobs) run at 3.58 MHz. Banks $80-$BF also mirror the system
 * area ($0000-$1FFF WRAM + $2100/$42xx/$43xx I/O), so DBR=$80 keeps low-WRAM
 * globals and absolute I/O reachable — same discipline as bank $00, one
 * mirror over.
 *
 * The header checksum/complement words at $FFDC-$FFDF are emitted as zero
 * here and patched to valid values by build.sh after linking (emulator GUIs
 * use them to score the LoROM/HiROM mapping guess; an invalid pair can make
 * a ROM mis-detect and "not work" outside headless test runners).
 */
__rc0 = 0x00;
INCLUDE imag-regs.ld
ASSERT(__rc31 == 0x001f, "Inconsistent zero page map.")

MEMORY {
    zp : ORIGIN = __rc31 + 1, LENGTH = 0x100 - (__rc31 + 1)
    ram (rw)         : ORIGIN = 0x0200,   LENGTH = 0x1E00   /* low WRAM        */
    bank_7e_ram (rw) : ORIGIN = 0x7E2000, LENGTH = 0xE000   /* shadow home     */

    rom (rx) : ORIGIN = 0x808000, LENGTH = 0x7FC0   /* 32KB LoROM window       */
    hdr (rx) : ORIGIN = 0x80FFC0, LENGTH = 0x40     /* header + vectors        */
}
REGION_ALIAS("c_readonly", rom)
REGION_ALIAS("c_writeable", ram)
__stack = 0x0200;
ENTRY(_reset)

SECTIONS {
  .text.startup : { KEEP(*(.text.startup)) } >rom
  .text   : { *(.text .text.*) } >rom
  .rodata : { *(.rodata .rodata.*) } >rom      /* incl. the .lz blobs (data.s) */
  .data   : { *(.data .data.*) } >ram AT>rom
  .bss    (NOLOAD) : { *(.bss .bss.* COMMON) } >ram
  .noinit (NOLOAD) : { *(.noinit .noinit.*) } >ram
  .zp     : { *(.zp .zp.*) } >zp

  /* The 13,440-byte decoded-tilemap shadow lives at $7E:2000 (port/DMA only). */
  .bank_7e_bss (NOLOAD) : { *(.bank_7e_bss .bank_7e_bss.*) . = ALIGN(2); } >bank_7e_ram

  /* SNES header — LoROM ($20) + FastROM ($10) = map mode $30. */
  .snes_header 0x80FFC0 : {
    BYTE(0x53) BYTE(0x4E) BYTE(0x45) BYTE(0x53) BYTE(0x2D)   /* "SNES-" */
    BYTE(0x4C) BYTE(0x5A) BYTE(0x53) BYTE(0x53) BYTE(0x20)   /* "LZSS " */
    BYTE(0x44) BYTE(0x45) BYTE(0x4D) BYTE(0x4F) BYTE(0x20)   /* "DEMO " */
    BYTE(0x20) BYTE(0x20) BYTE(0x20) BYTE(0x20) BYTE(0x20)
    BYTE(0x20)
    BYTE(0x30) BYTE(0x00)   /* $FFD5 map mode $30 = LoROM + FastROM; $FFD6 type */
    BYTE(0x05)              /* $FFD7 ROM size: 32KB */
    BYTE(0x00) BYTE(0x01) BYTE(0x33) BYTE(0x00)
    SHORT(0x0000) SHORT(0x0000)   /* $FFDC checksum pair — patched by build.sh */
  } >hdr
  .vectors_native 0x80FFE4 : {
    SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub)
    SHORT(_irq_stub) SHORT(_irq_stub)
    LONG(0x00000000)
  } >hdr
  .vectors_emulation 0x80FFF4 : {
    SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub)
    SHORT(_reset)    SHORT(_irq_stub)
  } >hdr
}
OUTPUT_FORMAT {
  FULL(rom) FULL(hdr)
}
