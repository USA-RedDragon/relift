//! The RSP state, a reference for the vector unit, and a run of one
//! lifted instruction on the IR interpreter.
//!
//! The reference (`Rsp::step`) is a plain per-element port of the SISD
//! paths of ares n64/rsp/interpreter-vpu.cpp, and its ROM tables are built
//! as ares n64/rsp/rsp.cpp builds them:
//!
//! Copyright (c) 2004-2025 ares team, Near et al
//!
//! Permission to use, copy, modify, and/or distribute this software for any
//! purpose with or without fee is hereby granted, provided that the above
//! copyright notice and this permission notice appear in all copies.
//!
//! THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
//! WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
//! MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
//! ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//! WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
//! ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
//! OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

use std::sync::OnceLock;

use relift_cpu_rsp::lift;
use relift_cpu_rsp::regs;
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};

/// A vector register or accumulator slice: element 0 first.
pub type Vec8 = [u16; 8];

/// A xorshift generator.
pub struct Rng(pub u64);

impl Rng {
    pub fn next(&mut self) -> u64 {
        let mut x = self.0 | 1;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }

    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }

    fn edge16(&mut self) -> u16 {
        const EDGE: [u16; 12] = [
            0x0000, 0x0001, 0x0002, 0x001F, 0x0020, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF,
            0xC000,
        ];
        EDGE[self.below(EDGE.len() as u64) as usize]
    }

    fn vec(&mut self) -> Vec8 {
        std::array::from_fn(|_| self.next() as u16)
    }

    fn edge_vec(&mut self) -> Vec8 {
        std::array::from_fn(|_| self.edge16())
    }

    fn flags(&mut self) -> [bool; 8] {
        std::array::from_fn(|_| self.next() & 1 == 1)
    }
}

/// The RSP state the vector unit reads and writes.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Rsp {
    pub r: [u32; 32],
    pub v: [Vec8; 32],
    pub acch: Vec8,
    pub accm: Vec8,
    pub accl: Vec8,
    pub vcol: [bool; 8],
    pub vcoh: [bool; 8],
    pub vccl: [bool; 8],
    pub vcch: [bool; 8],
    pub vce: [bool; 8],
    pub divin: u16,
    pub divout: u16,
    pub divdp: bool,
    pub dmem: Vec<u8>,
}

/// The reciprocal and inverse square root ROMs, built as ares builds them.
pub fn roms() -> &'static ([u16; 512], [u16; 512]) {
    static ROMS: OnceLock<([u16; 512], [u16; 512])> = OnceLock::new();
    ROMS.get_or_init(|| {
        let mut rcp = [0u16; 512];
        rcp[0] = 0xFFFF;
        for (i, e) in rcp.iter_mut().enumerate().skip(1) {
            let a = i as u64 + 512;
            let b = (1u64 << 34) / a;
            *e = ((b + 1) >> 8) as u16;
        }
        let mut rsq = [0u16; 512];
        for (i, e) in rsq.iter_mut().enumerate() {
            let a = (i as u64 + 512) >> (i % 2);
            let mut b = 1u64 << 17;
            while a * (b + 1) * (b + 1) < (1u64 << 44) {
                b += 1;
            }
            *e = (b >> 1) as u16;
        }
        (rcp, rsq)
    })
}

fn sclamp16(x: i64) -> u16 {
    x.clamp(-32768, 32767) as i16 as u16
}

fn s(x: u16) -> i32 {
    i32::from(x as i16)
}

fn u(x: u16) -> i32 {
    i32::from(x)
}

fn byte(v: &Vec8, k: usize) -> u8 {
    let k = k & 15;
    if k.is_multiple_of(2) {
        (v[k / 2] >> 8) as u8
    } else {
        v[k / 2] as u8
    }
}

fn set_byte(v: &mut Vec8, k: usize, x: u8) {
    let k = k & 15;
    let e = &mut v[k / 2];
    if k.is_multiple_of(2) {
        *e = (*e & 0x00FF) | (u16::from(x) << 8);
    } else {
        *e = (*e & 0xFF00) | u16::from(x);
    }
}

/// The element selector `e` applied to `v`.
pub fn sel(v: &Vec8, e: u32) -> Vec8 {
    std::array::from_fn(|n| {
        let k = match e {
            0 | 1 => n,
            2 | 3 => (n & !1) + (e as usize - 2),
            4..=7 => (n & !3) + (e as usize - 4),
            _ => e as usize - 8,
        };
        v[k]
    })
}

