//! Hardware known answers from n64-systemtest (src/tests/rsp), replayed
//! through the templates on the IR interpreter: the instruction sequences
//! of its tests and the values it expects, which it took from runs on a
//! console. The expected values are copied from n64-systemtest:
//!
//! MIT License
//!
//! Copyright (c) 2021 lemmy-64
//!
//! Permission is hereby granted, free of charge, to any person obtaining a
//! copy of this software and associated documentation files (the
//! "Software"), to deal in the Software without restriction, including
//! without limitation the rights to use, copy, modify, merge, publish,
//! distribute, sublicense, and/or sell copies of the Software, and to
//! permit persons to whom the Software is furnished to do so, subject to
//! the following conditions:
//!
//! The above copyright notice and this permission notice shall be included
//! in all copies or substantial portions of the Software.
//!
//! THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//! OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//! MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//! IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//! CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//! TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//! SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

use crate::reference::{Rsp, Vec8, run_lifted};

/// The multiply tables: the instruction, its element selector, and the
/// expected result, then the accumulator's high, middle and low slices.
const MULTIPLY: &[(&str, u32, [Vec8; 4])] = &[
    (
        "VMULF",
        0,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8001, 0x7FFE, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8001, 0x7FFE, 0x8000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x8000, 0x8002, 0x8000,
            ],
        ],
    ),
    (
        "VMULF",
        1,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8001, 0x7FFE, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8001, 0x7FFE, 0x8000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x8000, 0x8002, 0x8000,
            ],
        ],
    ),
    (
        "VMULF",
        4,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8002, 0x8002, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8002, 0x8002, 0x7FFF,
            ],
            [
                0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x7FFE, 0x7FFE, 0x8000,
            ],
        ],
    ),
    (
        "VMULF",
        5,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8001, 0x8001, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x8000, 0x8001, 0x8001, 0x8000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
            ],
        ],
    ),
    (
        "VMUDH",
        0,
        [
            [
                0x0000, 0x0000, 0x0000, 0x2000, 0x7FFF, 0x8000, 0x7FFF, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x3FFF, 0xC000, 0x3FFF, 0x4000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x2000, 0x8000, 0x8000, 0x0001, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            ],
        ],
    ),
    (
        "VMUDH",
        3,
        [
            [
                0x0000, 0x0000, 0x2000, 0x2000, 0x7FFF, 0x8000, 0x8000, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x4000, 0xC000, 0xC000, 0x4000,
            ],
            [
                0x0000, 0x0000, 0x2000, 0x2000, 0x0000, 0x8000, 0x8000, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            ],
        ],
    ),
    (
        "VMUDH",
        4,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8000, 0x8000, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x3FFF, 0xC000, 0xC000, 0x3FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x8000, 0xFFFF, 0xFFFF, 0x8000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            ],
        ],
    ),
    (
        "VMUDH",
        15,
        [
            [
                0x0000, 0x8000, 0x7FFF, 0x7FFF, 0x7FFF, 0x8000, 0x8000, 0x7FFF,
            ],
            [
                0x0000, 0xFFFF, 0x0000, 0x0000, 0x4000, 0xC000, 0xC000, 0x4000,
            ],
            [
                0x0000, 0x8000, 0x8000, 0x8000, 0x0000, 0x8000, 0x8000, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            ],
        ],
    ),
    (
        "VMUDM",
        0,
        [
            [
                0x0000, 0x0000, 0x0000, 0xFFFF, 0xBFFF, 0x3FFF, 0x3FFF, 0xC000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0xFFFF, 0xBFFF, 0x3FFF, 0x3FFF, 0xC000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x2000, 0x8000, 0x8000, 0x0001, 0x0000,
            ],
        ],
    ),
    (
        "VMUDM",
        5,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0xC000, 0x3FFF, 0x3FFF, 0xC000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0xC000, 0x3FFF, 0x3FFF, 0xC000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8000, 0x8000, 0x0000,
            ],
        ],
    ),
    (
        "VMUDM",
        15,
        [
            [
                0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xC000, 0x3FFF, 0x3FFF, 0xC000,
            ],
            [
                0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF,
            ],
            [
                0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xC000, 0x3FFF, 0x3FFF, 0xC000,
            ],
            [
                0x0000, 0x8000, 0x8000, 0x8000, 0x0000, 0x8000, 0x8000, 0x0000,
            ],
        ],
    ),
    (
        "VMUDN",
        0,
        [
            [
                0x0000, 0x8000, 0x0001, 0x8000, 0x8000, 0x8000, 0x0001, 0x0000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0x8000, 0xC000, 0xC000, 0x3FFF, 0xC000,
            ],
            [
                0x0000, 0x8000, 0x0001, 0x8000, 0x8000, 0x8000, 0x0001, 0x0000,
            ],
        ],
    ),
    (
        "VMUDN",
        6,
        [
            [
                0x0000, 0xFFFF, 0x0001, 0x0001, 0x8000, 0x0001, 0x0001, 0x8000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF,
            ],
            [
                0x0000, 0xFFFF, 0x0001, 0x0001, 0x8000, 0x0001, 0x0001, 0x8000,
            ],
        ],
    ),
    (
        "VMUDN",
        15,
        [
            [
                0x0000, 0x8000, 0x8000, 0x8000, 0x0000, 0x8000, 0x8000, 0x0000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
            ],
            [
                0x0000, 0xFFFF, 0x8000, 0x8000, 0xC000, 0xC000, 0xC000, 0xC000,
            ],
            [
                0x0000, 0x8000, 0x8000, 0x8000, 0x0000, 0x8000, 0x8000, 0x0000,
            ],
        ],
    ),
    (
        "VMADH",
        0,
        [
            [
                0x0000, 0x0000, 0x0000, 0x2000, 0x7FFF, 0x8000, 0x7FFF, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x3FFF, 0xC000, 0x3FFF, 0x4000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x2000, 0xFFFF, 0x0001, 0x7FFF, 0x8000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x8000, 0x8002, 0x8000,
            ],
        ],
    ),
    (
        "VMADH",
        3,
        [
            [
                0x0000, 0x0000, 0x2000, 0x2000, 0x7FFF, 0x8000, 0x8000, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x4000, 0xC000, 0xC000, 0x4000,
            ],
            [
                0x0000, 0x0000, 0x2000, 0x2000, 0x7FFF, 0x0001, 0xFFFE, 0x8000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x8000, 0x8002, 0x8000,
            ],
        ],
    ),
    (
        "VMADH",
        4,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8000, 0x8000, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x3FFF, 0xC000, 0xC001, 0x4000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x8000, 0x7FFD, 0x0000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x8000, 0x8002, 0x8000,
            ],
        ],
    ),
    (
        "VMADH",
        15,
        [
            [
                0x0000, 0x8000, 0x7FFF, 0x7FFF, 0x7FFF, 0x8000, 0x8000, 0x7FFF,
            ],
            [
                0x0000, 0xFFFF, 0x0000, 0x0000, 0x4000, 0xC000, 0xC000, 0x4000,
            ],
            [
                0x0000, 0x8000, 0x8000, 0x8000, 0x7FFF, 0x0001, 0xFFFE, 0x8000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x8000, 0x8002, 0x8000,
            ],
        ],
    ),
    (
        "VMACF",
        0,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0001, 0x7FFF, 0x8000, 0x7FFF, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0001,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0001, 0xFFFE, 0x0002, 0xFFFC, 0x0000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0x0000, 0x8000, 0x8000, 0x8004, 0x8000,
            ],
        ],
    ),
    (
        "VMACF",
        4,
        [
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x7FFF, 0x8000, 0x0000, 0x7FFF,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0xFFFE, 0x0003, 0x0000, 0xFFFF,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x7FFE, 0x8000, 0x8000,
            ],
        ],
    ),
    (
        "VMACF",
        13,
        [
            [
                0x0000, 0xFFFF, 0x0001, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x7FFF,
            ],
            [
                0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0001,
            ],
            [
                0x0000, 0xFFFF, 0x0001, 0x0001, 0xFFFF, 0x0002, 0xFFFF, 0x0000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xC000, 0x8000, 0x8000, 0x8002, 0x8000,
            ],
        ],
    ),
    (
        "VMADM",
        0,
        [
            [
                0x0000, 0x0000, 0x0000, 0xFFFF, 0x3FFF, 0xC001, 0x7FFF, 0x4000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0xFFFF, 0x3FFF, 0xC001, 0xBFFD, 0x4000,
            ],
            [
                0x8000, 0x8000, 0x8000, 0xE000, 0x0000, 0x0000, 0x8003, 0x8000,
            ],
        ],
    ),
    (
        "VMADM",
        12,
        [
            [
                0x0000, 0x0001, 0xFFFF, 0x0000, 0x3FFF, 0xC001, 0x7FFF, 0x4000,
            ],
            [
                0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000,
            ],
            [
                0x0000, 0x0001, 0xFFFF, 0x0000, 0x3FFF, 0xC001, 0xBFFE, 0x4000,
            ],
            [
                0x8000, 0x0001, 0xFFFF, 0x3FFF, 0x0000, 0x7FFF, 0x8001, 0x0000,
            ],
        ],
    ),
    (
        "VMADN",
        0,
        [
            [
                0x8000, 0x0000, 0x8003, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x8000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0x8002, 0x4000, 0x4002, 0xBFFD, 0x4000,
            ],
            [
                0x8000, 0x0000, 0x8003, 0x0000, 0x0000, 0x0000, 0x8003, 0x8000,
            ],
        ],
    ),
    (
        "VMADN",
        7,
        [
            [
                0x8000, 0x0000, 0x0000, 0x0000, 0x8000, 0x0000, 0x0000, 0x8000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000,
            ],
            [
                0x0000, 0xFFFF, 0x8002, 0x8002, 0x4000, 0x4002, 0x4002, 0x4000,
            ],
            [
                0x8000, 0x0000, 0x0000, 0x0000, 0x8000, 0x0000, 0x0000, 0x8000,
            ],
        ],
    ),
    (
        "VMADN",
        5,
        [
            [
                0x8000, 0x0000, 0x0000, 0x0000, 0x8000, 0x0000, 0x0000, 0x8000,
            ],
            [
                0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000,
            ],
            [
                0x0000, 0xFFFF, 0x8002, 0x8002, 0x4000, 0x4002, 0x4002, 0x4000,
            ],
            [
                0x8000, 0x0000, 0x0000, 0x0000, 0x8000, 0x0000, 0x0000, 0x8000,
            ],
        ],
    ),
    (
        "VMADN",
        14,
        [
            [
                0x8000, 0xFFFD, 0x0003, 0x0003, 0x0000, 0xFFFF, 0xFFFF, 0x0000,
            ],
            [
                0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF,
            ],
            [
                0x0000, 0x0001, 0x7FFE, 0x7FFE, 0xC001, 0xBFFD, 0xBFFD, 0xC001,
            ],
            [
                0x8000, 0xFFFD, 0x0003, 0x0003, 0x0000, 0x8003, 0x8003, 0x0000,
            ],
        ],
    ),
];

