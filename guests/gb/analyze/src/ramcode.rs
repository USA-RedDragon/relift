//! The instructions a ROM will run from RAM, predicted without running it.
//!
//! Every function found is run from its entry on a CPU whose values are
//! either known bytes or unknown: registers start unknown (the cartridge
//! entry starts with the banks mapped at reset), ROM reads give the image's
//! bytes (the window only while its bank is known), and RAM reads give what
//! the same path stored. Calls are followed into the callee; a callee that
//! runs too long without returning to it from a call of its own, or whose
//! path ends, is left as if it had returned with the caller's registers but
//! `a` and the flags unknown. A branch on an unknown flag goes on along
//! the fall-through and forks the other way with a smaller budget. A path
//! ends at an unknown transfer, at a state it has been in before, at a ROM
//! location the analysis did not decode, or when its budget is spent. Loops
//! with known counts therefore run to the end: a copy of a routine from a
//! ROM table, through any helper, stores the routine's bytes, and a
//! generator that writes opcodes with immediates stores those opcodes. Code
//! copied to RAM runs on the same path, so code it stores is found too.
//!
//! A path that jumps from ROM into RAM, as a test driver does when it hands
//! control to a test it copied there, gets a budget of its own for that
//! code; when the path ends there it goes on, with its memory but unknown
//! registers, at the ROM targets of the jumps that code holds, so the
//! driver's next round copies its next test. A `halt` or `ei` with the
//! interrupt master enable set and known enabled interrupts calls the
//! handler of the first and forks into the others.
//!
//! Every instruction a path runs in RAM is predicted, and so is the code
//! decoded from the bytes a path holds where it jumps into RAM from ROM.
//! Every byte stored at a known RAM address also joins that address's value
//! set (with the WRAM bank in effect, on a colour cartridge), marked as
//! copied when the byte was read from ROM and generated otherwise. The
//! code is then decoded from the RAM targets of the transfers the analysis
//! found, of the direct jumps and calls from ROM the paths took, and of the
//! calls and jumps in copied code: at each address every opcode of the set
//! is a candidate and gives its own next address from its length; a CB
//! prefix takes the keys of the byte after it.

use std::collections::{BTreeMap, BTreeSet};

use relift_cpu_sm83::BASE;

use crate::bankmodel::{BankModel, BankReg};
use crate::fx::{FxMap, FxSet};
use crate::loc::Loc;

/// Most opcode keys decoded from one RAM address's value set; an address
/// with more is listed in [`RamOps::wide`] instead.
pub const MAX_KEYS: usize = 32;

/// Instructions the first path of one function may run, callees included.
const ROOT_STEPS: u32 = 1 << 22;

/// Instructions each path forked at an unknown branch may run.
const FORK_STEPS: u32 = 1 << 12;

/// Instructions all forked paths of one function may run.
const FORKS_STEPS: u32 = 1 << 16;

/// Instructions a call may run before the path gives it up and goes on
/// after it.
const CALL_STEPS: u32 = 1 << 18;

/// Most forked paths of one function waiting to run; the oldest is
/// dropped for a new one.
const MAX_PENDING: usize = 64;

/// Instructions a path may run in RAM code it jumped to from ROM before it
/// goes on at that code's exits to ROM.
const LAUNCH_STEPS: u32 = 1 << 24;

/// Most times the paths of one function go on at the exits of RAM code.
const MAX_LAUNCHES: usize = 64;

/// Most states one function's paths remember having been in; the
/// memory starts again when it is full.
const MAX_SEEN: usize = 1 << 16;

/// Most calls a path follows inside each other.
const MAX_FRAMES: usize = 32;

/// Most instructions predicted from one transfer target.
const MAX_PER_ENTRY: usize = 4096;

/// Most addresses in a row holding nothing but a NOP the program made
/// before decoding stops: cleared RAM, which the runtime runs as a NOP
/// sled. A `$00` or `$FF` among other values is taken as the fill of RAM
/// cleared before the code was stored, and decoding does not go on past it.
const NOP_RUN: u32 = 4;

/// Where the opcode byte of a predicted instruction came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum RamClass {
    /// A byte read from ROM and stored, as a copy loop does.
    Copied,
    /// A byte the program made: an immediate or a computed value.
    Generated,
}

impl RamClass {
    /// A short name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            RamClass::Copied => "copied",
            RamClass::Generated => "generated",
        }
    }
}

/// One instruction predicted to run in RAM.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RamOp {
    /// The address.
    pub addr: u16,
    /// The opcode, or `0x100 | byte` after a CB prefix.
    pub key: u16,
    /// Where the opcode byte came from.
    pub class: RamClass,
}