impl Rsp {
    fn blank() -> Rsp {
        Rsp {
            r: [0; 32],
            v: [[0; 8]; 32],
            acch: [0; 8],
            accm: [0; 8],
            accl: [0; 8],
            vcol: [false; 8],
            vcoh: [false; 8],
            vccl: [false; 8],
            vcch: [false; 8],
            vce: [false; 8],
            divin: 0,
            divout: 0,
            divdp: false,
            dmem: vec![0; 4096],
        }
    }

    fn fill_common(&mut self, rng: &mut Rng) {
        for i in 1..32 {
            self.r[i] = match rng.below(4) {
                0 => rng.next() as u32,
                1 => 0xFF0 + rng.below(16) as u32,
                2 => rng.below(16) as u32,
                _ => rng.below(0x1000) as u32,
            };
        }
        self.vcol = rng.flags();
        self.vcoh = rng.flags();
        self.vccl = rng.flags();
        self.vcch = rng.flags();
        self.vce = rng.flags();
        self.divin = rng.next() as u16;
        self.divout = rng.next() as u16;
        self.divdp = rng.next() & 1 == 1;
        for b in &mut self.dmem {
            *b = rng.next() as u8;
        }
    }

    /// A state of random values.
    pub fn random(rng: &mut Rng) -> Rsp {
        let mut s = Rsp::blank();
        s.fill_common(rng);
        for v in &mut s.v {
            *v = rng.vec();
        }
        s.acch = rng.vec();
        s.accm = rng.vec();
        s.accl = rng.vec();
        s
    }

    /// A state of edge values: every element and accumulator slice one of
    /// the values where saturation, carries and signs change.
    pub fn edge(rng: &mut Rng) -> Rsp {
        let mut s = Rsp::blank();
        s.fill_common(rng);
        for v in &mut s.v {
            *v = rng.edge_vec();
        }
        s.acch = std::array::from_fn(|_| {
            [0x0000, 0xFFFF, 0x0001, 0xFFFE, 0x7FFF, 0x8000][rng.below(6) as usize]
        });
        s.accm = rng.edge_vec();
        s.accl = rng.edge_vec();
        s
    }

    fn acc_get(&self, n: usize) -> u64 {
        (u64::from(self.acch[n]) << 32) | (u64::from(self.accm[n]) << 16) | u64::from(self.accl[n])
    }

    fn acc_set(&mut self, n: usize, x: u64) {
        self.acch[n] = (x >> 32) as u16;
        self.accm[n] = (x >> 16) as u16;
        self.accl[n] = x as u16;
    }

    fn acc_sat(&self, n: usize, slice: bool, negative: u16, positive: u16) -> u16 {
        if (self.acch[n] as i16) < 0 {
            if self.acch[n] != 0xFFFF || (self.accm[n] as i16) >= 0 {
                return negative;
            }
        } else if self.acch[n] != 0 || (self.accm[n] as i16) < 0 {
            return positive;
        }
        if slice { self.accm[n] } else { self.accl[n] }
    }

    fn rd(&self, a: u32) -> u8 {
        self.dmem[(a & 0xFFF) as usize]
    }

    fn wr(&mut self, a: u32, x: u8) {
        self.dmem[(a & 0xFFF) as usize] = x;
    }

    /// Runs one COP2, LWC2 or SWC2 word.
    pub fn step(&mut self, w: u32) {
        match w >> 26 {
            0x12 if w & (1 << 25) != 0 => self.vector(w),
            0x12 => self.moves(w),
            0x32 => self.load(w),
            0x3A => self.store(w),
            _ => panic!("{w:#010x} is no COP2 word"),
        }
    }