const VMULF: u32 = 0x00;
const VMUDM: u32 = 0x05;
const VMUDN: u32 = 0x06;
const VMUDH: u32 = 0x07;
const VMACF: u32 = 0x08;
const VMADM: u32 = 0x0D;
const VMADN: u32 = 0x0E;
const VMADH: u32 = 0x0F;
const VSAR: u32 = 0x1D;

/// A computational instruction in the assembler's operand order.
fn vec(f: u32, vd: u32, vt: u32, vs: u32, e: u32) -> u32 {
    0x4A00_0000 | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | f
}

fn lqv(vt: u32, e: u32, off: u32, base: u32) -> u32 {
    0xC800_0000 | (base << 21) | (vt << 16) | (4 << 11) | (e << 7) | ((off / 16) & 0x7F)
}

fn sqv(vt: u32, e: u32, off: u32, base: u32) -> u32 {
    0xE800_0000 | (base << 21) | (vt << 16) | (4 << 11) | (e << 7) | ((off / 16) & 0x7F)
}

fn ori(rt: u32, rs: u32, imm: u32) -> u32 {
    (0x0D << 26) | (rs << 21) | (rt << 16) | imm
}

fn lui(rt: u32, imm: u32) -> u32 {
    (0x0F << 26) | (rt << 16) | imm
}