/// The instructions predicted to run in RAM.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RamOps {
    /// Address and key, each once.
    pub ops: Vec<RamOp>,
    /// The entry addresses decoded from: transfers into RAM.
    pub entries: Vec<u16>,
    /// Addresses on a decoded path whose value set holds more than
    /// [`MAX_KEYS`] opcode keys; nothing past them is predicted.
    pub wide: Vec<u16>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum Src {
    Imm,
    Rom,
    Calc,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct B {
    v: Option<u8>,
    s: Src,
}

const U: B = B {
    v: None,
    s: Src::Calc,
};

fn calc(v: Option<u8>) -> B {
    B { v, s: Src::Calc }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum Sp {
    Abs(u16),
    Rel(u16),
    Unknown,
}

const RA: usize = 7;
const RH: usize = 4;
const RL: usize = 5;
const UNKNOWN_BANK: u8 = 8;
const STACK_KEY: u32 = 1 << 24;

#[derive(Clone, Copy)]
struct Frame {
    r: [B; 8],
    ret: u16,
    sp: Sp,
    lo: Option<u16>,
    hi: Option<u8>,
    svbk: Option<u8>,
    at: u32,
    resumable: bool,
}

/// The memory a path stored, in pages shared between the paths forked
/// from each other until one of them stores into the page.
#[derive(Clone, Default)]
struct Mem {
    pages: FxMap<u32, std::rc::Rc<[B; 256]>>,
}

impl Mem {
    fn get(&self, k: u32) -> B {
        self.pages
            .get(&(k >> 8))
            .map_or(U, |pg| pg[(k & 0xFF) as usize])
    }

    fn insert(&mut self, k: u32, b: B) -> B {
        let pg = self
            .pages
            .entry(k >> 8)
            .or_insert_with(|| std::rc::Rc::new([U; 256]));
        std::mem::replace(&mut std::rc::Rc::make_mut(pg)[(k & 0xFF) as usize], b)
    }
}

#[derive(Clone)]
struct Path {
    r: [B; 8],
    z: Option<bool>,
    n: Option<bool>,
    h: Option<bool>,
    c: Option<bool>,
    sp: Sp,
    pc: u16,
    lo: Option<u16>,
    hi: Option<u8>,
    svbk: Option<u8>,
    mem: Mem,
    memh: u64,
    steps: u32,
    limit: u32,
    frames: Vec<Frame>,
    ie: Option<u8>,
    ime: Option<bool>,
    main: bool,
    free: bool,
    launch: Option<Launch>,
}

#[derive(Clone)]
struct Launch {
    exits: Vec<u16>,
    start: u32,
    limit: u32,
}

impl Path {
    fn resume(&mut self) -> bool {
        while let Some(fr) = self.frames.pop() {
            if !fr.resumable {
                continue;
            }
            self.r = fr.r;
            self.r[RA] = U;
            (self.z, self.n, self.h, self.c) = (None, None, None, None);
            self.sp = fr.sp;
            self.pc = fr.ret;
            self.lo = fr.lo;
            self.hi = fr.hi;
            self.svbk = fr.svbk;
            for f in &mut self.frames {
                f.at = self.steps;
            }
            return true;
        }
        false
    }
}

fn mix(mut x: u64) -> u64 {
    x = (x ^ (x >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    x ^ (x >> 31)
}

fn bhash(b: B) -> u64 {
    let s = match b.s {
        Src::Imm => 1,
        Src::Rom => 2,
        Src::Calc => 3,
    };
    b.v.map_or(0x1000, u64::from) | s << 16
}

#[derive(Clone, Copy, Default)]
struct Cell {
    known: [u64; 4],
    copied: [u64; 4],
}

impl Cell {
    fn add(&mut self, v: u8, copied: bool) {
        let (w, b) = (usize::from(v >> 6), 1u64 << (v & 63));
        self.known[w] |= b;
        if copied {
            self.copied[w] |= b;
        }
    }

    fn join(&mut self, o: &Cell) {
        for i in 0..4 {
            self.known[i] |= o.known[i];
            self.copied[i] |= o.copied[i];
        }
    }

    fn values(&self) -> impl Iterator<Item = u8> + '_ {
        (0..=255u8).filter(|v| self.known[usize::from(v >> 6)] & (1 << (v & 63)) != 0)
    }

    fn count(&self) -> usize {
        self.known.iter().map(|w| w.count_ones() as usize).sum()
    }

    fn class(&self, v: u8) -> RamClass {
        if self.copied[usize::from(v >> 6)] & (1 << (v & 63)) != 0 {
            RamClass::Copied
        } else {
            RamClass::Generated
        }
    }
}

#[derive(Default)]
struct Found {
    stores: FxMap<(u16, u8), Cell>,
    entries: FxSet<(u16, u8)>,
    ran: FxMap<(u16, u16), RamClass>,
}

impl Found {
    fn join(&mut self, o: Found) {
        for (k, c) in o.stores {
            self.stores.entry(k).or_default().join(&c);
        }
        self.entries.extend(o.entries);
        for (k, c) in o.ran {
            let e = self.ran.entry(k).or_insert(c);
            *e = (*e).min(c);
        }
    }
}

struct Machine<'a> {
    rom: &'a [u8],
    model: &'a BankModel,
    cgb: bool,
    code: &'a FxSet<Loc>,
}

fn norm(a: u16) -> u16 {
    if (0xE000..0xFE00).contains(&a) {
        a - 0x2000
    } else {
        a
    }
}

fn is_ram(a: u16) -> bool {
    a >= 0x8000 && !(0xFEA0..0xFF80).contains(&a) && a != 0xFFFF
}

fn banked(a: u16) -> bool {
    (0xD000..0xE000).contains(&a)
}

impl Machine<'_> {
    fn decoded(&self, p: &Path) -> bool {
        if p.pc >= 0x8000 || p.free {
            return true;
        }
        let at = if p.pc < 0x4000 {
            Loc::fixed(p.pc)
        } else {
            match self.bank(p) {
                Some(bank) => Loc { bank, addr: p.pc },
                None => return false,
            }
        };
        self.code.contains(&at)
    }

    fn bank(&self, p: &Path) -> Option<u16> {
        match self.model.resolve(p.lo, p.hi) {
            Some(Ok(b)) => Some(b),
            _ => None,
        }
    }

    fn rom_byte(&self, p: &Path, a: u16) -> Option<u8> {
        let off = if a < 0x4000 {
            usize::from(a)
        } else {
            usize::from(self.bank(p)?) * 0x4000 + usize::from(a - 0x4000)
        };
        self.rom.get(off).copied()
    }

    fn bucket(&self, p: &Path, a: u16) -> u8 {
        if !banked(a) {
            0
        } else if !self.cgb {
            1
        } else {
            p.svbk.unwrap_or(UNKNOWN_BANK)
        }
    }

    fn key(&self, p: &Path, a: u16) -> u32 {
        let a = norm(a);
        u32::from(self.bucket(p, a)) << 16 | u32::from(a)
    }

    fn read(&self, p: &Path, a: Option<u16>) -> B {
        let Some(a) = a else { return U };
        if a < 0x8000 {
            return B {
                v: self.rom_byte(p, a),
                s: Src::Rom,
            };
        }
        if !is_ram(a) {
            return U;
        }
        p.mem.get(self.key(p, a))
    }

    fn put(p: &mut Path, k: u32, b: B) {
        let old = p.mem.insert(k, b);
        let kh = u64::from(k) << 20;
        if old != U {
            p.memh ^= mix(kh ^ bhash(old));
        }
        if b != U {
            p.memh ^= mix(kh ^ bhash(b));
        }
    }

    fn write(&self, p: &mut Path, f: &mut Found, a: Option<u16>, b: B) {
        let Some(a) = a else { return };
        if a < 0x8000 {
            if let Some(reg) = self.model.classify(a) {
                let v = b.v.map(|v| self.model.register_value(reg, v));
                match reg {
                    BankReg::Lo => p.lo = v,
                    BankReg::Hi => p.hi = v.map(|v| u8::try_from(v).unwrap_or(0)),
                }
            }
            return;
        }
        if a == 0xFF70 {
            p.svbk = b.v.map(|v| (v & 7).max(1));
            return;
        }
        if a == 0xFFFF {
            p.ie = b.v;
            return;
        }
        if !is_ram(a) {
            return;
        }
        let k = self.key(p, a);
        Self::put(p, k, b);
        let a = norm(a);
        let bucket = self.bucket(p, a);
        let cell = f.stores.entry((a, bucket)).or_default();
        if let Some(v) = b.v {
            cell.add(v, b.s == Src::Rom);
        }
    }

    fn push(&self, p: &mut Path, f: &mut Found, hi: B, lo: B) {
        match p.sp {
            Sp::Abs(s) => {
                let s = s.wrapping_sub(2);
                p.sp = Sp::Abs(s);
                self.write(p, f, Some(s.wrapping_add(1)), hi);
                self.write(p, f, Some(s), lo);
            }
            Sp::Rel(s) => {
                let s = s.wrapping_sub(2);
                p.sp = Sp::Rel(s);
                Self::put(p, STACK_KEY | u32::from(s.wrapping_add(1)), hi);
                Self::put(p, STACK_KEY | u32::from(s), lo);
            }
            Sp::Unknown => {}
        }
    }

    fn pop(&self, p: &mut Path) -> (B, B) {
        match p.sp {
            Sp::Abs(s) => {
                p.sp = Sp::Abs(s.wrapping_add(2));
                (self.read(p, Some(s.wrapping_add(1))), self.read(p, Some(s)))
            }
            Sp::Rel(s) => {
                p.sp = Sp::Rel(s.wrapping_add(2));
                let g = |o: u16| p.mem.get(STACK_KEY | u32::from(o));
                (g(s.wrapping_add(1)), g(s))
            }
            Sp::Unknown => (U, U),
        }
    }
}

fn pair(p: &Path, h: usize, l: usize) -> Option<u16> {
    Some(u16::from_be_bytes([p.r[h].v?, p.r[l].v?]))
}

fn set_pair(p: &mut Path, h: usize, l: usize, v: Option<u16>) {
    let [a, b] = v.map_or([None, None], |v| v.to_be_bytes().map(Some));
    p.r[h] = calc(a);
    p.r[l] = calc(b);
}

fn rp(i: u8) -> (usize, usize) {
    match i & 3 {
        0 => (0, 1),
        1 => (2, 3),
        _ => (RH, RL),
    }
}

fn get_rr(p: &Path, i: u8) -> Option<u16> {
    if i & 3 == 3 {
        match p.sp {
            Sp::Abs(s) => Some(s),
            _ => None,
        }
    } else {
        let (h, l) = rp(i);
        pair(p, h, l)
    }
}

fn flags_byte(p: &Path) -> Option<u8> {
    let b = |f: Option<bool>, s: u8| f.map(|f| u8::from(f) << s);
    Some(b(p.z, 7)? | b(p.n, 6)? | b(p.h, 5)? | b(p.c, 4)?)
}

fn cond(p: &Path, cc: u8) -> Option<bool> {
    match cc & 3 {
        0 => p.z.map(|z| !z),
        1 => p.z,
        2 => p.c.map(|c| !c),
        _ => p.c,
    }
}

fn alu(p: &mut Path, op: u8, x: Option<u8>, same: bool) {
    let a = p.r[RA].v;
    if same && matches!(op, 2 | 5 | 7) {
        if op != 7 {
            p.r[RA] = calc(Some(0));
        }
        (p.z, p.n, p.h, p.c) = (Some(true), Some(op != 5), Some(false), Some(false));
        return;
    }
    let (r, n, h, c) = match op {
        0 | 1 | 2 | 3 | 7 => {
            let sub = matches!(op, 2 | 3 | 7);
            let cin = if matches!(op, 1 | 3) {
                p.c
            } else {
                Some(false)
            };
            match (a, x, cin) {
                (Some(a), Some(x), Some(ci)) => {
                    let ci = u8::from(ci);
                    if sub {
                        let r = a.wrapping_sub(x).wrapping_sub(ci);
                        let h = (a & 15) < (x & 15) + ci;
                        let c = u16::from(a) < u16::from(x) + u16::from(ci);
                        (Some(r), true, Some(h), Some(c))
                    } else {
                        let r = a.wrapping_add(x).wrapping_add(ci);
                        let h = (a & 15) + (x & 15) + ci > 15;
                        let c = u16::from(a) + u16::from(x) + u16::from(ci) > 255;
                        (Some(r), false, Some(h), Some(c))
                    }
                }
                _ => (None, sub, None, None),
            }
        }
        4 => {
            let r = match (a, x) {
                (Some(a), Some(x)) => Some(a & x),
                (Some(0), _) | (_, Some(0)) => Some(0),
                _ => None,
            };
            (r, false, Some(true), Some(false))
        }
        5 => (
            a.zip(x).map(|(a, x)| a ^ x),
            false,
            Some(false),
            Some(false),
        ),
        _ => {
            let r = match (a, x) {
                (Some(a), Some(x)) => Some(a | x),
                (Some(0xFF), _) | (_, Some(0xFF)) => Some(0xFF),
                _ => None,
            };
            (r, false, Some(false), Some(false))
        }
    };
    p.z = r.map(|r| r == 0);
    (p.n, p.h, p.c) = (Some(n), h, c);
    if op != 7 {
        p.r[RA] = calc(r);
    }
}

fn cb_op(p: &mut Path, op: u8, v: Option<u8>) -> Option<Option<u8>> {
    let b = (op >> 3) & 7;
    match op >> 6 {
        1 => {
            p.z = v.map(|v| v & (1 << b) == 0);
            (p.n, p.h) = (Some(false), Some(true));
            None
        }
        2 => Some(v.map(|v| v & !(1 << b))),
        3 => Some(v.map(|v| v | (1 << b))),
        _ => {
            let c = p.c;
            let (r, co) = match (b, v) {
                (0, Some(v)) => (Some(v.rotate_left(1)), Some(v & 0x80 != 0)),
                (1, Some(v)) => (Some(v.rotate_right(1)), Some(v & 1 != 0)),
                (2, Some(v)) => (c.map(|c| v << 1 | u8::from(c)), Some(v & 0x80 != 0)),
                (3, Some(v)) => (c.map(|c| v >> 1 | u8::from(c) << 7), Some(v & 1 != 0)),
                (4, Some(v)) => (Some(v << 1), Some(v & 0x80 != 0)),
                (5, Some(v)) => (Some(((v as i8) >> 1) as u8), Some(v & 1 != 0)),
                (6, Some(v)) => (Some(v.rotate_left(4)), Some(false)),
                (7, Some(v)) => (Some(v >> 1), Some(v & 1 != 0)),
                (6, None) => (None, Some(false)),
                _ => (None, None),
            };
            p.z = r.map(|r| r == 0);
            (p.n, p.h, p.c) = (Some(false), Some(false), co);
            Some(r)
        }
    }
}

enum Flow {
    Next,
    Halt,
    Ei,
    Jump(Option<u16>),
    Branch(Option<bool>, Option<u16>),
    Call(Option<bool>, Option<u16>),
    Ret(Option<bool>),
    Stop,
}

fn state_hash(p: &Path) -> u64 {
    let mut h = mix(u64::from(p.pc) ^ 0x5151);
    for (i, b) in p.r.iter().enumerate() {
        h = mix(h ^ bhash(*b) << (i % 4));
    }
    let f = |f: Option<bool>| f.map_or(2, u64::from);
    h = mix(h ^ f(p.z) ^ f(p.n) << 2 ^ f(p.h) << 4 ^ f(p.c) << 6);
    let sp = match p.sp {
        Sp::Abs(s) => u64::from(s),
        Sp::Rel(s) => u64::from(s) | 1 << 17,
        Sp::Unknown => 1 << 18,
    };
    h = mix(h ^ sp << 8);
    h = mix(h ^ p.lo.map_or(0x000F_FFFF, u64::from) ^ p.hi.map_or(0x1FF, u64::from) << 20);
    mix(h ^ p.svbk.map_or(9, u64::from) << 40 ^ p.memh)
}

impl Machine<'_> {
    /// One instruction; `None` when the opcode is not known.
    fn step(&self, p: &mut Path, f: &mut Found) -> Option<Flow> {
        let pc = p.pc;
        let opb = self.read(p, Some(pc));
        let op = opb.v?;
        if pc >= 0x8000 && !is_ram(pc) {
            return None;
        }
        let len = if op == 0xCB {
            2
        } else {
            BASE[usize::from(op)].length
        };
        let i1 = self.read(p, Some(pc.wrapping_add(1)));
        let i2 = self.read(p, Some(pc.wrapping_add(2)));
        let imm8 = B {
            v: i1.v,
            s: Src::Imm,
        };
        let imm16 = i1.v.zip(i2.v).map(|(l, h)| u16::from_le_bytes([l, h]));
        if pc >= 0x8000 {
            let key = if op == 0xCB {
                i1.v.map(|c| 0x100 | u16::from(c))
            } else {
                Some(u16::from(op))
            };
            if let Some(key) = key {
                let class = if opb.s == Src::Rom {
                    RamClass::Copied
                } else {
                    RamClass::Generated
                };
                let e = f.ran.entry((pc, key)).or_insert(class);
                *e = (*e).min(class);
            }
        }
        p.pc = pc.wrapping_add(u16::from(len));
        let next = p.pc;
        let hl = pair(p, RH, RL);
        let x = (op >> 3) & 7;
        let z = op & 7;
        let rd = |m: &Self, p: &Path, r: u8| -> B {
            if r == 6 {
                m.read(p, pair(p, RH, RL))
            } else {
                p.r[usize::from(r)]
            }
        };
        let flow = match op {
            0x00 | 0x10 => Flow::Next,
            0x76 => Flow::Halt,
            0xF3 => {
                p.ime = Some(false);
                Flow::Next
            }
            0xFB => {
                p.ime = Some(true);
                Flow::Ei
            }
            0x40..=0x7F => {
                let v = rd(self, p, z);
                if x == 6 {
                    self.write(p, f, hl, v);
                } else {
                    p.r[usize::from(x)] = v;
                }
                Flow::Next
            }
            0x80..=0xBF => {
                let v = rd(self, p, z).v;
                alu(p, x, v, z == 7);
                Flow::Next
            }
            _ if op & 0xC7 == 0x06 => {
                if x == 6 {
                    self.write(p, f, hl, imm8);
                } else {
                    p.r[usize::from(x)] = imm8;
                }
                Flow::Next
            }
            _ if op & 0xC7 == 0x04 || op & 0xC7 == 0x05 => {
                let v = rd(self, p, x).v;
                let dec = op & 1 == 1;
                let r = v.map(|v| {
                    if dec {
                        v.wrapping_sub(1)
                    } else {
                        v.wrapping_add(1)
                    }
                });
                p.z = r.map(|r| r == 0);
                p.n = Some(dec);
                p.h = v.map(|v| if dec { v & 15 == 0 } else { v & 15 == 15 });
                if x == 6 {
                    self.write(p, f, hl, calc(r));
                } else {
                    p.r[usize::from(x)] = calc(r);
                }
                Flow::Next
            }
            _ if op & 0xCF == 0x01 => {
                let i = op >> 4;
                if i == 3 {
                    p.sp = imm16.map_or(Sp::Unknown, Sp::Abs);
                } else {
                    let (h, l) = rp(i);
                    p.r[h] = B { v: i2.v, s: imm8.s };
                    p.r[l] = imm8;
                }
                Flow::Next
            }
            _ if op & 0xCF == 0x03 || op & 0xCF == 0x0B => {
                let i = op >> 4;
                let d: u16 = if op & 8 != 0 { 0xFFFF } else { 1 };
                if i == 3 {
                    p.sp = match p.sp {
                        Sp::Abs(s) => Sp::Abs(s.wrapping_add(d)),
                        Sp::Rel(s) => Sp::Rel(s.wrapping_add(d)),
                        Sp::Unknown => Sp::Unknown,
                    };
                } else {
                    let (h, l) = rp(i);
                    let lo = p.r[l].v;
                    match (pair(p, h, l), lo) {
                        (Some(v), _) => set_pair(p, h, l, Some(v.wrapping_add(d))),
                        (None, Some(lo)) if (d == 1 && lo != 0xFF) || (d != 1 && lo != 0) => {
                            p.r[l] = calc(Some(lo.wrapping_add(d as u8)));
                        }
                        _ => set_pair(p, h, l, None),
                    }
                }
                Flow::Next
            }
            _ if op & 0xCF == 0x09 => {
                let r = get_rr(p, op >> 4);
                match (hl, r) {
                    (Some(a), Some(b)) => {
                        let (s, c) = a.overflowing_add(b);
                        p.h = Some((a & 0xFFF) + (b & 0xFFF) > 0xFFF);
                        p.c = Some(c);
                        set_pair(p, RH, RL, Some(s));
                    }
                    _ => {
                        (p.h, p.c) = (None, None);
                        set_pair(p, RH, RL, None);
                    }
                }
                p.n = Some(false);
                Flow::Next
            }
            0x02 | 0x12 => {
                let (h, l) = rp(op >> 4);
                let a = pair(p, h, l);
                let v = p.r[RA];
                self.write(p, f, a, v);
                Flow::Next
            }
            0x0A | 0x1A => {
                let (h, l) = rp(op >> 4);
                p.r[RA] = self.read(p, pair(p, h, l));
                Flow::Next
            }
            0x22 | 0x32 | 0x2A | 0x3A => {
                if op & 8 != 0 {
                    p.r[RA] = self.read(p, hl);
                } else {
                    let v = p.r[RA];
                    self.write(p, f, hl, v);
                }
                let d: u16 = if op & 0x10 != 0 { 0xFFFF } else { 1 };
                set_pair(p, RH, RL, hl.map(|a| a.wrapping_add(d)));
                Flow::Next
            }
            0x07 | 0x0F | 0x17 | 0x1F => {
                let v = p.r[RA].v;
                let r = cb_op(p, x << 3, v).flatten();
                p.r[RA] = calc(r);
                p.z = Some(false);
                Flow::Next
            }
            0x27 => {
                let r = match (p.r[RA].v, p.n, p.h, p.c) {
                    (Some(a), Some(n), Some(h), Some(c)) => {
                        let mut adj = 0u8;
                        let mut cy = c;
                        if h || (!n && a & 15 > 9) {
                            adj |= 6;
                        }
                        if c || (!n && a > 0x99) {
                            adj |= 0x60;
                            cy = true;
                        }
                        let r = if n {
                            a.wrapping_sub(adj)
                        } else {
                            a.wrapping_add(adj)
                        };
                        p.c = Some(cy);
                        Some(r)
                    }
                    _ => {
                        p.c = None;
                        None
                    }
                };
                p.z = r.map(|r| r == 0);
                p.h = Some(false);
                p.r[RA] = calc(r);
                Flow::Next
            }
            0x2F => {
                p.r[RA] = calc(p.r[RA].v.map(|a| !a));
                (p.n, p.h) = (Some(true), Some(true));
                Flow::Next
            }
            0x37 | 0x3F => {
                p.c = if op == 0x37 {
                    Some(true)
                } else {
                    p.c.map(|c| !c)
                };
                (p.n, p.h) = (Some(false), Some(false));
                Flow::Next
            }
            0x08 => {
                let (lo, hi) = match p.sp {
                    Sp::Abs(s) => {
                        let [h, l] = s.to_be_bytes();
                        (calc(Some(l)), calc(Some(h)))
                    }
                    _ => (U, U),
                };
                self.write(p, f, imm16, lo);
                self.write(p, f, imm16.map(|a| a.wrapping_add(1)), hi);
                Flow::Next
            }
            0x18 => Flow::Jump(i1.v.map(|o| next.wrapping_add_signed(i16::from(o as i8)))),
            0x20 | 0x28 | 0x30 | 0x38 => Flow::Branch(
                cond(p, x - 4),
                i1.v.map(|o| next.wrapping_add_signed(i16::from(o as i8))),
            ),
            0xC3 => Flow::Jump(imm16),
            0xC2 | 0xCA | 0xD2 | 0xDA => Flow::Branch(cond(p, x), imm16),
            0xCD => Flow::Call(Some(true), imm16),
            0xC4 | 0xCC | 0xD4 | 0xDC => Flow::Call(cond(p, x), imm16),
            _ if op & 0xC7 == 0xC7 => Flow::Call(Some(true), Some(u16::from(op & 0x38))),
            0xC9 => Flow::Ret(Some(true)),
            0xD9 => {
                p.ime = Some(true);
                Flow::Ret(Some(true))
            }
            0xC0 | 0xC8 | 0xD0 | 0xD8 => Flow::Ret(cond(p, x)),
            0xE9 => Flow::Jump(hl),
            _ if op & 0xCF == 0xC5 => {
                let i = (op >> 4) & 3;
                let (h, l) = if i == 3 {
                    (p.r[RA], calc(flags_byte(p)))
                } else {
                    let (h, l) = rp(i);
                    (p.r[h], p.r[l])
                };
                self.push(p, f, h, l);
                Flow::Next
            }
            _ if op & 0xCF == 0xC1 => {
                let i = (op >> 4) & 3;
                let (h, l) = self.pop(p);
                if i == 3 {
                    p.r[RA] = h;
                    let fl = |s: u8| l.v.map(|v| v & (1 << s) != 0);
                    (p.z, p.n, p.h, p.c) = (fl(7), fl(6), fl(5), fl(4));
                } else {
                    let (rh, rl) = rp(i);
                    p.r[rh] = h;
                    p.r[rl] = l;
                }
                Flow::Next
            }
            _ if op & 0xC7 == 0xC6 => {
                alu(p, x, imm8.v, false);
                Flow::Next
            }
            0xE0 | 0xF0 | 0xE2 | 0xF2 | 0xEA | 0xFA => {
                let a = match op {
                    0xE0 | 0xF0 => i1.v.map(|o| 0xFF00 | u16::from(o)),
                    0xE2 | 0xF2 => p.r[1].v.map(|o| 0xFF00 | u16::from(o)),
                    _ => imm16,
                };
                if op & 0x10 != 0 {
                    p.r[RA] = self.read(p, a);
                } else {
                    let v = p.r[RA];
                    self.write(p, f, a, v);
                }
                Flow::Next
            }
            0xE8 | 0xF8 => {
                let e = i1.v.map(|o| o as i8);
                let r = match (p.sp, e) {
                    (Sp::Abs(s), Some(e)) => {
                        let u = u16::from(e as u8);
                        p.h = Some((s & 15) + (u & 15) > 15);
                        p.c = Some((s & 255) + (u & 255) > 255);
                        Sp::Abs(s.wrapping_add_signed(i16::from(e)))
                    }
                    (Sp::Rel(s), Some(e)) => {
                        (p.h, p.c) = (None, None);
                        Sp::Rel(s.wrapping_add_signed(i16::from(e)))
                    }
                    _ => {
                        (p.h, p.c) = (None, None);
                        Sp::Unknown
                    }
                };
                (p.z, p.n) = (Some(false), Some(false));
                if op == 0xE8 {
                    p.sp = r;
                } else {
                    let v = match r {
                        Sp::Abs(s) => Some(s),
                        _ => None,
                    };
                    set_pair(p, RH, RL, v);
                }
                Flow::Next
            }
            0xF9 => {
                p.sp = hl.map_or(Sp::Unknown, Sp::Abs);
                Flow::Next
            }
            0xCB => {
                let c = i1.v?;
                let r = c & 7;
                let v = rd(self, p, r).v;
                if let Some(out) = cb_op(p, c, v) {
                    if r == 6 {
                        self.write(p, f, hl, calc(out));
                    } else {
                        p.r[usize::from(r)] = calc(out);
                    }
                }
                Flow::Next
            }
            _ => Flow::Stop,
        };
        Some(flow)
    }

    /// Calls `t` from `p.pc`, which the callee returns to.
    fn call(&self, p: &mut Path, f: &mut Found, t: u16, resumable: bool) {
        if p.frames.len() >= MAX_FRAMES {
            return;
        }
        p.frames.push(Frame {
            r: p.r,
            ret: p.pc,
            sp: p.sp,
            lo: p.lo,
            hi: p.hi,
            svbk: p.svbk,
            at: p.steps,
            resumable,
        });
        let [h, l] = p.pc.to_be_bytes();
        self.push(p, f, calc(Some(h)), calc(Some(l)));
        p.pc = t;
    }

    fn enter(&self, p: &Path, f: &mut Found, from: u16, to: u16) {
        if from < 0x8000 && to >= 0x8000 && is_ram(to) && self.read(p, Some(from)).v != Some(0xE9) {
            f.entries.insert((norm(to), self.bucket(p, norm(to))));
        }
    }

    fn run_root(&self, start: Path, f: &mut Found) {
        let mut seen: FxSet<u64> = FxSet::default();
        let mut work = std::collections::VecDeque::from([start]);
        let mut forks = 0u32;
        let fork = |p: &Path| {
            let mut q = p.clone();
            q.limit = q.steps.saturating_add(FORK_STEPS);
            q.main = false;
            q.launch = None;
            q
        };
        let mut launches = 0usize;
        let again = |seen: &mut FxSet<u64>, p: &Path| {
            if seen.len() >= MAX_SEEN {
                seen.clear();
            }
            !seen.insert(state_hash(p))
        };
        let queue = |work: &mut std::collections::VecDeque<Path>, q: Path| {
            if work.len() >= MAX_PENDING {
                work.pop_front();
            }
            work.push_back(q);
        };
        while let Some(mut p) = work.pop_back() {
            loop {
                if p.steps >= p.limit
                    || p.launch
                        .as_ref()
                        .is_some_and(|l| p.steps - l.start >= LAUNCH_STEPS)
                    || (!p.main && forks >= FORKS_STEPS)
                {
                    break;
                }
                if p.frames
                    .last()
                    .is_some_and(|fr| p.steps - fr.at > CALL_STEPS)
                    && !p.resume()
                {
                    break;
                }
                if !self.decoded(&p) {
                    if p.resume() {
                        continue;
                    }
                    break;
                }
                if !p.main {
                    forks += 1;
                }
                p.steps += 1;
                let pc = p.pc;
                let op = self.read(&p, Some(pc)).v;
                let ime = p.ime == Some(true);
                let Some(flow) = self.step(&mut p, f) else {
                    if p.resume() {
                        continue;
                    }
                    break;
                };
                let goto = |p: &mut Path, t: u16, f: &mut Found| {
                    self.enter(p, f, pc, t);
                    p.pc = t;
                };
                let jumped = |p: &mut Path, t: u16, f: &mut Found| {
                    if pc < 0x8000 && is_ram(t) && p.main && p.launch.is_none() {
                        p.launch = Some(Launch {
                            exits: self.image(p, f, t),
                            start: p.steps,
                            limit: p.limit,
                        });
                        p.limit = p.limit.saturating_add(LAUNCH_STEPS);
                    } else if pc >= 0x8000 && t < 0x8000 {
                        p.launch = None;
                        p.free = true;
                    }
                };
                let dead = match flow {
                    Flow::Next | Flow::Branch(Some(false), _) => false,
                    Flow::Halt | Flow::Ei => {
                        let ie = p.ie.unwrap_or(0) & 0x1F;
                        let sure = p.ime == Some(true);
                        if p.ime != Some(false) && ie != 0 && !(ime && matches!(flow, Flow::Ei)) {
                            if sure && matches!(flow, Flow::Ei) {
                                queue(&mut work, fork(&p));
                            }
                            let vectors = (0..5u16).filter(|i| ie & (1 << i) != 0);
                            for (k, v) in vectors.map(|i| 0x40 + 8 * i).enumerate() {
                                if sure && k == 0 {
                                    p.ime = Some(false);
                                    self.call(&mut p, f, v, true);
                                } else {
                                    let mut q = fork(&p);
                                    q.ime = Some(false);
                                    self.call(&mut q, f, v, true);
                                    queue(&mut work, q);
                                }
                            }
                        }
                        false
                    }
                    Flow::Stop | Flow::Jump(None) | Flow::Branch(Some(true), None) => true,
                    Flow::Jump(Some(t)) | Flow::Branch(Some(true), Some(t)) => {
                        goto(&mut p, t, f);
                        jumped(&mut p, t, f);
                        if t <= pc && again(&mut seen, &p) {
                            break;
                        }
                        false
                    }
                    Flow::Branch(None, t) => {
                        if again(&mut seen, &p) {
                            break;
                        }
                        if let Some(t) = t {
                            let mut q = fork(&p);
                            goto(&mut q, t, f);
                            queue(&mut work, q);
                        }
                        false
                    }
                    Flow::Call(c, t) => {
                        if c.is_none() {
                            if again(&mut seen, &p) {
                                break;
                            }
                            queue(&mut work, fork(&p));
                        }
                        if c == Some(false) {
                            continue;
                        }
                        match t {
                            Some(t) if p.frames.len() < MAX_FRAMES => {
                                self.enter(&p, f, pc, t);
                                self.call(&mut p, f, t, op.is_some_and(|o| o & 0xC7 != 0xC7));
                                false
                            }
                            _ => {
                                p.r = [U; 8];
                                (p.z, p.n, p.h, p.c) = (None, None, None, None);
                                false
                            }
                        }
                    }
                    Flow::Ret(c) => {
                        if c == Some(false) {
                            continue;
                        }
                        if c.is_none() {
                            if again(&mut seen, &p) {
                                break;
                            }
                            queue(&mut work, fork(&p));
                        }
                        let (h, l) = self.pop(&mut p);
                        match h.v.zip(l.v).map(|(h, l)| u16::from_be_bytes([h, l])) {
                            Some(t) => {
                                let popped = |fr: &Frame| match (fr.sp, p.sp) {
                                    (Sp::Abs(a), Sp::Abs(b)) | (Sp::Rel(a), Sp::Rel(b)) => {
                                        b.wrapping_sub(a) < 0x8000
                                    }
                                    _ => fr.ret == t,
                                };
                                let keep = p
                                    .frames
                                    .iter()
                                    .rposition(|fr| !popped(fr))
                                    .map_or(0, |i| i + 1);
                                if let Some(fr) = p.frames.get(keep).copied() {
                                    if fr.ret == t
                                        && (0x4000..0x8000).contains(&t)
                                        && self.bank(&p).is_none()
                                    {
                                        p.lo = fr.lo;
                                        p.hi = fr.hi;
                                    }
                                    p.frames.truncate(keep);
                                }
                                if let Some(fr) = p.frames.last_mut() {
                                    fr.at = p.steps;
                                }
                                if pc >= 0x8000 && t < 0x8000 {
                                    p.launch = None;
                                    p.free = true;
                                }
                                p.pc = t;
                                false
                            }
                            None => true,
                        }
                    }
                };
                if dead && !p.resume() {
                    break;
                }
            }
            if let Some(l) = p
                .launch
                .take()
                .filter(|_| p.main && launches < MAX_LAUNCHES)
            {
                launches += 1;
                for t in l.exits {
                    let mut q = p.clone();
                    q.r = [U; 8];
                    (q.z, q.n, q.h, q.c) = (None, None, None, None);
                    q.sp = Sp::Unknown;
                    q.pc = t;
                    q.limit = l.limit.saturating_add(p.steps - l.start);
                    q.frames.clear();
                    q.free = true;
                    q.launch = None;
                    work.push_back(q);
                }
            }
        }
    }

    /// Decodes the code `p` holds in RAM from `entry` through its known
    /// bytes, adds its instructions to `f`, and gives the ROM targets of
    /// its jumps.
    fn image(&self, p: &Path, f: &mut Found, entry: u16) -> Vec<u16> {
        let mut out = BTreeSet::new();
        let mut done: FxSet<u16> = FxSet::default();
        let mut todo = vec![entry];
        while let Some(a) = todo.pop() {
            if !is_ram(a) || done.len() >= MAX_PER_ENTRY || !done.insert(a) {
                continue;
            }
            let b = |o: u16| self.read(p, Some(a.wrapping_add(o))).v;
            let opb = self.read(p, Some(a));
            let Some(op) = opb.v else { continue };
            let (len, key) = if op == 0xCB {
                let Some(c) = b(1) else { continue };
                (2, 0x100 | u16::from(c))
            } else {
                (u16::from(BASE[usize::from(op)].length), u16::from(op))
            };
            let class = if opb.s == Src::Rom {
                RamClass::Copied
            } else {
                RamClass::Generated
            };
            let e = f.ran.entry((a, key)).or_insert(class);
            *e = (*e).min(class);
            let next = a.wrapping_add(len);
            let abs = b(1).zip(b(2)).map(|(l, h)| u16::from_le_bytes([l, h]));
            let rel = b(1).map(|o| next.wrapping_add_signed(i16::from(o as i8)));
            let (target, falls) = match op {
                0x18 => (rel, false),
                0x20 | 0x28 | 0x30 | 0x38 => (rel, true),
                0xC3 => (abs, false),
                0xC2 | 0xCA | 0xD2 | 0xDA | 0xC4 | 0xCC | 0xD4 | 0xDC | 0xCD => (abs, true),
                0xC9 | 0xD9 | 0xE9 | 0xD3 | 0xDB | 0xDD | 0xE3 | 0xE4 | 0xEB | 0xEC | 0xED
                | 0xF4 | 0xFC | 0xFD => (None, false),
                _ => (None, true),
            };
            if let Some(t) = target {
                if is_ram(t) {
                    todo.push(t);
                } else if t < 0x8000 && matches!(op, 0xC2 | 0xC3 | 0xCA | 0xD2 | 0xDA) {
                    out.insert(t);
                }
            }
            if falls {
                todo.push(next);
            }
        }
        out.into_iter().collect()
    }

    fn start(&self, at: Loc) -> Path {
        let (lo, hi) = if at.is_window() {
            let (lo, hi) = self.model.split(at.bank);
            (Some(lo), Some(hi))
        } else if at.addr == 0x100 {
            let (lo, hi) = self.model.reset();
            (Some(lo), Some(hi))
        } else {
            (None, None)
        };
        Path {
            r: [U; 8],
            z: None,
            n: None,
            h: None,
            c: None,
            sp: Sp::Rel(0),
            pc: at.addr,
            lo,
            hi,
            svbk: None,
            mem: Mem::default(),
            memh: 0,
            steps: 0,
            limit: ROOT_STEPS,
            frames: Vec::new(),
            ie: None,
            ime: None,
            main: true,
            free: false,
            launch: None,
        }
    }
}