    fn moves(&mut self, w: u32) {
        let rt = ((w >> 16) & 31) as usize;
        let rd = ((w >> 11) & 31) as usize;
        let e = ((w >> 7) & 15) as usize;
        match (w >> 21) & 31 {
            0 => {
                if rt != 0 {
                    let v = &self.v[rd];
                    let x = (u16::from(byte(v, e)) << 8) | u16::from(byte(v, e + 1));
                    self.r[rt] = x as i16 as i32 as u32;
                }
            }
            4 => {
                let x = self.r[rt];
                set_byte(&mut self.v[rd], e, (x >> 8) as u8);
                if e != 15 {
                    set_byte(&mut self.v[rd], e + 1, x as u8);
                }
            }
            2 => {
                if rt != 0 {
                    let zero = [false; 8];
                    let (hi, lo) = match rd & 3 {
                        0 => (self.vcoh, self.vcol),
                        1 => (self.vcch, self.vccl),
                        _ => (zero, self.vce),
                    };
                    let mut x = 0u32;
                    for n in 0..8 {
                        x |= u32::from(lo[n]) << n;
                        x |= u32::from(hi[n]) << (8 + n);
                    }
                    self.r[rt] = x as u16 as i16 as i32 as u32;
                }
            }
            6 => {
                let x = self.r[rt];
                let lo: [bool; 8] = std::array::from_fn(|n| x & (1 << n) != 0);
                let hi: [bool; 8] = std::array::from_fn(|n| x & (1 << (8 + n)) != 0);
                match rd & 3 {
                    0 => {
                        self.vcol = lo;
                        self.vcoh = hi;
                    }
                    1 => {
                        self.vccl = lo;
                        self.vcch = hi;
                    }
                    _ => self.vce = lo,
                }
            }
            _ => {}
        }
    }