fn li(rt: u32, v: u32) -> Vec<u32> {
    vec![lui(rt, v >> 16), ori(rt, rt, v & 0xFFFF)]
}

fn mtc2(vs: u32, rt: u32, e: u32) -> u32 {
    0x4880_0000 | (rt << 16) | (vs << 11) | (e << 7)
}

fn mfc2(vs: u32, rt: u32, e: u32) -> u32 {
    0x4800_0000 | (rt << 16) | (vs << 11) | (e << 7)
}

fn ctc2(rt: u32, rd: u32) -> u32 {
    0x48C0_0000 | (rt << 16) | (rd << 11)
}

fn cfc2(rt: u32, rd: u32) -> u32 {
    0x4840_0000 | (rt << 16) | (rd << 11)
}

fn blank() -> Rsp {
    let mut s = Rsp::random(&mut crate::reference::Rng(3));
    s.r = [0; 32];
    s.dmem = vec![0; 4096];
    s
}

fn put16(s: &mut Rsp, a: usize, v: Vec8) {
    for (i, x) in v.iter().enumerate() {
        s.dmem[a + 2 * i] = (x >> 8) as u8;
        s.dmem[a + 2 * i + 1] = *x as u8;
    }
}

fn get16(s: &Rsp, a: usize) -> Vec8 {
    std::array::from_fn(|i| (u16::from(s.dmem[a + 2 * i]) << 8) | u16::from(s.dmem[a + 2 * i + 1]))
}