/// The instructions `rom` will run from RAM: every function in `roots` is
/// run as the module documentation says, and the code is decoded from the
/// RAM transfer targets in `targets` and those the runs reached. A path
/// ends at a ROM location that is not in `code`, the instructions the
/// analysis decoded, as it has gone astray.
#[must_use]
pub(crate) fn predict(
    rom: &[u8],
    model: &BankModel,
    roots: &[Loc],
    code: &FxSet<Loc>,
    targets: &[u16],
) -> RamOps {
    let m = Machine {
        rom,
        model,
        cgb: rom.get(0x143).is_some_and(|c| c & 0x80 != 0),
        code,
    };
    let jobs = std::thread::available_parallelism()
        .map_or(1, std::num::NonZero::get)
        .clamp(1, 8);
    let next = std::sync::atomic::AtomicUsize::new(0);
    let found = std::thread::scope(|s| {
        let hs: Vec<_> = (0..jobs)
            .map(|_| {
                let (m, next) = (&m, &next);
                s.spawn(move || {
                    let mut f = Found::default();
                    while let Some(&r) =
                        roots.get(next.fetch_add(1, std::sync::atomic::Ordering::Relaxed))
                    {
                        m.run_root(m.start(r), &mut f);
                    }
                    f
                })
            })
            .collect();
        let mut all = Found::default();
        for h in hs {
            if let Ok(f) = h.join() {
                all.join(f);
            }
        }
        all
    });
    let mut entries: BTreeSet<(u16, u8)> = found.entries.iter().copied().collect();
    for &t in targets {
        let t = norm(t);
        if is_ram(t) && !entries.iter().any(|e| e.0 == t) {
            entries.insert((t, UNKNOWN_BANK));
        }
    }
    let called = called(&found.stores, m.cgb);
    entries.extend(called);
    let mut ops = decode(&found.stores, &entries, m.cgb);
    let mut all: BTreeMap<(u16, u16), RamClass> =
        ops.ops.iter().map(|o| ((o.addr, o.key), o.class)).collect();
    for (&k, &c) in &found.ran {
        all.entry(k).or_insert(c);
    }
    ops.ops = all
        .into_iter()
        .map(|((addr, key), class)| RamOp { addr, key, class })
        .collect();
    ops
}

