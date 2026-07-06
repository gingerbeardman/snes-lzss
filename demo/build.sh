#!/bin/bash
# build.sh — build the snes-lzss demo ROM (demo/snes-lzss-demo.smc).
#
# Requires the llvm-mos SDK (https://llvm-mos.org — the 65816 'mosw65816' CPU
# target ships in current releases). Either have mos-common-clang on PATH or
# set LLVM_MOS to the SDK root:
#
#   LLVM_MOS=~/llvm-mos ./build.sh
#
# A prebuilt snes-lzss-demo.smc is checked in, so building is only needed
# after changing the demo source.
set -eu
cd "$(dirname "$0")"

if [ -n "${LLVM_MOS:-}" ]; then
  export PATH="$LLVM_MOS/bin:$PATH"
fi
command -v mos-common-clang >/dev/null || {
  echo "ERROR: mos-common-clang not found. Install llvm-mos (https://llvm-mos.org)"
  echo "       and put its bin/ on PATH, or run: LLVM_MOS=/path/to/llvm-mos $0"
  exit 1
}

mkdir -p build
mos-common-clang -mcpu=mosw65816 -I. -T demo.ld \
  -Os -fnonreentrant -fomit-frame-pointer -fno-stack-protector \
  -fdata-sections -ffunction-sections \
  -Wl,-Map=build/demo.map \
  -o snes-lzss-demo.smc \
  main.c data.s boot.s

# Patch a VALID header checksum into $FFDC-$FFDF (LoROM: file offset 0x7FDC).
# The linker emits zeros there; emulator GUIs score the LoROM/HiROM mapping
# guess with this pair, and an invalid pair can make the ROM mis-detect.
# With the pair pre-set to FF FF 00 00 its own contribution to the sum is the
# same as the final complement+checksum (always 0x1FE), so: sum, then write.
python3 - snes-lzss-demo.smc <<'EOF'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
assert len(d) == 0x8000, f"expected 32KB LoROM image, got {len(d)}"
d[0x7FDC:0x7FE0] = b'\xFF\xFF\x00\x00'
s = sum(d) & 0xFFFF
d[0x7FDC:0x7FE0] = bytes([(s ^ 0xFFFF) & 0xFF, (s ^ 0xFFFF) >> 8, s & 0xFF, s >> 8])
open(p, 'wb').write(d)
print(f"[ok] header checksum patched: ${s:04X} (complement ${s ^ 0xFFFF:04X})")
EOF

ls -la snes-lzss-demo.smc
echo "[ok] built snes-lzss-demo.smc"