fn run(s: &mut Rsp, words: &[u32]) {
    for &w in words {
        *s = run_lifted(s, w);
    }
}

fn bytes_of(v: &Vec8) -> [u8; 16] {
    std::array::from_fn(|i| {
        if i % 2 == 0 {
            (v[i / 2] >> 8) as u8
        } else {
            v[i / 2] as u8
        }
    })
}

fn from_bytes(b: &[u8; 16]) -> Vec8 {
    std::array::from_fn(|i| (u16::from(b[2 * i]) << 8) | u16::from(b[2 * i + 1]))
}

/// Counts the checks and keeps the failures.
#[derive(Default)]
struct Checks {
    pass: usize,
    fails: Vec<String>,
}

impl Checks {
    fn eq<T: PartialEq + std::fmt::Debug>(&mut self, what: &str, got: T, want: T) {
        if got == want {
            self.pass += 1;
        } else {
            self.fails
                .push(format!("{what}: got {got:04x?}, want {want:04x?}"));
        }
    }
}

fn multiply(c: &mut Checks) {
    let a0 = [
        0x0000, 0x0000, 0x0000, 0xE000, 0x8001, 0x8000, 0x7FFF, 0x8000,
    ];
    let a0n = [
        0x0000, 0x8000, 0xFFFF, 0x8000, 0x8001, 0x8000, 0x7FFF, 0x8000,
    ];
    let a1 = [
        0x0000, 0x0001, 0xFFFF, 0xFFFF, 0x8000, 0x7FFF, 0x7FFF, 0x8000,
    ];
    for (name, e, want) in MULTIPLY {
        let e = *e;
        let (f, setup_e, first) = match *name {
            "VMULF" => (VMULF, e, None),
            "VMUDH" => (VMUDH, e, Some(VMULF)),
            "VMUDM" => (VMUDM, e, Some(VMULF)),
            "VMUDN" => (VMUDN, e, None),
            "VMADH" => (VMADH, 0, Some(VMULF)),
            "VMACF" => (VMACF, 0, Some(VMULF)),
            "VMADM" => (VMADM, 0, Some(VMULF)),
            _ => (VMADN, e, Some(VMULF)),
        };
        let mut s = blank();
        let v0 = if matches!(f, VMADN | VMUDN) { a0n } else { a0 };
        put16(&mut s, 0x00, v0);
        put16(&mut s, 0x10, a1);
        let mut w = vec![
            lqv(0, 0, 0x00, 0),
            lqv(1, 0, 0x10, 0),
            lqv(6, 0, 0x00, 0),
            lqv(7, 0, 0x10, 0),
        ];
        let accumulates = matches!(f, VMADH | VMACF | VMADM | VMADN);
        if let Some(g) = first {
            w.push(vec(g, 2, 0, 1, setup_e));
        }
        w.push(vec(f, 2, 0, 1, e));
        w.extend([
            vec(VSAR, 3, 0, 0, 8),
            vec(VSAR, 4, 0, 0, 9),
            vec(VSAR, 5, 0, 0, 10),
        ]);
        for r in 2..6u32 {
            w.push(sqv(r, 0, 0x100 + 0x10 * (r - 2), 0));
        }
        if accumulates {
            w.push(vec(VMULF, 2, 0, 1, setup_e));
        }
        w.push(vec(f, 6, 6, 1, e));
        if accumulates {
            w.push(vec(VMULF, 2, 0, 1, setup_e));
        }
        w.push(vec(f, 7, 0, 7, e));
        w.extend([sqv(6, 0, 0x140, 0), sqv(7, 0, 0x150, 0)]);
        run(&mut s, &w);
        let names = ["result", "acc high", "acc mid", "acc low"];
        for k in 0..4 {
            c.eq(
                &format!("{name} e={e} {}", names[k]),
                get16(&s, 0x100 + 0x10 * k),
                want[k],
            );
        }
        c.eq(
            &format!("{name} e={e} V6, V6, V1"),
            get16(&s, 0x140),
            want[0],
        );
        c.eq(
            &format!("{name} e={e} V7, V0, V7"),
            get16(&s, 0x150),
            want[0],
        );
    }
}