/// The RAM targets of calls and jumps copied to RAM: an address whose value
/// set holds a call or jump opcode read from ROM, followed by two addresses
/// with one value each, read from ROM, that name a RAM address holding a
/// byte read from ROM.
fn called(stores: &FxMap<(u16, u8), Cell>, cgb: bool) -> Vec<(u16, u8)> {
    let mut out = Vec::new();
    for (&(a, bucket), c) in stores {
        if !c.values().any(|v| {
            matches!(
                v,
                0xC2 | 0xC3 | 0xC4 | 0xCA | 0xCC | 0xCD | 0xD2 | 0xD4 | 0xDA | 0xDC
            ) && c.class(v) == RamClass::Copied
        }) {
            continue;
        }
        let one = |o: u16| {
            let c = cell_at(stores, a.wrapping_add(o), bucket, cgb);
            let v = c.values().next()?;
            (c.count() == 1 && c.class(v) == RamClass::Copied).then_some(v)
        };
        let (Some(l), Some(h)) = (one(1), one(2)) else {
            continue;
        };
        let t = norm(u16::from_be_bytes([h, l]));
        let target = cell_at(stores, t, bucket, cgb);
        if is_ram(t) && target.values().any(|v| target.class(v) == RamClass::Copied) {
            out.push((t, bucket));
        }
    }
    out
}