    fn vector(&mut self, w: u32) {
        let f = w & 63;
        let e = (w >> 21) & 15;
        let vt = ((w >> 16) & 31) as usize;
        let vsn = ((w >> 11) & 31) as usize;
        let vd = ((w >> 6) & 31) as usize;
        let de = vsn & 7;
        let vs = self.v[vsn];
        let vte = sel(&self.v[vt], e);
        let mut d = self.v[vd];
        match f {
            0x00 | 0x01 => {
                for n in 0..8 {
                    let p = i64::from(vs[n] as i16) * i64::from(vte[n] as i16) * 2 + 0x8000;
                    self.acc_set(n, p as u64);
                    d[n] = if f == 0 {
                        self.acc_sat(n, true, 0x8000, 0x7FFF)
                    } else if (self.acch[n] as i16) < 0 {
                        0
                    } else if ((self.acch[n] ^ self.accm[n]) as i16) < 0 {
                        0xFFFF
                    } else {
                        self.accm[n]
                    };
                }
            }
            0x02 | 0x0A => {
                let positive = f == 0x02;
                for n in 0..8 {
                    let mut product = i64::from(vte[n] as i16);
                    if vsn & 1 != 0 {
                        product <<= 16;
                    }
                    let mut acc = ((self.acc_get(n) << 16) as i64) >> 16;
                    if (!positive && acc < 0) || (positive && acc >= 0) {
                        acc = ((acc + product) << 16) >> 16;
                    }
                    self.acc_set(n, acc as u64);
                    d[n] = sclamp16(acc >> 16);
                }
            }
            0x03 => {
                for n in 0..8 {
                    let mut p = s(vs[n]) * s(vte[n]);
                    if p < 0 {
                        p += 31;
                    }
                    self.acch[n] = (p >> 16) as u16;
                    self.accm[n] = p as u16;
                    self.accl[n] = 0;
                    d[n] = sclamp16(i64::from(p >> 1)) & !15;
                }
            }
            0x04 => {
                for n in 0..8 {
                    let p = (u32::from(vs[n]) * u32::from(vte[n])) >> 16;
                    self.acc_set(n, u64::from(p as u16));
                }
                d = self.accl;
            }
            0x05 => {
                for n in 0..8 {
                    self.acc_set(n, i64::from(s(vs[n]) * u(vte[n])) as u64);
                }
                d = self.accm;
            }
            0x06 => {
                for n in 0..8 {
                    self.acc_set(n, i64::from(u(vs[n]) * s(vte[n])) as u64);
                }
                d = self.accl;
            }
            0x07 => {
                for n in 0..8 {
                    self.acc_set(n, (i64::from(s(vs[n]) * s(vte[n])) << 16) as u64);
                    d[n] = self.acc_sat(n, true, 0x8000, 0x7FFF);
                }
            }
            0x08 | 0x09 => {
                for n in 0..8 {
                    let p = i64::from(vs[n] as i16) * i64::from(vte[n] as i16) * 2;
                    self.acc_set(n, self.acc_get(n).wrapping_add(p as u64));
                    d[n] = if f == 0x08 {
                        self.acc_sat(n, true, 0x8000, 0x7FFF)
                    } else if (self.acch[n] as i16) < 0 {
                        0
                    } else if self.acch[n] != 0 || (self.accm[n] as i16) < 0 {
                        0xFFFF
                    } else {
                        self.accm[n]
                    };
                }
            }
            0x0B => {
                for (n, dn) in d.iter_mut().enumerate() {
                    let mut p = ((u32::from(self.acch[n]) << 16) | u32::from(self.accm[n])) as i32;
                    if p < 0 && p & 32 == 0 {
                        p += 32;
                    } else if p >= 32 && p & 32 == 0 {
                        p -= 32;
                    }
                    self.acch[n] = (p >> 16) as u16;
                    self.accm[n] = p as u16;
                    *dn = sclamp16(i64::from(p >> 1)) & !15;
                }
            }
            0x0C => {
                for n in 0..8 {
                    let p = (u32::from(vs[n]) * u32::from(vte[n])) >> 16;
                    self.acc_set(n, self.acc_get(n).wrapping_add(u64::from(p)));
                    d[n] = self.acc_sat(n, false, 0, 0xFFFF);
                }
            }
            0x0D => {
                for n in 0..8 {
                    let p = i64::from(s(vs[n]) * u(vte[n]));
                    self.acc_set(n, self.acc_get(n).wrapping_add(p as u64));
                    d[n] = self.acc_sat(n, true, 0x8000, 0x7FFF);
                }
            }
            0x0E => {
                for n in 0..8 {
                    let p = i64::from(u(vs[n]) * s(vte[n]));
                    self.acc_set(n, self.acc_get(n).wrapping_add(p as u64));
                    d[n] = self.acc_sat(n, false, 0, 0xFFFF);
                }
            }
            0x0F => {
                for n in 0..8 {
                    let r = ((self.acc_get(n) >> 16) as i32).wrapping_add(s(vs[n]) * s(vte[n]));
                    self.acch[n] = (r >> 16) as u16;
                    self.accm[n] = r as u16;
                    d[n] = self.acc_sat(n, true, 0x8000, 0x7FFF);
                }
            }
            0x10 | 0x11 => {
                for n in 0..8 {
                    let c = i32::from(self.vcol[n]);
                    let r = if f == 0x10 {
                        s(vs[n]) + s(vte[n]) + c
                    } else {
                        s(vs[n]) - s(vte[n]) - c
                    };
                    self.accl[n] = r as u16;
                    d[n] = sclamp16(i64::from(r));
                }
                self.vcol = [false; 8];
                self.vcoh = [false; 8];
            }
            0x13 => {
                for n in 0..8 {
                    let (a, b) = (vs[n] as i16, vte[n] as i16);
                    let (acc, r) = if a < 0 {
                        if b == i16::MIN {
                            (i16::MIN, i16::MAX)
                        } else {
                            (-b, -b)
                        }
                    } else if a > 0 {
                        (b, b)
                    } else {
                        (0, 0)
                    };
                    self.accl[n] = acc as u16;
                    d[n] = r as u16;
                }
            }
            0x14 => {
                for n in 0..8 {
                    let r = u32::from(vs[n]) + u32::from(vte[n]);
                    self.accl[n] = r as u16;
                    self.vcol[n] = r >> 16 != 0;
                }
                self.vcoh = [false; 8];
                d = self.accl;
            }
            0x15 => {
                for n in 0..8 {
                    let r = u32::from(vs[n]).wrapping_sub(u32::from(vte[n]));
                    self.accl[n] = r as u16;
                    self.vcol[n] = r >> 16 != 0;
                    self.vcoh[n] = r != 0;
                }
                d = self.accl;
            }
            0x1D => {
                d = match e {
                    8 => self.acch,
                    9 => self.accm,
                    10 => self.accl,
                    _ => [0; 8],
                };
            }
            0x20..=0x23 => {
                for n in 0..8 {
                    let (a, b) = (vs[n], vte[n]);
                    let c = match f {
                        0x20 => (a as i16) < (b as i16) || (a == b && self.vcol[n] && self.vcoh[n]),
                        0x21 => !self.vcoh[n] && a == b,
                        0x22 => a != b || self.vcoh[n],
                        _ => {
                            (a as i16) > (b as i16) || (a == b && (!self.vcol[n] || !self.vcoh[n]))
                        }
                    };
                    self.vccl[n] = c;
                    self.accl[n] = if c { a } else { b };
                }
                self.vcch = [false; 8];
                self.vcol = [false; 8];
                self.vcoh = [false; 8];
                d = self.accl;
            }
            0x24 => {
                for n in 0..8 {
                    let (a, b) = (vs[n], vte[n]);
                    if self.vcol[n] {
                        if self.vcoh[n] {
                            self.accl[n] = if self.vccl[n] { b.wrapping_neg() } else { a };
                        } else {
                            let sum = a.wrapping_add(b);
                            let carry = u32::from(a) + u32::from(b) != u32::from(sum);
                            let c = if self.vce[n] {
                                sum == 0 || !carry
                            } else {
                                sum == 0 && !carry
                            };
                            self.vccl[n] = c;
                            self.accl[n] = if c { b.wrapping_neg() } else { a };
                        }
                    } else if self.vcoh[n] {
                        self.accl[n] = if self.vcch[n] { b } else { a };
                    } else {
                        let c = u(a) - u(b) >= 0;
                        self.vcch[n] = c;
                        self.accl[n] = if c { b } else { a };
                    }
                }
                self.vcol = [false; 8];
                self.vcoh = [false; 8];
                self.vce = [false; 8];
                d = self.accl;
            }
            0x25 => {
                for n in 0..8 {
                    let (a, b) = (vs[n], vte[n]);
                    let (sa, sb) = (a as i16, b as i16);
                    if (sa ^ sb) < 0 {
                        let r = sa.wrapping_add(sb);
                        self.accl[n] = if r <= 0 { sb.wrapping_neg() as u16 } else { a };
                        self.vccl[n] = r <= 0;
                        self.vcch[n] = sb < 0;
                        self.vcol[n] = true;
                        self.vcoh[n] = r != 0 && a != (b ^ 0xFFFF);
                        self.vce[n] = r == -1;
                    } else {
                        let r = sa.wrapping_sub(sb);
                        self.accl[n] = if r >= 0 { b } else { a };
                        self.vccl[n] = sb < 0;
                        self.vcch[n] = r >= 0;
                        self.vcol[n] = false;
                        self.vcoh[n] = r != 0 && a != (b ^ 0xFFFF);
                        self.vce[n] = false;
                    }
                }
                d = self.accl;
            }
            0x26 => {
                for n in 0..8 {
                    let (a, b) = (vs[n], vte[n]);
                    if (s(a) ^ s(b)) < 0 {
                        self.vcch[n] = s(b) < 0;
                        let c = s(a) + s(b) < 0;
                        self.vccl[n] = c;
                        self.accl[n] = if c { !b } else { a };
                    } else {
                        self.vccl[n] = s(b) < 0;
                        let c = s(a) - s(b) >= 0;
                        self.vcch[n] = c;
                        self.accl[n] = if c { b } else { a };
                    }
                }
                self.vcol = [false; 8];
                self.vcoh = [false; 8];
                self.vce = [false; 8];
                d = self.accl;
            }
            0x27 => {
                for n in 0..8 {
                    self.accl[n] = if self.vccl[n] { vs[n] } else { vte[n] };
                }
                self.vcoh = [false; 8];
                self.vcol = [false; 8];
                d = self.accl;
            }
            0x28..=0x2D => {
                for n in 0..8 {
                    let (a, b) = (vs[n], vte[n]);
                    self.accl[n] = match f {
                        0x28 => a & b,
                        0x29 => !(a & b),
                        0x2A => a | b,
                        0x2B => !(a | b),
                        0x2C => a ^ b,
                        _ => !(a ^ b),
                    };
                }
                d = self.accl;
            }
            0x30 | 0x31 | 0x34 | 0x35 => {
                let (rcp, rsq) = roms();
                let low = f & 1 == 1;
                let x = self.v[vt][(e & 7) as usize];
                let input: i32 = if low && self.divdp {
                    ((u32::from(self.divin) << 16) | u32::from(x)) as i32
                } else {
                    i32::from(x as i16)
                };
                let mask = input >> 31;
                let mut data = input ^ mask;
                if input > -32768 {
                    data = data.wrapping_sub(mask);
                }
                let result: i32 = if data == 0 {
                    0x7FFF_FFFF
                } else if input == -32768 {
                    0xFFFF_0000u32 as i32
                } else {
                    let shift = (data as u32).leading_zeros();
                    let index = ((((data as u64) << shift) & 0x7FC0_0000) >> 22) as usize;
                    if f < 0x34 {
                        let r = (0x10000 | i32::from(rcp[index])) << 14;
                        (r >> (31 - shift)) ^ mask
                    } else {
                        let r = i32::from(rsq[(index & 0x1FE) | (shift as usize & 1)]);
                        let r = (0x10000 | r) << 14;
                        (r >> ((31 - shift) >> 1)) ^ mask
                    }
                };
                self.divdp = false;
                self.divout = (result >> 16) as u16;
                self.accl = vte;
                d[de] = result as u16;
            }
            0x32 | 0x36 => {
                self.accl = vte;
                self.divdp = true;
                self.divin = self.v[vt][(e & 7) as usize];
                d[de] = self.divout;
            }
            0x33 => {
                d[de] = vte[de];
                self.accl = vte;
            }
            0x37 | 0x3F => return,
            _ => {
                for n in 0..8 {
                    self.accl[n] = (s(vs[n]) + s(vte[n])) as u16;
                }
                d = [0; 8];
            }
        }
        self.v[vd] = d;
    }