/// The accumulator overflow test of op_vmadh.rs.
fn vmadh_overflow(c: &mut Checks) {
    let mut s = blank();
    put16(&mut s, 0, [0x8000, 0x7FFF, 0, 0, 0, 0, 0, 0]);
    put16(&mut s, 0x10, [0x8000, 0x8000, 0, 0, 0, 0, 0, 0]);
    let sar = [
        vec(VSAR, 3, 0, 0, 8),
        vec(VSAR, 4, 0, 0, 9),
        vec(VSAR, 5, 0, 0, 10),
    ];
    run(
        &mut s,
        &[lqv(0, 0, 0, 0), lqv(1, 0, 0x10, 0), vec(VMULF, 2, 0, 1, 0)],
    );
    run(&mut s, &[vec(VMADH, 2, 0, 1, 0), vec(VMADH, 2, 0, 1, 0)]);
    run(&mut s, &sar);
    c.eq(
        "VMADH overflow 1 result",
        s.v[2],
        [0x8000, 0x8000, 0, 0, 0, 0, 0, 0],
    );
    c.eq(
        "VMADH overflow 1 high",
        s.v[3],
        [0x8000, 0x8000, 0, 0, 0, 0, 0, 0],
    );
    c.eq(
        "VMADH overflow 1 mid",
        s.v[4],
        [0x8000, 0x8001, 0, 0, 0, 0, 0, 0],
    );
    c.eq("VMADH overflow 1 low", s.v[5], [0x8000; 8]);
    run(&mut s, &[vec(VMADH, 2, 0, 1, 0)]);
    run(&mut s, &sar);
    c.eq(
        "VMADH overflow 2 result",
        s.v[2],
        [0x8000, 0x7FFF, 0, 0, 0, 0, 0, 0],
    );
    c.eq(
        "VMADH overflow 2 high",
        s.v[3],
        [0xC000, 0x4001, 0, 0, 0, 0, 0, 0],
    );
    c.eq(
        "VMADH overflow 2 mid",
        s.v[4],
        [0x8000, 1, 0, 0, 0, 0, 0, 0],
    );
    c.eq("VMADH overflow 2 low", s.v[5], [0x8000; 8]);
}

