//! The source-wide scans: dense code regions, function starts by what a
//! word does, the link address vote, relocation fields and RSP code.

use std::collections::HashMap;

use relift_cpu_mips::decode::{self as mdecode, Encoding};
use relift_cpu_mips::lift;

use crate::eval;
use crate::val::{Abs, Base, SP, State, sext32};

/// The word decodes to an instruction with a template.
pub(crate) const VALID: u8 = 1;
/// It is a branch or jump.
pub(crate) const BRANCH: u8 = 2;
/// It is zero.
pub(crate) const ZERO: u8 = 4;

/// The words of a source and what each is.
pub(crate) struct Words {
    pub w: Vec<u32>,
    pub f: Vec<u8>,
}

/// The decoded encoding and its template class.
fn form(w: u32) -> Option<(Encoding, &'static str)> {
    mdecode::forms()
        .iter()
        .find(|(_, m, v, _)| w & m == *v)
        .map(|f| (f.0, f.3))
}

impl Words {
    pub(crate) fn new(bytes: &[u8]) -> Words {
        let w: Vec<u32> = bytes
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| u32::from_be_bytes(*c))
            .collect();
        let f = w
            .iter()
            .map(|&x| {
                let mut fl = 0;
                if x == 0 {
                    fl |= ZERO;
                }
                if let Some((e, class)) = form(x)
                    && lift::has_template(e)
                {
                    fl |= VALID;
                    if class == "branch" || e == Encoding::Eret {
                        fl |= BRANCH;
                    }
                }
                fl
            })
            .collect();
        Words { w, f }
    }

    pub(crate) fn valid(&self, i: usize) -> bool {
        self.f.get(i).is_some_and(|f| f & VALID != 0)
    }
}

/// The window of the region scan, in words.
const WIN: usize = 32;
/// Valid words a window needs.
const DENSE: usize = 29;
/// Zero words a window may have, less one.
const MAX_ZERO: usize = WIN / 2;
/// Regions closer than this many words are one.
const GAP: usize = 64;
/// The shortest region, in words.
const MIN_LEN: usize = 16;

/// The dense code regions of `w` from word `from`: windows of 32 words with
/// at least 29 that decode, fewer than half zero and a branch or jump,
/// merged across gaps up to 64 words, at least 16 words long.
pub(crate) fn regions(w: &Words, from: usize) -> Vec<(usize, usize)> {
    let n = w.f.len();
    let mut out: Vec<(usize, usize)> = Vec::new();
    if n < from + WIN {
        return out;
    }
    let pre = |bit: u8| {
        let mut c = vec![0u32; n + 1];
        for i in 0..n {
            c[i + 1] = c[i] + u32::from(w.f[i] & bit != 0);
        }
        c
    };
    let (cv, cz, cb) = (pre(VALID), pre(ZERO), pre(BRANCH));
    let mut s = from;
    let mut cur: Option<(usize, usize)> = None;
    while s + WIN <= n {
        let e = s + WIN;
        let v = (cv[e] - cv[s]) as usize;
        let z = (cz[e] - cz[s]) as usize;
        let b = cb[e] - cb[s];
        if v >= DENSE && z < MAX_ZERO && b >= 1 {
            cur = match cur {
                Some((a, ce)) if s <= ce + GAP => Some((a, e.max(ce))),
                Some(r) => {
                    out.push(r);
                    Some((s, e))
                }
                None => Some((s, e)),
            };
        }
        s += WIN / 4;
    }
    out.extend(cur);
    out.retain(|r| r.1 - r.0 >= MIN_LEN);
    let mut grown: Vec<(usize, usize)> = Vec::with_capacity(out.len());
    for r in out {
        let g = grow(w, r, from, n);
        match grown.last_mut() {
            Some(l) if g.0 <= l.1 => l.1 = l.1.max(g.1),
            _ => grown.push(g),
        }
    }
    grown
}

/// The most zero words in a row that growing crosses: the padding that
/// aligns a function to 32 bytes.
const GROW_ZEROS: usize = 7;

