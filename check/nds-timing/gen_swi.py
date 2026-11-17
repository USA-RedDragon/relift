#!/usr/bin/env python3
"""gen_swi.py OUT: writes OUT/swidata.h, the BIOS call and cartridge cases of the timing test ROM.

Each BIOS case times one call on a small and on a large input (or 16 and 48 calls of a call whose
input size is fixed), so the difference gives the cost per unit: an output byte for the
decompressors and copies, a call otherwise. The compressed inputs are made here (LZ77 0x10,
Huffman 0x28, run length 0x30, in the BIOS's formats) from deterministic text-like data; the ROM
checks each output's CRC16 against the one computed here. The cartridge cases time ROMCTRL
transfers of the chip ID command with a game's latency settings and with none, at both clocks."""
import os
import random
import sys

OUT = sys.argv[1]
SMALL, LARGE = 256, 4096


def sample(n, seed):
    rnd = random.Random(seed)
    words = [bytes(rnd.choice(b"etaoinshrdlu ") for _ in range(rnd.randrange(2, 8))) for _ in range(40)]
    out = bytearray()
    while len(out) < n:
        out += rnd.choice(words) + b" "
    return bytes(out[:n])


def lz77(data):
    out = bytearray((0x10 | len(data) << 8).to_bytes(4, "little"))
    i = 0
    while i < len(data):
        flags_at = len(out)
        out.append(0)
        for bit in range(8):
            if i >= len(data):
                break
            best_len, best_disp = 0, 0
            for disp in range(2, min(i, 4096) + 1):
                n = 0
                while n < 18 and i + n < len(data) and data[i + n] == data[i - disp + n]:
                    n += 1
                if n > best_len:
                    best_len, best_disp = n, disp
            if best_len >= 3:
                out[flags_at] |= 0x80 >> bit
                d = best_disp - 1
                out += bytes([(best_len - 3) << 4 | d >> 8, d & 0xFF])
                i += best_len
            else:
                out.append(data[i])
                i += 1
    out += bytes(-len(out) % 4)
    return bytes(out)


def rle(data):
    out = bytearray((0x30 | len(data) << 8).to_bytes(4, "little"))
    i = 0
    lit = bytearray()

    def flush():
        while lit:
            chunk = lit[:128]
            del lit[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)

    while i < len(data):
        n = 1
        while i + n < len(data) and n < 130 and data[i + n] == data[i]:
            n += 1
        if n >= 3:
            flush()
            out += bytes([0x80 | (n - 3), data[i]])
            i += n
        else:
            lit.append(data[i])
            i += 1
    flush()
    out += bytes(-len(out) % 4)
    return bytes(out)