/// op_vsar.rs: VSAR with every element selector.
fn vsar(c: &mut Checks) {
    let mut s = blank();
    put16(
        &mut s,
        0,
        [
            0xEEEE, 0xFFFF, 0xDDDD, 0xCCCC, 0xBBBB, 0xAAAA, 0x9999, 0x8888,
        ],
    );
    put16(
        &mut s,
        0x10,
        [
            0x0010, 0x0001, 0xFFF1, 0x0200, 0xF1E2, 0x0810, 0x7FFF, 0x8100,
        ],
    );
    put16(
        &mut s,
        0x20,
        [
            0x0020, 0x0002, 0xFFF2, 0x0300, 0xF2E2, 0x0820, 0x7FFF, 0x8200,
        ],
    );
    run(
        &mut s,
        &[
            lqv(0, 0, 0, 0),
            lqv(1, 0, 0x10, 0),
            lqv(2, 0, 0x20, 0),
            vec(VMULF, 4, 1, 2, 0),
        ],
    );
    for e in 0..15u32 {
        let vt = 5 + e;
        run(&mut s, &[lqv(vt, 0, 0, 0), vec(VSAR, vt, 0, 0, e)]);
        let want = match e {
            9 => [
                0x0000, 0x0000, 0x0000, 0x000C, 0x0172, 0x0083, 0x7FFE, 0x7D04,
            ],
            10 => [
                0x8400, 0x8004, 0x81A4, 0x8000, 0xDB08, 0x8400, 0x8002, 0x8000,
            ],
            _ => [0; 8],
        };
        c.eq(&format!("VSAR e={e}"), s.v[vt as usize], want);
    }
}

/// op_mfc2_mtc2.rs.
fn mfc2_mtc2(c: &mut Checks) {
    let cleared = [
        0xAABB, 0xCCDD, 0xEEFF, 0xABBA, 0xBCCB, 0xCDDC, 0xEFFE, 0xACCA,
    ];
    let mut s = blank();
    put16(&mut s, 0, cleared);
    let mut w: Vec<u32> = (0..=16).map(|r| lqv(r, 0, 0, 0)).collect();
    w.extend(li(1, 0x1234_5678));
    w.extend((0..16).map(|e| mtc2(e, 1, e)));
    run(&mut s, &w);
    for i in 0..16usize {
        let mut want = bytes_of(&cleared);
        want[i] = 0x56;
        if i < 15 {
            want[i + 1] = 0x78;
        }
        c.eq(&format!("MTC2 e={i}"), s.v[i], from_bytes(&want));
    }
    c.eq("MTC2 e=15 leaves the next register", s.v[16], cleared);
    let t1 = [
        0x1122, 0x3344, 0x5566, 0x7788, 0x9887, 0x7665, 0x5443, 0x3221,
    ];
    let mut s = blank();
    put16(&mut s, 0, t1);
    run(&mut s, &[lqv(5, 0, 0, 0)]);
    let by = bytes_of(&t1);
    for e in 0..16u32 {
        let g = 8 + e;
        run(&mut s, &[lui(g, 0xEEEE), ori(g, g, 0xEEEE), mfc2(5, g, e)]);
        let x = (u16::from(by[e as usize]) << 8) | u16::from(by[(e as usize + 1) & 15]);
        c.eq(
            &format!("MFC2 e={e}"),
            s.r[g as usize],
            x as i16 as i32 as u32,
        );
    }
}