/// `r` grown across the words that decode on both sides, crossing runs of
/// at most seven zero words (alignment padding) and ending before a longer
/// one.
pub(crate) fn grow(w: &Words, r: (usize, usize), lo: usize, hi: usize) -> (usize, usize) {
    let (mut a, mut b) = r;
    let mut zeros = 0;
    let mut k = a;
    while k > lo && w.valid(k - 1) {
        k -= 1;
        if w.f[k] & ZERO != 0 {
            zeros += 1;
            if zeros > GROW_ZEROS {
                break;
            }
        } else {
            zeros = 0;
            a = k;
        }
    }
    zeros = 0;
    k = b;
    while k < hi && w.valid(k) {
        if w.f[k] & ZERO != 0 {
            zeros += 1;
            if zeros > GROW_ZEROS {
                break;
            }
        } else {
            zeros = 0;
            b = k + 1;
        }
        k += 1;
    }
    (a, b)
}

/// The lifted block of word `i` run from an entry state with nothing
/// known of memory.
fn probe(w: &Words, i: usize, pc: u32) -> Option<eval::Out> {
    let slot = w.w.get(i + 1).copied().unwrap_or(0);
    let l = lift::lift(sext32(pc) as u64, w.w[i], slot).ok()?;
    Some(eval::eval(&l.block, &State::entry(), &mut |_, _| None))
}

/// Function start candidates in `r`, as word indices: a word that lowers
/// the stack pointer by a constant (a frame allocation), and the word
/// after a return and its delay slot. `base` is the address of word 0.
pub(crate) fn starts(w: &Words, r: (usize, usize), base: u32) -> Vec<usize> {
    let mut out = Vec::new();
    let pc = |i: usize| base.wrapping_add(4 * i as u32);
    for i in r.0..r.1 {
        let Some((e, _)) = form(w.w[i]) else {
            continue;
        };
        match e {
            Encoding::Addiu | Encoding::Daddiu | Encoding::Addi | Encoding::Daddi => {
                if let Some(o) = probe(w, i, pc(i))
                    && let Abs::S(s) = o.st.regs[SP]
                    && s.base == Base::Entry(SP as u8)
                    && (-0x8000..0).contains(&(s.off as i32))
                {
                    out.push(i);
                }
            }
            Encoding::Jr => {
                if let Some(o) = probe(w, i, pc(i))
                    && o.targets == [Abs::entry(crate::val::RA)]
                    && i + 2 < r.1
                    && w.valid(i + 2)
                    && w.w[i + 2] != 0
                {
                    out.push(i + 2);
                }
            }
            _ => {}
        }
    }
    out.sort_unstable();
    out.dedup();
    out
}

/// The most targets and starts the vote pairs.
const VOTE_CAP: usize = 2048;

/// The link address of region `r`: the address `V` that the most `JAL`
/// targets of the region give as `V + 4 * s` for a start `s` of the region,
/// with at least 3 votes and twice the runner-up. Gives the address and
/// its votes.
pub(crate) fn vote(w: &Words, r: (usize, usize), starts: &[usize]) -> Option<(u32, u32)> {
    let span = 4 * (r.1 - r.0) as u64;
    let mut t: Vec<u32> = (r.0..r.1)
        .filter(|&i| w.valid(i) && w.w[i] >> 26 == 3)
        .map(|i| 0x8000_0000 | (w.w[i] & 0x03FF_FFFF) << 2)
        .collect();
    if t.len() < 3 || starts.is_empty() {
        return None;
    }
    let sample = |n: usize| -> Vec<usize> {
        if n <= VOTE_CAP {
            (0..n).collect()
        } else {
            (0..VOTE_CAP)
                .map(|k| k * (n - 1) / (VOTE_CAP - 1))
                .collect()
        }
    };
    t = sample(t.len()).into_iter().map(|k| t[k]).collect();
    let f: Vec<u32> = sample(starts.len())
        .into_iter()
        .map(|k| 4 * (starts[k] - r.0) as u32)
        .collect();
    let mut votes: HashMap<u32, u32> = HashMap::new();
    for &tt in &t {
        for &ff in &f {
            let Some(d) = tt.checked_sub(ff) else {
                continue;
            };
            if d >= 0x8000_0000 && u64::from(d) + span <= 0x9000_0000 {
                *votes.entry(d).or_default() += 1;
            }
        }
    }
    let mut best = (0, 0u32);
    let mut second = 0;
    for (d, c) in votes {
        if c > best.1 || c == best.1 && d < best.0 {
            second = second.max(best.1);
            best = (d, c);
        } else {
            second = second.max(c);
        }
    }
    (best.1 >= 3 && best.1 >= 2 * second).then_some(best)
}