    fn load(&mut self, w: u32) {
        let rs = self.r[((w >> 21) & 31) as usize];
        let vt = ((w >> 16) & 31) as usize;
        let op = (w >> 11) & 31;
        let e = (w >> 7) & 15;
        let imm = (((w & 0x7F) << 25) as i32 >> 25) as u32;
        let mut v = self.v[vt];
        match op {
            0..=3 => {
                let n = 1u32 << op;
                let mut a = rs.wrapping_add(imm.wrapping_mul(n));
                for k in e..(e + n).min(16) {
                    set_byte(&mut v, k as usize, self.rd(a));
                    a = a.wrapping_add(1);
                }
            }
            4 => {
                let mut a = rs.wrapping_add(imm.wrapping_mul(16));
                let end = (16 + e - (a & 15)).min(16);
                for k in e..end {
                    set_byte(&mut v, k as usize, self.rd(a));
                    a = a.wrapping_add(1);
                }
            }
            5 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let start = 16u32.wrapping_sub((a & 15).wrapping_sub(e));
                let mut a = a & !15;
                for k in start..16 {
                    set_byte(&mut v, k as usize, self.rd(a));
                    a = a.wrapping_add(1);
                }
            }
            6 | 7 => {
                let a = rs.wrapping_add(imm.wrapping_mul(8));
                let index = (a & 7).wrapping_sub(e);
                let base = a & !7;
                let sh = if op == 6 { 8 } else { 7 };
                for n in 0..8u32 {
                    v[n as usize] =
                        u16::from(self.rd(base.wrapping_add(index.wrapping_add(n) & 15))) << sh;
                }
            }
            8 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let index = (a & 7).wrapping_sub(e);
                let base = a & !7;
                for n in 0..8u32 {
                    v[n as usize] =
                        u16::from(self.rd(base.wrapping_add(index.wrapping_add(n * 2) & 15))) << 7;
                }
            }
            9 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let index = (a & 7).wrapping_sub(e);
                let base = a & !7;
                let mut tmp = [0u16; 8];
                for o in 0..4u32 {
                    tmp[o as usize] =
                        u16::from(self.rd(base.wrapping_add(index.wrapping_add(o * 4) & 15))) << 7;
                    tmp[o as usize + 4] =
                        u16::from(self.rd(base.wrapping_add(index.wrapping_add(o * 4 + 8) & 15)))
                            << 7;
                }
                for k in e..(e + 8).min(16) {
                    set_byte(&mut v, k as usize, byte(&tmp, k as usize));
                }
            }
            11 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let begin = a & !7;
                let mut a = begin.wrapping_add((e + (a & 8)) & 15);
                let g = vt & !7;
                let mut off = (e >> 1) as usize;
                for i in 0..8 {
                    let x = self.rd(a);
                    set_byte(&mut self.v[g + off], i * 2, x);
                    a = a.wrapping_add(1);
                    if a == begin.wrapping_add(16) {
                        a = begin;
                    }
                    let x = self.rd(a);
                    set_byte(&mut self.v[g + off], i * 2 + 1, x);
                    a = a.wrapping_add(1);
                    if a == begin.wrapping_add(16) {
                        a = begin;
                    }
                    off = (off + 1) & 7;
                }
                return;
            }
            _ => return,
        }
        self.v[vt] = v;
    }

    fn store(&mut self, w: u32) {
        let rs = self.r[((w >> 21) & 31) as usize];
        let vt = ((w >> 16) & 31) as usize;
        let op = (w >> 11) & 31;
        let e = (w >> 7) & 15;
        let imm = (((w & 0x7F) << 25) as i32 >> 25) as u32;
        let v = self.v[vt];
        match op {
            0..=3 => {
                let n = 1u32 << op;
                let mut a = rs.wrapping_add(imm.wrapping_mul(n));
                for k in e..e + n {
                    self.wr(a, byte(&v, k as usize));
                    a = a.wrapping_add(1);
                }
            }
            4 => {
                let mut a = rs.wrapping_add(imm.wrapping_mul(16));
                let end = e + (16 - (a & 15));
                for k in e..end {
                    self.wr(a, byte(&v, k as usize));
                    a = a.wrapping_add(1);
                }
            }
            5 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let end = e + (a & 15);
                let base = 16 - (a & 15);
                let mut a = a & !15;
                for k in e..end {
                    self.wr(a, byte(&v, (k + base) as usize));
                    a = a.wrapping_add(1);
                }
            }
            6 | 7 => {
                let mut a = rs.wrapping_add(imm.wrapping_mul(8));
                for k in e..e + 8 {
                    let o = k & 15;
                    let hi_first = op == 6;
                    let x = if (o < 8) == hi_first {
                        byte(&v, ((o & 7) << 1) as usize)
                    } else {
                        (v[(o & 7) as usize] >> 7) as u8
                    };
                    self.wr(a, x);
                    a = a.wrapping_add(1);
                }
            }
            8 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let index = a & 7;
                let base = a & !7;
                for o in 0..8u32 {
                    let b = e + o * 2;
                    let x = (byte(&v, b as usize) << 1) | (byte(&v, (b + 1) as usize) >> 7);
                    self.wr(base.wrapping_add((index + o * 2) & 15), x);
                }
            }
            9 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let b = a & 7;
                let base = a & !7;
                let order: Option<[usize; 4]> = match e {
                    0 | 15 => Some([0, 1, 2, 3]),
                    1 => Some([6, 7, 4, 5]),
                    4 => Some([1, 2, 3, 0]),
                    5 => Some([7, 4, 5, 6]),
                    8 => Some([4, 5, 6, 7]),
                    11 => Some([3, 0, 1, 2]),
                    12 => Some([5, 6, 7, 4]),
                    _ => None,
                };
                for k in 0..4u32 {
                    let x = order.map_or(0, |o| (v[o[k as usize]] >> 7) as u8);
                    self.wr(base.wrapping_add((b + k * 4) & 15), x);
                }
            }
            10 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let base = a & !7;
                for k in 0..16 {
                    self.wr(
                        base.wrapping_add(((a & 7) + k) & 15),
                        byte(&v, (e + k) as usize),
                    );
                }
            }
            11 => {
                let a = rs.wrapping_add(imm.wrapping_mul(16));
                let g = vt & !7;
                let mut element = 16 - (e & !1);
                let mut b = (a & 7).wrapping_sub(e & !1);
                let base = a & !7;
                for r in g..g + 8 {
                    for _ in 0..2 {
                        let x = byte(&self.v[r], (element & 15) as usize);
                        self.wr(base.wrapping_add(b & 15), x);
                        b = b.wrapping_add(1);
                        element += 1;
                    }
                }
            }
            _ => {}
        }
    }
}