/// op_cfc2_ctc2.rs.
fn cfc2_ctc2(c: &mut Checks) {
    let mut s = blank();
    let mut w = Vec::new();
    for (r, v) in [(1, 0x1234_5678u32), (4, 0x8765_4321), (5, 0x1122_3344)] {
        w.extend(li(r, v));
    }
    w.extend([
        ctc2(1, 0),
        ctc2(4, 1),
        ctc2(5, 2),
        cfc2(16, 0),
        cfc2(17, 1),
        cfc2(18, 2),
    ]);
    for (r, v) in [(1, 0x1234_8678u32), (4, 0x8765_8321), (5, 0x1122_3384)] {
        w.extend(li(r, v));
    }
    w.extend([
        ctc2(1, 0),
        ctc2(4, 1),
        ctc2(5, 2),
        cfc2(19, 0),
        cfc2(20, 1),
        cfc2(21, 2),
    ]);
    run(&mut s, &w);
    for (i, want) in [0x5678u32, 0x4321, 0x44, 0xFFFF_8678, 0xFFFF_8321, 0x84]
        .iter()
        .enumerate()
    {
        c.eq(&format!("CTC2/CFC2 #{i}"), s.r[16 + i], *want);
    }
    let mut s = blank();
    let mut w = Vec::new();
    for (r, v) in [(1, 0x8678u32), (4, 0x8321), (5, 0x84)] {
        w.extend(li(r, v));
    }
    w.extend([ctc2(1, 0), ctc2(4, 1), ctc2(5, 2)]);
    run(&mut s, &w);
    for i in 0..32u32 {
        run(&mut s, &[ori(16, 0, 0), cfc2(16, i)]);
        let want = match i & 3 {
            0 => 0xFFFF_8678u32,
            1 => 0xFFFF_8321,
            _ => 0x84,
        };
        c.eq(&format!("CFC2 {i}"), s.r[16], want);
    }
    for i in 0..32u32 {
        run(
            &mut s,
            &[ori(16, 0, i), ctc2(16, i), cfc2(17, (i & 3).min(3))],
        );
        c.eq(&format!("CTC2 {i} read back"), s.r[17], i);
    }
}

