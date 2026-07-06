; boot.s — reset stub for the snes-lzss demo ROM (one 32KB LoROM FastROM bank).
;
; Runs in 6502 emulation mode (llvm-mos mosw65816 keeps the 8-bit register
; model). The 16-bit reset vector lands here at PBR=$00 (the always-SlowROM
; bank $00 mirror of the same 32KB window).
;
; Init sequence (canonical SNES order):
;   1. SEI
;   2. Forced blank ($2100=$8F) — first PPU write, before anything else
;   3. NMITIMEN=0 ($4200) — NMI/IRQ/auto-joypad off during init
;   4. Stack pointer
;   5. Zero WRAM BSS ($0200-$1FFF)
;   6. Zero ZP imaginary registers ($00-$1F) — the clear loop leaves $01=$20
;   7. MEMSEL=1 ($420D) — unlock FastROM on banks $80-$FF (3.58 MHz)
;   8. PLB $80 — DBR=$80 so instruction fetch AND data reads run at 3.58 MHz,
;      and bank $80 still mirrors WRAM ($0000-$1FFF) + I/O ($2100/$42xx/$43xx)
;   9. JML into bank $80 (fast instruction fetch)
;
; NOTE: HDMAEN ($420C) and the PPU registers other than INIDISP are handled
; in main() which runs entirely under forced blank.
.section .text.startup,"ax",@progbits

.global _reset
.extern main

_reset:
    sei

    ; 1. Forced blank — mandatory first PPU write on every SNES program.
    ;    INIDISP is NOT reset by the hardware /RESET signal; it can hold a
    ;    non-blank value after a warm reset or power-on. Setting it here
    ;    prevents the PPU from outputting garbage during the init sequence,
    ;    and ensures VRAM writes in main() are never silently discarded.
    lda #$8f
    sta $2100           ; INIDISP: forced blank + max brightness

    ; 2. Disable NMI / IRQ / auto-joypad.
    ;    Hardware resets $4200 to $00, but explicit zeroing follows the
    ;    canonical init convention and makes the intent self-documenting.
    lda #$00
    sta $4200           ; NMITIMEN: all interrupts + auto-joypad off

    ; 3. Stack pointer (emulation mode: SP is always in page $01).
    ldx #$ff
    txs

    ; 4. Zero $0200-$1FFF (.bss / .noinit: decoder state, ring, scratch, stack).
    sta $00            ; DP pointer low  (imaginary reg)
    ldx #$02
    stx $01            ; DP pointer high -> start at page $02
    ldy #$00
clear_loop:
    sta ($00),y        ; DBR=$00 at reset -> writes RAM $0200-$1FFF
    iny
    bne clear_loop
    inc $01
    ldx $01
    cpx #$20           ; stop once we pass page $1F ($2000)
    bne clear_loop

    ; 5. Zero ZP $00-$1F (llvm-mos imaginary registers rc0-rc31).
    ;    The WRAM clear above used $00/$01 as a scratch pointer and left
    ;    $01=$20 (the loop's final high-byte value). Zeroing the full range
    ;    matches the C runtime convention — imaginary registers are zero at
    ;    program entry — and prevents rc1=$20 from silently corrupting any
    ;    16-bit value the compiler wide-zero-extends from rc1 without an
    ;    explicit high-byte store.
    ldx #$1f
clear_zp:
    sta $00,x
    dex
    bpl clear_zp

    ; 6. Enable FastROM and enter the $80 mirror.
    lda #$01
    sta $420d          ; MEMSEL = 1: banks $80-$FF run at 3.58MHz (FastROM)
    lda #$80
    pha
    plb                ; DBR = $80 (fast rodata mirror + visible I/O/WRAM)
    jml (main_vec)     ; PBR = $80 (fast instruction fetch)
halt:
    jmp halt
main_vec:
    .word main
    .byte $80

; No NMI/IRQ is ever enabled; vectors still need a real target.
.global _irq_stub
_irq_stub:
    rti