fn vec_word(v: &Vec8) -> u128 {
    v.iter().fold(0u128, |acc, &x| (acc << 16) | u128::from(x))
}

fn word_vec(x: u128) -> Vec8 {
    std::array::from_fn(|n| (x >> (16 * (7 - n))) as u16)
}

fn mask_word(f: &[bool; 8]) -> u128 {
    f.iter()
        .fold(0u128, |acc, &b| (acc << 16) | if b { 0xFFFF } else { 0 })
}

fn word_mask(x: u128) -> [bool; 8] {
    let v = word_vec(x);
    std::array::from_fn(|n| {
        assert!(
            v[n] == 0 || v[n] == 0xFFFF,
            "flag lane {n} is {:#06x}",
            v[n]
        );
        v[n] != 0
    })
}

/// Lifts `word` at IMEM 0x100 and runs it on the interpreter from `s`.
pub fn run_lifted(s: &Rsp, word: u32) -> Rsp {
    let l = lift::lift(0x100, word, 0).expect("lifts");
    let mut st = State::new(regs::regs().len());
    for n in 0..32u8 {
        st.regs[regs::gpr(n).index()] = u128::from(s.r[usize::from(n)]);
        st.regs[regs::vpr(n).index()] = vec_word(&s.v[usize::from(n)]);
    }
    st.regs[regs::ACCH.index()] = vec_word(&s.acch);
    st.regs[regs::ACCM.index()] = vec_word(&s.accm);
    st.regs[regs::ACCL.index()] = vec_word(&s.accl);
    st.regs[regs::VCOL.index()] = mask_word(&s.vcol);
    st.regs[regs::VCOH.index()] = mask_word(&s.vcoh);
    st.regs[regs::VCCL.index()] = mask_word(&s.vccl);
    st.regs[regs::VCCH.index()] = mask_word(&s.vcch);
    st.regs[regs::VCE.index()] = mask_word(&s.vce);
    st.regs[regs::DIVIN.index()] = u128::from(s.divin);
    st.regs[regs::DIVOUT.index()] = u128::from(s.divout);
    st.regs[regs::DIVDP.index()] = u128::from(s.divdp);
    let mut mem = FlatMemory {
        base: 0,
        bytes: s.dmem.clone(),
    };
    interp::run(&l.block, &mut st, &mut mem, &mut NoIntrinsics).expect("runs");
    let mut out = s.clone();
    for n in 0..32u8 {
        out.r[usize::from(n)] = st.regs[regs::gpr(n).index()] as u32;
        out.v[usize::from(n)] = word_vec(st.regs[regs::vpr(n).index()]);
    }
    out.acch = word_vec(st.regs[regs::ACCH.index()]);
    out.accm = word_vec(st.regs[regs::ACCM.index()]);
    out.accl = word_vec(st.regs[regs::ACCL.index()]);
    out.vcol = word_mask(st.regs[regs::VCOL.index()]);
    out.vcoh = word_mask(st.regs[regs::VCOH.index()]);
    out.vccl = word_mask(st.regs[regs::VCCL.index()]);
    out.vcch = word_mask(st.regs[regs::VCCH.index()]);
    out.vce = word_mask(st.regs[regs::VCE.index()]);
    out.divin = st.regs[regs::DIVIN.index()] as u16;
    out.divout = st.regs[regs::DIVOUT.index()] as u16;
    out.divdp = st.regs[regs::DIVDP.index()] != 0;
    out.dmem = mem.bytes;
    out
}

