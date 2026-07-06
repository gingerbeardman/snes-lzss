; boot.s — reset stub for the snes-lzss demo ROM (one 64KB HiROM bank).
;
; Runs in 6502 emulation mode (llvm-mos mosw65816 keeps the 8-bit register
; model). Zeroes low-WRAM .bss ($0200-$1FFF), then long-jumps into bank $C0
; (PBR=$C0) leaving DBR=$00 so RAM access, absolute I/O ($21xx/$42xx/$43xx)
; and rodata reads (via the $00:8000 mirror of $C0:8000) all resolve.
.section .text.startup,"ax",@progbits

.global _reset
.extern main

_reset:
    sei
    ldx #$ff
    txs

    ; Zero $0200-$1FFF (.bss / .noinit: decoder state, ring, scratch, stack).
    lda #$00
    sta $00            ; DP pointer low  (imaginary reg)
    ldx #$02
    stx $01            ; DP pointer high -> start at page $02
    ldy #$00
clear_loop:
    sta ($00),y        ; DBR=$00 -> writes RAM $0200-$1FFF
    iny
    bne clear_loop
    inc $01
    ldx $01
    cpx #$20           ; stop once we pass page $1F ($2000)
    bne clear_loop

    jml (main_vec)     ; PBR=$C0 (lower bank half is not visible at PBR=$00)
halt:
    jmp halt
main_vec:
    .word main
    .byte $C0

; No NMI/IRQ is ever enabled; vectors still need a real target.
.global _irq_stub
_irq_stub:
    rti
