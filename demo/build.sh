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

ls -la snes-lzss-demo.smc
echo "[ok] built snes-lzss-demo.smc"