def huffman(data):
    """8-bit Huffman: tree of node pairs; a node byte's bits 0-5 are the offset to its children, bit 7
    marks the left child a leaf, bit 6 the right child."""
    import heapq
    freq = {}
    for b in data:
        freq[b] = freq.get(b, 0) + 1
    heap = [(f, k, ("leaf", s)) for k, (s, f) in enumerate(sorted(freq.items()))]
    heapq.heapify(heap)
    k = len(heap)
    while len(heap) > 1:
        f1, _, a = heapq.heappop(heap)
        f2, _, b = heapq.heappop(heap)
        heapq.heappush(heap, (f1 + f2, k, ("node", a, b)))
        k += 1
    root = heap[0][2]
    codes = {}

    def walk(n, code):
        if n[0] == "leaf":
            codes[n[1]] = code or "0"
        else:
            walk(n[1], code + "0")
            walk(n[2], code + "1")

    walk(root, "")
    # node bytes by address: the root at 5 (the byte after the tree size), each node's pair of
    # children at the next free even address
    table = {}
    free = 6
    queue = [(root, 5)]
    while queue:
        node, at = queue.pop(0)
        if node[0] == "leaf":
            table[at] = node[1]
            continue
        pair, free = free, free + 2
        off = (pair - (at & ~1) - 2) // 2
        assert 0 <= off < 64, off
        flags = (0x80 if node[1][0] == "leaf" else 0) | (0x40 if node[2][0] == "leaf" else 0)
        table[at] = off | flags
        queue.append((node[1], pair))
        queue.append((node[2], pair + 1))
    end = free + (-free % 4)
    tree = bytes([(end - 4) // 2 - 1]) + bytes(table.get(a, 0) for a in range(5, end))
    bits = "".join(codes[b] for b in data)
    bits += "0" * (-len(bits) % 32)
    stream = b"".join(int(bits[i:i + 32], 2).to_bytes(4, "little") for i in range(0, len(bits), 32))
    return (0x28 | len(data) << 8).to_bytes(4, "little") + tree + stream


def unhuffman(blob):
    size = int.from_bytes(blob[:4], "little") >> 8
    tree_end = 4 + (blob[4] + 1) * 2
    out = bytearray()
    at = 5
    pos = tree_end
    while len(out) < size:
        word = int.from_bytes(blob[pos:pos + 4], "little")
        pos += 4
        for i in range(31, -1, -1):
            node = blob[at]
            child = (at & ~1) + (node & 0x3F) * 2 + 2
            right = word >> i & 1
            at = child + right
            if node & (0x40 if right else 0x80):
                out.append(blob[at])
                at = 5
                if len(out) == size:
                    break
    return bytes(out)


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


blobs = {}
for size in (SMALL, LARGE):
    d = sample(size, 7)
    blobs[f"lz_{size}"] = (lz77(d), crc16(d))
    blobs[f"rl_{size}"] = (rle(d), crc16(d))
    h = huffman(d)
    assert unhuffman(h) == d
    blobs[f"hf_{size}"] = (h, crc16(d))

def noise(n, seed):
    rnd = random.Random(seed)
    return bytes(rnd.randrange(256) for _ in range(n))


def runs(n, seed):
    rnd = random.Random(seed)
    out = bytearray()
    while len(out) < n:
        out += bytes([rnd.randrange(256)]) * 130
    return bytes(out[:n])


def bitpack(data):
    """1 bit per source pixel, LSB first, for BitUnPack's 1 to 8 bits."""
    return bytes(sum((data[i + j] & 1) << j for j in range(8)) for i in range(0, len(data), 8))


def unpack1to8(packed, off):
    return bytes(((b >> j) & 1) + off if ((b >> j) & 1) else 0 for b in packed for j in range(8))


def diff8(data):
    return (0x81 | len(data) << 8).to_bytes(4, "little") + bytes(
        [data[0]] + [(data[i] - data[i - 1]) & 0xFF for i in range(1, len(data))])


def diff16(data):
    h = [int.from_bytes(data[i:i + 2], "little") for i in range(0, len(data), 2)]
    d = [h[0]] + [(h[i] - h[i - 1]) & 0xFFFF for i in range(1, len(h))]
    return (0x82 | len(data) << 8).to_bytes(4, "little") + b"".join(x.to_bytes(2, "little") for x in d)


def pad4(b):
    return b + bytes(-len(b) % 4)


# hwtest 3c: inputs that split the decompressors' per-byte costs (all literals, all matches or runs,
# a two-symbol Huffman tree) and the calls hwtest 3 did not time, at 256 and 1024 bytes
SMALL2, LARGE2 = 256, 1024
for size in (SMALL2, LARGE2):
    lit, rep, run2 = noise(size, 11), bytes([0x41]) * size, runs(size, 13)
    blobs[f"lzlit_{size}"] = (lz77(lit), crc16(lit))
    blobs[f"lzrep_{size}"] = (lz77(rep), crc16(rep))
    blobs[f"rllit_{size}"] = (rle(lit), crc16(lit))
    blobs[f"rlrun_{size}"] = (rle(run2), crc16(run2))
    two = bytes(b"ab"[x & 1] for x in noise(size, 17))
    h = huffman(two)
    assert unhuffman(h) == two
    blobs[f"hf2_{size}"] = (h, crc16(two))
    smooth = sample(size, 19)
    blobs[f"df8_{size}"] = (pad4(diff8(smooth)), crc16(smooth))
    blobs[f"df16_{size}"] = (pad4(diff16(smooth)), crc16(smooth))
for size in (SMALL2, LARGE2):
    bits = noise(size, 23)
    packed = bitpack(bits)
    blobs[f"bup_{size}"] = (pad4(packed), crc16(unpack1to8(packed, 0x30)))
src = sample(LARGE, 9)
lines = ["/* generated by gen_swi.py */", f"#define SWI_SMALL {SMALL}", f"#define SWI_LARGE {LARGE}",
         f"#define SWI_SMALL2 {SMALL2}", f"#define SWI_LARGE2 {LARGE2}"]
for name, (b, c) in blobs.items():
    words = [int.from_bytes(b[i:i + 4], "little") for i in range(0, len(b), 4)]
    lines.append(f"static const u32 {name}[] __attribute__((aligned(4))) = {{")
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 8]) + ",")
    lines.append("};")
    lines.append(f"#define {name.upper()}_CRC 0x{c:04x}")
lines.append("static const u8 swi_src[] __attribute__((aligned(4))) = {")
for i in range(0, len(src), 16):
    lines.append("    " + ", ".join(str(x) for x in src[i:i + 16]) + ",")
lines.append("};")
lines.append(f"#define SWI_SRC_CRC_SMALL 0x{crc16(src[:SMALL]):04x}")
lines.append(f"#define SWI_SRC_CRC_LARGE 0x{crc16(src):04x}")
open(os.path.join(OUT, "swidata.h"), "w").write("\n".join(lines) + "\n")
print({k: len(v[0]) for k, v in blobs.items()})