fn cell_at(stores: &FxMap<(u16, u8), Cell>, a: u16, bucket: u8, cgb: bool) -> Cell {
    let a = norm(a);
    let mut c = Cell::default();
    let get = |b: u8, c: &mut Cell| {
        if let Some(x) = stores.get(&(a, b)) {
            c.join(x);
        }
    };
    if !banked(a) {
        get(0, &mut c);
    } else if !cgb {
        get(1, &mut c);
    } else if bucket == UNKNOWN_BANK {
        for b in 1..=UNKNOWN_BANK {
            get(b, &mut c);
        }
    } else {
        get(bucket, &mut c);
        get(UNKNOWN_BANK, &mut c);
    }
    c
}

fn decode(stores: &FxMap<(u16, u8), Cell>, entries: &BTreeSet<(u16, u8)>, cgb: bool) -> RamOps {
    let mut ops: BTreeMap<(u16, u16), RamClass> = BTreeMap::new();
    let mut wide = BTreeSet::new();
    let mut done: FxSet<(u16, u8)> = FxSet::default();
    for &(entry, bucket) in entries {
        let mut todo = vec![(entry, 0u32)];
        let mut n = 0usize;
        while let Some((a, nops)) = todo.pop() {
            if n >= MAX_PER_ENTRY || !is_ram(a) || !done.insert((a, bucket)) {
                continue;
            }
            let cell = cell_at(stores, a, bucket, cgb);
            if cell.count() == 0 {
                continue;
            }
            if cell.count() > MAX_KEYS {
                wide.insert(a);
                continue;
            }
            let at = |o: u16| cell_at(stores, a.wrapping_add(o), bucket, cgb);
            for op in cell.values() {
                let class = cell.class(op);
                if op == 0xCB {
                    let c = at(1);
                    if c.count() > MAX_KEYS {
                        wide.insert(a.wrapping_add(1));
                        continue;
                    }
                    for v in c.values() {
                        n += 1;
                        let k = (a, 0x100 | u16::from(v));
                        ops.entry(k).or_insert(class);
                    }
                    todo.push((a.wrapping_add(2), 0));
                    continue;
                }
                n += 1;
                ops.entry((a, u16::from(op))).or_insert(class);
                let len = u16::from(BASE[usize::from(op)].length);
                let next = a.wrapping_add(len);
                let rel = |todo: &mut Vec<(u16, u32)>| {
                    for o in at(1).values() {
                        todo.push((next.wrapping_add_signed(i16::from(o as i8)), 0));
                    }
                };
                let abs = |todo: &mut Vec<(u16, u32)>| {
                    let (l, h) = (at(1), at(2));
                    if l.count() * h.count() <= MAX_KEYS {
                        for hv in h.values() {
                            for lv in l.values() {
                                todo.push((u16::from_be_bytes([hv, lv]), 0));
                            }
                        }
                    }
                };
                let falls = match op {
                    0x18 => {
                        rel(&mut todo);
                        false
                    }
                    0x20 | 0x28 | 0x30 | 0x38 => {
                        rel(&mut todo);
                        true
                    }
                    0xC3 => {
                        abs(&mut todo);
                        false
                    }
                    0xC2 | 0xCA | 0xD2 | 0xDA | 0xC4 | 0xCC | 0xD4 | 0xDC | 0xCD => {
                        abs(&mut todo);
                        true
                    }
                    0xC9 | 0xD9 | 0xE9 => false,
                    0xD3 | 0xDB | 0xDD | 0xE3 | 0xE4 | 0xEB | 0xEC | 0xED | 0xF4 | 0xFC | 0xFD => {
                        false
                    }
                    _ => true,
                };
                if falls && (!matches!(op, 0x00 | 0xFF) || cell.count() == 1) {
                    let run = if op == 0 && class == RamClass::Generated {
                        nops + 1
                    } else {
                        0
                    };
                    if run <= NOP_RUN {
                        todo.push((next, run));
                    }
                }
            }
        }
    }
    RamOps {
        ops: ops
            .into_iter()
            .map(|((addr, key), class)| RamOp { addr, key, class })
            .collect(),
        entries: entries
            .iter()
            .map(|e| e.0)
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect(),
        wide: wide.into_iter().collect(),
    }
}