/// The fields where `got` differs from `want`.
pub fn diff(want: &Rsp, got: &Rsp) -> String {
    let mut d = Vec::new();
    for i in 0..32 {
        if want.r[i] != got.r[i] {
            d.push(format!("r{i} want {:#x} got {:#x}", want.r[i], got.r[i]));
        }
        if want.v[i] != got.v[i] {
            d.push(format!(
                "v{i} want {:04x?} got {:04x?}",
                want.v[i], got.v[i]
            ));
        }
    }
    for (n, a, b) in [
        ("ACCH", &want.acch, &got.acch),
        ("ACCM", &want.accm, &got.accm),
        ("ACCL", &want.accl, &got.accl),
    ] {
        if a != b {
            d.push(format!("{n} want {a:04x?} got {b:04x?}"));
        }
    }
    for (n, a, b) in [
        ("VCOL", &want.vcol, &got.vcol),
        ("VCOH", &want.vcoh, &got.vcoh),
        ("VCCL", &want.vccl, &got.vccl),
        ("VCCH", &want.vcch, &got.vcch),
        ("VCE", &want.vce, &got.vce),
    ] {
        if a != b {
            d.push(format!("{n} want {a:?} got {b:?}"));
        }
    }
    if (want.divin, want.divout, want.divdp) != (got.divin, got.divout, got.divdp) {
        d.push(format!(
            "DIV want {:#06x} {:#06x} {} got {:#06x} {:#06x} {}",
            want.divin, want.divout, want.divdp, got.divin, got.divout, got.divdp
        ));
    }
    for a in 0..4096 {
        if want.dmem[a] != got.dmem[a] {
            d.push(format!(
                "dmem[{a:#05x}] want {:#04x} got {:#04x}",
                want.dmem[a], got.dmem[a]
            ));
        }
    }
    d.join("; ")
}
