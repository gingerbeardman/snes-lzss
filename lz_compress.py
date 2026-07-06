#!/usr/bin/env python3
"""LZSS compressor for SNES tilemap data (and general byte streams).

This is the compressor half of a two-part SNES LZSS codec.  The decoder is
map_lz.h, a C header that compiles unchanged on both llvm-mos/65816 (SNES) and
the host.

Stream format
-------------
A flag byte precedes every group of 8 tokens (bit 0 = first token; 1 = literal,
0 = match), then the token bytes:

  literal:  1 raw byte

  match:    b0 = lo8(off-1)
            b1 = hi4(off-1) | (lenN << 4)
            off = distance back into the DECODED output, 1..4096
            lenN 0..14 -> len 3..17
            lenN 15    -> one extra byte follows; len = 18 + ext_byte (18..273)

Matches may overlap (off < len); the decoder replicates the period-off pattern
by clamping each copy chunk to off bytes.  The sliding window IS the decoded
output — no separate ring buffer is needed.

Usage (CLI)
-----------
    # Compress a file:
    python3 lz_compress.py encode input.bin output.lz

    # Decompress (for verification):
    python3 lz_compress.py decode input.lz output.bin <rawlen>

    # Round-trip self-check:
    python3 lz_compress.py check input.bin

Usage (library)
---------------
    from lz_compress import lz_encode, lz_decode

    compressed = lz_encode(raw_bytes)
    recovered  = lz_decode(compressed, len(raw_bytes))
    assert recovered == raw_bytes
"""

import sys

# ---- codec constants (must match map_lz.h) ----
LZ_WINDOW = 4096
LZ_MINLEN = 3
LZ_MAXLEN = 18 + 255   # lenN==15 + max ext byte (273)


def lz_encode(data: bytes) -> bytes:
    """Greedy-with-lazy LZSS encoder.

    Uses hash chains on 3-byte prefixes with exhaustive search within the
    4096-byte window, giving near-optimal results for a greedy+lazy parse.
    Lazy evaluation: after finding a match at position i, the encoder checks
    whether a match starting at i+1 is longer; if so, it emits a literal for i
    and continues from i+1.  This recovers a few percent on typical tilemap data.

    Args:
        data: raw bytes to compress (max 65535 bytes; decoder uses uint16_t).

    Returns:
        Compressed bytes in the stream format described above.
    """
    if len(data) > 0xFFFF:
        raise ValueError(f"input too large ({len(data)} > 65535 bytes)")
    n = len(data)
    head: dict[bytes, list[int]] = {}   # 3-byte prefix -> list of positions (ascending)

    def find(i: int) -> tuple[int, int]:
        """Return (best_len, best_off) for the longest match at position i."""
        if i + LZ_MINLEN > n:
            return 0, 0
        best_len, best_off = 0, 0
        lim = min(LZ_MAXLEN, n - i)
        for j in reversed(head.get(bytes(data[i:i + 3]), ())):
            if i - j > LZ_WINDOW:
                break           # list is ascending; older positions only from here
            l = 3
            while l < lim and data[j + l] == data[i + l]:
                l += 1
            if l > best_len:
                best_len, best_off = l, i - j
                if l == lim:
                    break
        return best_len, best_off

    def insert(i: int) -> None:
        if i + 3 <= n:
            head.setdefault(bytes(data[i:i + 3]), []).append(i)

    out = bytearray()
    flag_pos = -1
    flag_bit = 8

    def token(bs: bytes, is_lit: bool) -> None:
        nonlocal flag_pos, flag_bit
        if flag_bit == 8:
            flag_pos = len(out)
            out.append(0)
            flag_bit = 0
        if is_lit:
            out[flag_pos] |= 1 << flag_bit
        flag_bit += 1
        out.extend(bs)

    i = 0
    while i < n:
        mlen, moff = find(i)
        if mlen >= LZ_MINLEN:
            insert(i)
            # Lazy: check if i+1 gives a better match; if so emit literal for i
            nlen, _ = find(i + 1)
            if nlen > mlen:
                token(bytes([data[i]]), True)
                i += 1
                continue
            om1 = moff - 1
            if mlen <= 17:
                token(bytes([om1 & 0xFF, ((om1 >> 8) & 0x0F) | ((mlen - 3) << 4)]), False)
            else:
                token(bytes([om1 & 0xFF, ((om1 >> 8) & 0x0F) | 0xF0, mlen - 18]), False)
            for k in range(i + 1, i + mlen):
                insert(k)
            i += mlen
        else:
            insert(i)
            token(bytes([data[i]]), True)
            i += 1

    return bytes(out)


def lz_decode(blob: bytes, rawlen: int) -> bytes:
    """Reference decoder — mirrors the C implementation in map_lz.h exactly.

    Args:
        blob:   compressed bytes produced by lz_encode.
        rawlen: expected length of the decompressed output (in bytes).

    Returns:
        Decompressed bytes.
    """
    out = bytearray()
    si = 0
    flags, nbits = 0, 0
    while len(out) < rawlen:
        if nbits == 0:
            flags = blob[si]; si += 1; nbits = 8
        if flags & 1:
            out.append(blob[si]); si += 1
        else:
            b0, b1 = blob[si], blob[si + 1]; si += 2
            off = (((b1 & 0x0F) << 8) | b0) + 1
            ln = b1 >> 4
            if ln == 15:
                ln = 18 + blob[si]; si += 1
            else:
                ln += 3
            for _ in range(ln):
                out.append(out[-off])
        flags >>= 1; nbits -= 1
    return bytes(out)


# ---- CLI ----

def _cmd_encode(args: list[str]) -> int:
    if len(args) != 2:
        print("usage: lz_compress.py encode <input> <output.lz>", file=sys.stderr)
        return 2
    data = open(args[0], "rb").read()
    compressed = lz_encode(data)
    open(args[1], "wb").write(compressed)
    ratio = 100 * len(compressed) / len(data) if data else 0
    print(f"{args[0]}: {len(data)} -> {len(compressed)} bytes ({ratio:.1f}%)")
    return 0


def _cmd_decode(args: list[str]) -> int:
    if len(args) != 3:
        print("usage: lz_compress.py decode <input.lz> <output> <rawlen>", file=sys.stderr)
        return 2
    blob = open(args[0], "rb").read()
    rawlen = int(args[2])
    out = lz_decode(blob, rawlen)
    open(args[1], "wb").write(out)
    print(f"decoded {len(blob)} -> {len(out)} bytes")
    return 0


def _cmd_check(args: list[str]) -> int:
    if len(args) != 1:
        print("usage: lz_compress.py check <input>", file=sys.stderr)
        return 2
    data = open(args[0], "rb").read()
    compressed = lz_encode(data)
    recovered = lz_decode(compressed, len(data))
    if recovered != data:
        print(f"MISMATCH after round-trip!", file=sys.stderr)
        return 1
    ratio = 100 * len(compressed) / len(data) if data else 0
    print(f"{args[0]}: {len(data)} -> {len(compressed)} bytes ({ratio:.1f}%)  OK")
    return 0


_COMMANDS = {"encode": _cmd_encode, "decode": _cmd_decode, "check": _cmd_check}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in _COMMANDS:
        print(f"usage: lz_compress.py [{' | '.join(_COMMANDS)}] ...", file=sys.stderr)
        sys.exit(2)
    sys.exit(_COMMANDS[sys.argv[1]](sys.argv[2:]))
