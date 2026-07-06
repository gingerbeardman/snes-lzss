; data.s — embed the two compressed asset blobs (lz_compress.py output) in ROM.
; They land in .rodata, which demo.ld pins into the bank's upper half so the
; decoder can both DMA-stage them (bank $C0) and address them 16-bit (DBR=$00).
.section .rodata,"a",@progbits

; wild_ride tilemap, 240x28 tiles, column-major words (13,440 B raw -> 1,167 B)
.global demo_map_lz
demo_map_lz:
    .incbin "assets/wild_ride_map.lz"
.global demo_map_lz_end
demo_map_lz_end:

; wild_ride terrain CHR, 341 4bpp 8x8 tiles (10,912 B raw -> 1,179 B)
.global demo_chr_lz
demo_chr_lz:
    .incbin "assets/wild_ride_chr.lz"
.global demo_chr_lz_end
demo_chr_lz_end:
