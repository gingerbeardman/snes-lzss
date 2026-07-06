/* demo.ld — one 64KB HiROM bank ($C0) linker script for the snes-lzss demo
 * (llvm-mos, -mcpu=mosw65816).
 *
 * Layout rationale: the 65816 reaches a HiROM bank's LOWER half ($C0:0000-$7FFF)
 * only with PBR=$C0, while the $00:8000-$FFFF mirror exposes the UPPER half
 * ($C0:8000-$FFFF). llvm-mos runs code with PBR=$C0 (set by boot.s) but reads
 * data (RAM pointers, absolute I/O, rodata) with DBR=$00, so:
 *
 *   .text   -> lower half ($C0:0000+): near JSR/JMP under PBR=$C0.
 *   .rodata -> upper half ($C0:8000+): readable through a 16-bit pointer under
 *              DBR=$00 (the $00:8000 mirror), and DMA-able as bank $C0.
 *
 * The compressed asset blobs (data.s .incbin) land in .rodata, so both the
 * decoder's DMA staging (explicit bank $C0) and any direct pointer reads work.
 */
__rc0 = 0x00;
INCLUDE imag-regs.ld
ASSERT(__rc31 == 0x001f, "Inconsistent zero page map.")

MEMORY {
    zp : ORIGIN = __rc31 + 1, LENGTH = 0x100 - (__rc31 + 1)
    ram (rw)         : ORIGIN = 0x0200,   LENGTH = 0x1E00   /* low WRAM (DBR=$00) */
    bank_7e_ram (rw) : ORIGIN = 0x7E2000, LENGTH = 0xE000   /* WRAM shadow home  */

    rom  (rx) : ORIGIN = 0xC00000, LENGTH = 0xFE00   /* $C0:0000-$FDFF (linear)   */
    boot (rx) : ORIGIN = 0xC0FE00, LENGTH = 0x0200   /* startup, header, vectors  */
}
REGION_ALIAS("c_readonly", rom)
REGION_ALIAS("c_writeable", ram)
__stack = 0x0200;
ENTRY(_reset)

SECTIONS {
  /* Boot at $C0:FE00 (reachable at reset: $00:FE00 mirrors $C0:FE00). */
  .text.startup : { KEEP(*(.text.startup)) } >boot

  INCLUDE zp.ld

  /* Code in the lower half (PBR=$C0). */
  .text : { INCLUDE text-sections.ld } >rom

  /* rodata (incl. the .lz blobs) pinned into the upper half (DBR=$00 window). */
  .rodata 0xC08000 : { INCLUDE rodata-sections.ld } >rom
  ASSERT(ADDR(.rodata) >= 0xC08000, "rodata must sit in the DBR=$00 window ($C0:8000+)")

  INCLUDE data.ld
  INCLUDE bss.ld
  INCLUDE noinit.ld

  /* The 13,440-byte decoded-tilemap shadow lives at $7E:2000 (port/DMA only). */
  .bank_7e_bss : { *(.bank_7e_bss .bank_7e_bss.*) . = ALIGN(2); } >bank_7e_ram

  /* SNES header — HiROM, SlowROM. */
  .snes_header 0xC0FFC0 : {
    BYTE(0x53) BYTE(0x4E) BYTE(0x45) BYTE(0x53) BYTE(0x2D)   /* "SNES-" */
    BYTE(0x4C) BYTE(0x5A) BYTE(0x53) BYTE(0x53) BYTE(0x20)   /* "LZSS " */
    BYTE(0x44) BYTE(0x45) BYTE(0x4D) BYTE(0x4F) BYTE(0x20)   /* "DEMO " */
    BYTE(0x20) BYTE(0x20) BYTE(0x20) BYTE(0x20) BYTE(0x20)
    BYTE(0x20)
    BYTE(0x21) BYTE(0x00)   /* $FFD5 map mode: 0x21 = HiROM + SlowROM */
    BYTE(0x06)              /* ROM size: 64KB */
    BYTE(0x00) BYTE(0x01) BYTE(0x33) BYTE(0x00)
    SHORT(0x0000) SHORT(0x0000)
  } >boot
  .vectors_native 0xC0FFE4 : {
    SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub)
    SHORT(_irq_stub) SHORT(_irq_stub)
    LONG(0x00000000)
  } >boot
  .vectors_emulation 0xC0FFF4 : {
    SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub) SHORT(_irq_stub)
    SHORT(_reset)    SHORT(_irq_stub)
  } >boot
}
OUTPUT_FORMAT {
  FULL(rom) FULL(boot)
}