/// The relocation fields of `words` linked at `link`: per word, the bits a
/// loader patches when it moves the code, for the fields that point into
/// `[link, link + 4 * len)`: `J` and `JAL` targets and `LUI` with the
/// low half that a later instruction adds through the same register.
pub(crate) fn reloc_masks(words: &[u32], link: u32) -> Vec<u32> {
    let end = link.wrapping_add(4 * words.len() as u32);
    let inside = |a: u32| a >= link && a < end;
    let mut m = vec![0u32; words.len()];
    let mut hi: [Option<(usize, u32)>; 32] = [None; 32];
    for (i, &x) in words.iter().enumerate() {
        let op = x >> 26;
        let rs = (x >> 21 & 31) as usize;
        let rt = (x >> 16 & 31) as usize;
        let imm = x & 0xFFFF;
        match op {
            2 | 3 => {
                if inside(link & 0xF000_0000 | (x & 0x03FF_FFFF) << 2) {
                    m[i] |= 0x03FF_FFFF;
                }
            }
            0x0F => hi[rt] = Some((i, imm << 16)),
            0x09
            | 0x19
            | 0x0D
            | 0x20..=0x27
            | 0x28..=0x2F
            | 0x31
            | 0x35
            | 0x37
            | 0x39
            | 0x3D
            | 0x3F => {
                if let Some((j, h)) = hi[rs] {
                    let a = if op == 0x0D {
                        h | imm
                    } else {
                        h.wrapping_add(i32::from(imm as u16 as i16) as u32)
                    };
                    if inside(a) {
                        m[j] |= 0xFFFF;
                        m[i] |= 0xFFFF;
                    }
                }
                if matches!(op, 0x09 | 0x19 | 0x0D | 0x20..=0x27) && rt != rs {
                    hi[rt] = None;
                }
            }
            _ => {}
        }
        if x == 0x03E0_0008 {
            hi = [None; 32];
        }
    }
    m
}

/// RSP code regions of a source: windows of 32 words that all decode as
/// RSP instructions, at least a quarter of them vector instructions that
/// the VR4300 does not decode, merged across gaps up to 64 words and grown
/// across the RSP words on both sides (crossing at most seven zero words;
/// at the end, up to seven zero words are kept, since IMEM runs them as
/// `NOP`s).
pub(crate) fn rsp_regions(cpu: &Words) -> Vec<(usize, usize)> {
    let n = cpu.w.len();
    let mut good = vec![false; n];
    let mut vec_op = vec![false; n];
    for i in 0..n {
        let x = cpu.w[i];
        match relift_cpu_rsp::decode::decode(x) {
            Some(e) => {
                good[i] = true;
                let c = e.class();
                vec_op[i] = (c == "vector" || c == "vmem") && cpu.f[i] & VALID == 0;
            }
            None => good[i] = x == 0,
        }
    }
    let mut out: Vec<(usize, usize)> = Vec::new();
    let mut s = 0;
    while s + WIN <= n {
        let e = s + WIN;
        let all = good[s..e].iter().all(|g| *g);
        let v = vec_op[s..e].iter().filter(|v| **v).count();
        if all && v >= WIN / 4 {
            match out.last_mut() {
                Some(l) if s <= l.1 + GAP => l.1 = l.1.max(e),
                _ => out.push((s, e)),
            }
        }
        s += WIN / 4;
    }
    let zero = |i: usize| cpu.w[i] == 0;
    let mut grown: Vec<(usize, usize)> = Vec::with_capacity(out.len());
    for (mut a, mut b) in out {
        let (mut k, mut z) = (a, 0);
        while k > 0 && good[k - 1] {
            k -= 1;
            if zero(k) {
                z += 1;
                if z > GROW_ZEROS {
                    break;
                }
            } else {
                z = 0;
                a = k;
            }
        }
        (k, z) = (b, 0);
        while k < n && good[k] {
            z = if zero(k) { z + 1 } else { 0 };
            if z > GROW_ZEROS {
                break;
            }
            b = k + 1;
            k += 1;
        }
        match grown.last_mut() {
            Some(l) if a <= l.1 => l.1 = l.1.max(b),
            _ => grown.push((a, b)),
        }
    }
    grown
}