/// op_lqv_sqv.rs.
fn lqv_sqv(c: &mut Checks) {
    let mut s = blank();
    let base = [0xFF01u16, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF];
    put16(&mut s, 0x10, base);
    let mut w = vec![
        ori(2, 0, 5),
        ori(3, 0, 12),
        ori(4, 0, 0x400),
        lqv(0, 0, 0x10, 0),
    ];
    for (vt, e) in [(1, 1), (2, 2), (3, 3), (4, 8), (5, 13), (6, 14), (7, 15)] {
        w.extend([lqv(vt, 0, 0, 0), lqv(vt, e, 0x10, 0)]);
    }
    w.extend([
        lqv(8, 0, 0x10, 0),
        lqv(8, 8, 0x10, 0),
        lqv(9, 0, 0x10, 0),
        lqv(9, 0, 0x10, 2),
        lqv(10, 0, 0x10, 0),
        lqv(10, 6, 0x10, 2),
        lqv(11, 0, 0x10, 0),
        lqv(11, 4, 0x10, 2),
    ]);
    for vt in 0..=11u32 {
        let off = if vt >= 10 {
            0x200 + 0x10 * (vt - 10)
        } else {
            0x100 + 0x10 * vt
        };
        w.push(sqv(vt, 0, off, 0));
    }
    run(&mut s, &w);
    let wants: [(&str, usize, Vec8); 12] = [
        ("e=0", 0x100, base),
        (
            "e=1",
            0x110,
            [0xFF, 0x100, 0x2300, 0x4500, 0x6700, 0x8900, 0xAB00, 0xCD00],
        ),
        (
            "e=2",
            0x120,
            [0x00, 0xFF01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD],
        ),
        (
            "e=3",
            0x130,
            [0x00, 0xFF, 0x0100, 0x2300, 0x4500, 0x6700, 0x8900, 0xAB00],
        ),
        ("e=8", 0x140, [0, 0, 0, 0, 0xFF01, 0x23, 0x45, 0x67]),
        ("e=13", 0x150, [0, 0, 0, 0, 0, 0, 0xFF, 0x0100]),
        ("e=14", 0x160, [0, 0, 0, 0, 0, 0, 0, 0xFF01]),
        ("e=15", 0x170, [0, 0, 0, 0, 0, 0, 0, 0x00FF]),
        (
            "e=0 then e=8",
            0x180,
            [0xFF01, 0x23, 0x45, 0x67, 0xFF01, 0x23, 0x45, 0x67],
        ),
        (
            "unaligned",
            0x190,
            [0x4500, 0x6700, 0x8900, 0xAB00, 0xCD00, 0xEFAB, 0xCD, 0xEF],
        ),
        (
            "unaligned above e",
            0x200,
            [0xFF01, 0x23, 0x45, 0x4500, 0x6700, 0x8900, 0xAB00, 0xCD00],
        ),
        (
            "unaligned below e",
            0x210,
            [0xFF01, 0x23, 0x4500, 0x6700, 0x8900, 0xAB00, 0xCD00, 0xEFEF],
        ),
    ];
    for (n, a, want) in wants {
        c.eq(&format!("LQV/SQV {n}"), get16(&s, a), want);
    }
    let mut s2 = s.clone();
    let mut w = Vec::new();
    for e in 0..16u32 {
        let a = 0x350 + 0x10 * e;
        if a >= 0x400 {
            w.push(sqv(0, e, a - 0x400, 4));
        } else {
            w.push(sqv(0, e, a, 0));
        }
    }
    run(&mut s2, &w);
    let by = bytes_of(&base);
    for e in 0..16usize {
        let b: [u8; 16] = std::array::from_fn(|k| by[(e + k) & 15]);
        c.eq(
            &format!("SQV e={e}"),
            get16(&s2, 0x350 + 0x10 * e),
            from_bytes(&b),
        );
    }
    let mut s3 = s.clone();
    run(
        &mut s3,
        &[
            sqv(0, 0, 0x240, 0),
            sqv(0, 0, 0x250, 0),
            sqv(0, 0, 0x260, 0),
            sqv(0, 0, 0x270, 0),
            sqv(0, 0, 0x280, 0),
            sqv(0, 0, 0x280, 2),
            sqv(0, 0, 0x270, 3),
            sqv(0, 13, 0x260, 3),
            sqv(0, 12, 0x250, 3),
            sqv(0, 11, 0x240, 3),
        ],
    );
    c.eq(
        "SQV e=11 at 12",
        get16(&s3, 0x240),
        [0xFF01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xAB00, 0xCD00],
    );
    c.eq(
        "SQV e=12 at 12",
        get16(&s3, 0x250),
        [0xFF01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0x00CD, 0x00EF],
    );
    c.eq(
        "SQV e=13 at 12",
        get16(&s3, 0x260),
        [0xFF01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD00, 0xEFFF],
    );
    c.eq(
        "SQV e=0 at 12",
        get16(&s3, 0x270),
        [0xFF01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xFF01, 0x0023],
    );
    c.eq(
        "SQV e=0 at 5",
        get16(&s3, 0x280),
        [0xFF01, 0x23, 0xFF, 0x100, 0x2300, 0x4500, 0x6700, 0x8900],
    );
}

#[test]
fn hardware_known_answers_hold() {
    let mut c = Checks::default();
    multiply(&mut c);
    vmadh_overflow(&mut c);
    vsar(&mut c);
    mfc2_mtc2(&mut c);
    cfc2_ctc2(&mut c);
    lqv_sqv(&mut c);
    println!(
        "n64-systemtest known answers: {} pass, {} fail",
        c.pass,
        c.fails.len()
    );
    assert!(c.fails.is_empty(), "{}", c.fails.join("\n"));
}
