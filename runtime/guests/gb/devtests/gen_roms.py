#!/usr/bin/env python3
"""Writes the ROMs of the local cases of ci_cases.toml into a directory.

  python3 -I gen_roms.py DIR

Each ROM is a few hundred bytes of hand-assembled SM83 on a 32 KiB cartridge
with no mapper, ending in the mooneye pass protocol (B, C, D, E, H, L =
3, 5, 8, 13, 21, 34 at an LD B,B). Standard library only.
"""

import os
import sys


def cartridge(code):
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x134:0x13C] = b"RELIFTCB"
    rom[0x148] = 0
    for at, blob in code:
        rom[at : at + len(blob)] = blob
    sum_ = 0
    for b in rom[0x134:0x14D]:
        sum_ = (sum_ - b - 1) & 0xFF
    rom[0x14D] = sum_
    total = (sum(rom) - rom[0x14E] - rom[0x14F]) & 0xFFFF
    rom[0x14E] = total >> 8
    rom[0x14F] = total & 0xFF
    return bytes(rom)


PASS = bytes([0x06, 3, 0x0E, 5, 0x16, 8, 0x1E, 13, 0x26, 21, 0x2E, 34, 0x40, 0x18, 0xFE])


def copy(src, dst, count):
    return bytes(
        [0x21, src & 0xFF, src >> 8, 0x11, dst & 0xFF, dst >> 8, 0x01, count & 0xFF, count >> 8]
        + [0x2A, 0x12, 0x13, 0x0B, 0x78, 0xB1, 0x20, 0xF8]
    )


def fill_pairs(dst, count, first, second):
    return bytes(
        [0x21, dst & 0xFF, dst >> 8, 0x06, count, 0x3E, first, 0x22, 0x3E, second, 0x22, 0x05, 0x20, 0xF7]
    )


def cb_fetch_during_dma():
    """CB CB pairs in work RAM (SET 1,E) run while an OAM DMA copies a page of
    work RAM that alternates CB and 37: the bus gives CB 37 (SWAP A) for each
    instruction, whatever the memory at its two bytes holds. The NOP before
    the call sets which of the two bytes the first fetch of the run sees."""
    ram = bytes([0xCB, 0xCB] * 127 + [0xC9, 0x00])
    hram = bytes([0x3E, 0xC2, 0xE0, 0x46] + [0x00] + [0xCD, 0x00, 0xC0, 0xC9])
    main = (
        bytes([0xF3, 0x31, 0xFE, 0xFF])
        + copy(0x200, 0xC000, 0x100)
        + fill_pairs(0xC200, 0x50, 0xCB, 0x37)
        + copy(0x300, 0xFF80, len(hram))
        + bytes([0x3E, 0x5A, 0xCD, 0x80, 0xFF])
        + PASS
    )
    return cartridge([(0x150, main), (0x200, ram), (0x300, hram)])


ROMS = {"cb-fetch-dma.gb": cb_fetch_during_dma}


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, make in ROMS.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(make())


if __name__ == "__main__":
    main()
