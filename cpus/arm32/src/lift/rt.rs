//! What the generated ARMv4T templates call: the guest state as the
//! instruction sees it, the condition guard, branch exits and the bus
//! events that become the block's cycle cost.

use relift_ir::{BinOp, Builder, CmpOp, Exit, Flow, Next, Reg, Value, Width};

use crate::regs::{self, Bank, Mode};

/// The slow-path code of a guard that leaves when the instruction's
/// condition fails: the instruction does nothing and costs 1S.
pub const COND_FAIL: u32 = 0x434f_4e44;

/// Where a branch goes.
#[derive(Clone, Copy, Debug)]
pub enum Target {
    /// A known address.
    Const(u64),
    /// A computed address.
    Dynamic(Value),
}

/// The kind of a branch.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A plain jump.
    Jump,
    /// A call returning to the next instruction.
    Call,
    /// A return.
    Return,
}

/// One bus cycle of the instruction, in order (GBATEK "ARM CPU Instruction
/// Cycle Times"): N and S accesses of code or data and internal cycles.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bus {
    /// A code fetch; `true` when sequential.
    Code(bool, u32),
    /// A data read; `true` when sequential, with the width in bytes.
    Read(bool, u32),
    /// A data write.
    Write(bool, u32),
    /// Internal cycles.
    I(u32),
    /// Internal cycles taken only at run time (multiply early termination).
    IDyn(u32),
}

/// The guest state of one instruction while its template runs; see the
/// A64 `St` for why writes are kept until [`St::finish`].
#[derive(Debug)]
pub struct St {
    cur: Vec<Option<Value>>,
    exit: Option<(Next, Kind)>,
    thumb: bool,
    /// The bus events so far.
    pub bus: Vec<Bus>,
    force_n: bool,
}

impl St {
    /// A state that has written nothing.
    pub fn new(thumb: bool) -> St {
        St {
            cur: Vec::new(),
            exit: None,
            thumb,
            bus: Vec::new(),
            force_n: false,
        }
    }

    fn len(&self) -> u64 {
        if self.thumb { 2 } else { 4 }
    }

    /// The current value of `r`.
    pub fn get(&mut self, b: &mut Builder, r: Reg) -> Value {
        if let Some(Some(v)) = self.cur.get(r.index()) {
            return *v;
        }
        b.get(r)
    }

    /// Writes `v` to `r`, or under `g` keeps the old value when `g` is 0.
    pub fn set(&mut self, b: &mut Builder, r: Reg, v: Value, g: Option<Value>) {
        let v = match g {
            Some(g) => {
                let old = self.get(b, r);
                b.select(g, v, old)
            }
            None => v,
        };
        if self.cur.len() <= r.index() {
            self.cur.resize(r.index() + 1, None);
        }
        self.cur[r.index()] = Some(v);
    }

    /// `R[n]` of the current mode, n from 0 to 14 (the overlay's `R_read`
    /// gives the PC values).
    /// A template may compute the read of R15 on a path a static condition
    /// then discards; it reads as 0.
    pub fn r_read(&mut self, b: &mut Builder, n: u32) -> Value {
        if n == 15 {
            return b.konst(Width::W32, 0);
        }
        self.get(b, regs::r(n))
    }

    /// Writes `R[n]`; a write of R15 is a jump.
    pub fn r_set(&mut self, b: &mut Builder, n: u32, v: Value, g: Option<Value>) {
        if n == 15 {
            self.branch(b, Target::Dynamic(v), Kind::Jump, g);
            return;
        }
        self.set(b, regs::r(n), v, g);
    }

    /// Writes the Thumb bit.
    pub fn set_thumb(&mut self, b: &mut Builder, v: Value, g: Option<Value>) {
        self.set(b, regs::T, v, g);
    }

    fn mode_is(&mut self, b: &mut Builder, m: Value, mode: Mode) -> Value {
        let k = b.konst(Width::W8, u128::from(mode.bits()));
        b.cmp(CmpOp::Eq, m, k)
    }

    fn any_mode(&mut self, b: &mut Builder, m: Value, modes: &[Mode]) -> Value {
        let mut acc = b.konst(Width::W1, 0);
        for &x in modes {
            let e = self.mode_is(b, m, x);
            acc = b.or(acc, e);
        }
        acc
    }

    fn and_guard(b: &mut Builder, c: Value, g: Option<Value>) -> Value {
        match g {
            Some(g) => b.and(c, g),
            None => c,
        }
    }

    /// Whether mode `m` keeps register `n` (8 to 14) in `bank`.
    fn in_bank(&mut self, b: &mut Builder, m: Value, n: u32, bank: Bank) -> Value {
        let modes: Vec<Mode> = Mode::ALL
            .into_iter()
            .filter(|x| x.bank(n) == bank)
            .collect();
        self.any_mode(b, m, &modes)
    }

    /// Whether the current mode has an SPSR.
    pub fn has_spsr(&mut self, b: &mut Builder) -> Value {
        let m = self.get(b, regs::M);
        let none = self.any_mode(b, m, &[Mode::Usr, Mode::Sys]);
        let one = b.konst(Width::W1, 1);
        b.xor(none, one)
    }

    /// Changes the mode bits to `new` (when `g` is 1): the current view of
    /// `r8` to `r14` goes to the banks of the old mode and comes from the
    /// banks of the new one.
    pub fn switch_mode(&mut self, b: &mut Builder, new: Value, g: Option<Value>) {
        let old = self.get(b, regs::M);
        let new = match g {
            Some(g) => b.select(g, new, old),
            None => new,
        };
        let one = b.konst(Width::W1, 1);
        for n in 8..15 {
            let cur = self.get(b, regs::r(n));
            let mut view = cur;
            for bank in Bank::ALL {
                let Some(slot) = bank.slot(n) else {
                    continue;
                };
                let was = self.in_bank(b, old, n, bank);
                let will = self.in_bank(b, new, n, bank);
                let not_will = b.xor(will, one);
                let not_was = b.xor(was, one);
                let save = b.and(was, not_will);
                let load = b.and(will, not_was);
                let kept = self.get(b, slot);
                view = b.select(load, kept, view);
                self.set(b, slot, cur, Some(save));
            }
            self.set(b, regs::r(n), view, None);
        }
        self.set(b, regs::M, new, None);
    }

    /// The CPSR.
    pub fn cpsr(&mut self, b: &mut Builder) -> Value {
        let mut acc = self.get(b, regs::PSR_RSVD);
        for (r, bit) in [
            (regs::N, 31),
            (regs::Z, 30),
            (regs::C, 29),
            (regs::V, 28),
            (regs::I, 7),
            (regs::F, 6),
            (regs::T, 5),
        ] {
            let v = self.get(b, r);
            let v = b.zext(v, Width::W32);
            let k = b.konst(Width::W32, bit);
            let v = b.bin(BinOp::Shl, v, k);
            acc = b.or(acc, v);
        }
        let m = self.get(b, regs::M);
        let m = b.zext(m, Width::W32);
        let k = b.konst(Width::W32, 0x1f);
        let m = b.and(m, k);
        b.or(acc, m)
    }

    /// The SPSR of the current mode; in user and system mode, which have
    /// none, the CPSR.
    pub fn spsr(&mut self, b: &mut Builder) -> Value {
        let mut acc = self.cpsr(b);
        let m = self.get(b, regs::M);
        for x in Mode::ALL {
            let Some(slot) = x.spsr() else {
                continue;
            };
            let is = self.mode_is(b, m, x);
            let v = self.get(b, slot);
            acc = b.select(is, v, acc);
        }
        acc
    }

    fn set_psr_bit(&mut self, b: &mut Builder, r: Reg, v: Value, bit: u32, g: Option<Value>) {
        let x = b.extract(v, bit, Width::W1);
        self.set(b, r, x, g);
    }

    fn set_flags_from(&mut self, b: &mut Builder, v: Value, g: Option<Value>) {
        for (r, bit) in [(regs::N, 31), (regs::Z, 30), (regs::C, 29), (regs::V, 28)] {
            self.set_psr_bit(b, r, v, bit, g);
        }
    }

    /// MSR to the CPSR with field mask `mask` (bit 0 c, bit 3 f): the f
    /// field writes the flags; the c field writes the mode and the
    /// interrupt masks outside user mode (not the Thumb bit). ARMv4T has no
    /// storage behind the x and s fields.
    pub fn write_cpsr(&mut self, b: &mut Builder, v: Value, mask: u32, g: Option<Value>) {
        if mask & 1 != 0 {
            let m = self.get(b, regs::M);
            let usr = self.mode_is(b, m, Mode::Usr);
            let one = b.konst(Width::W1, 1);
            let priv_ = b.xor(usr, one);
            let gg = Self::and_guard(b, priv_, g);
            self.set_psr_bit(b, regs::I, v, 7, Some(gg));
            self.set_psr_bit(b, regs::F, v, 6, Some(gg));
            let k = b.konst(Width::W32, 0x1f);
            let nm = b.and(v, k);
            let nm = b.trunc(nm, Width::W8);
            self.switch_mode(b, nm, Some(gg));
        }
        if mask & 8 != 0 {
            self.set_flags_from(b, v, g);
        }
    }

    /// MSR to the SPSR of the current mode with field mask `mask`; the
    /// mode field's bit 4 reads 1 after a write of the c field (measured on
    /// an AGB-001).
    pub fn write_spsr(&mut self, b: &mut Builder, v: Value, mask: u32, g: Option<Value>) {
        let mut keep = 0u128;
        if mask & 1 != 0 {
            keep |= 0xff;
        }
        if mask & 8 != 0 {
            keep |= 0xf000_0000;
        }
        let force = if mask & 1 != 0 { 0x10 } else { 0 };
        let m = self.get(b, regs::M);
        let kk = b.konst(Width::W32, keep);
        let nk = b.konst(Width::W32, !keep & 0xffff_ffff);
        let fk = b.konst(Width::W32, force);
        let part = b.and(v, kk);
        for x in Mode::ALL {
            let Some(slot) = x.spsr() else {
                continue;
            };
            let is = self.mode_is(b, m, x);
            let gg = Self::and_guard(b, is, g);
            let old = self.get(b, slot);
            let kept = b.and(old, nk);
            let new = b.or(kept, part);
            let new = b.or(new, fk);
            self.set(b, slot, new, Some(gg));
        }
    }

    /// CPSR := SPSR in a mode with an SPSR (keeping the Thumb bit set when
    /// `keep_t`); nothing in user and system mode.
    pub fn restore_cpsr(&mut self, b: &mut Builder, keep_t: bool, g: Option<Value>) {
        let has = self.has_spsr(b);
        let gg = Self::and_guard(b, has, g);
        let sp = self.spsr(b);
        self.set_flags_from(b, sp, Some(gg));
        self.set_psr_bit(b, regs::I, sp, 7, Some(gg));
        self.set_psr_bit(b, regs::F, sp, 6, Some(gg));
        if keep_t {
            let one = b.konst(Width::W1, 1);
            self.set(b, regs::T, one, Some(gg));
        } else {
            self.set_psr_bit(b, regs::T, sp, 5, Some(gg));
        }
        let k = b.konst(Width::W32, 0x0fff_ff00);
        let rsvd = b.and(sp, k);
        self.set(b, regs::PSR_RSVD, rsvd, Some(gg));
        let k = b.konst(Width::W32, 0x1f);
        let nm = b.and(sp, k);
        let nm = b.trunc(nm, Width::W8);
        self.switch_mode(b, nm, Some(gg));
    }

    /// `r[n]` of the user bank: the current `r[n]` in user and system mode
    /// and for `r0` to `r7`.
    pub fn user_read(&mut self, b: &mut Builder, n: u32) -> Value {
        let cur = self.r_read(b, n);
        let Some(slot) = Bank::Usr.slot(n) else {
            return cur;
        };
        let m = self.get(b, regs::M);
        let view = self.in_bank(b, m, n, Bank::Usr);
        let kept = self.get(b, slot);
        b.select(view, cur, kept)
    }

    /// Writes `r[n]` of the user bank.
    pub fn user_set(&mut self, b: &mut Builder, n: u32, v: Value, g: Option<Value>) {
        let Some(slot) = Bank::Usr.slot(n) else {
            self.r_set(b, n, v, g);
            return;
        };
        let m = self.get(b, regs::M);
        let view = self.in_bank(b, m, n, Bank::Usr);
        let one = b.konst(Width::W1, 1);
        let away = b.xor(view, one);
        let g1 = Self::and_guard(b, view, g);
        let g2 = Self::and_guard(b, away, g);
        self.set(b, regs::r(n), v, Some(g1));
        self.set(b, slot, v, Some(g2));
    }

    /// A branch to `addr` with the low bits the Thumb bit (as the
    /// instruction left it) ignores.
    pub fn branch_aligned(&mut self, b: &mut Builder, addr: Value, g: Option<Value>) {
        let t = self.get(b, regs::T);
        let k1 = b.konst(Width::W32, 0xffff_fffe);
        let k2 = b.konst(Width::W32, 0xffff_fffc);
        let mask = b.select(t, k1, k2);
        let target = b.and(addr, mask);
        self.branch(b, Target::Dynamic(target), Kind::Jump, g);
    }

    /// The software interrupt: SPSR_svc := CPSR, supervisor mode in ARM
    /// state with IRQs masked, `r14_svc` the next instruction, PC 0x08.
    pub fn svc(&mut self, b: &mut Builder, g: Option<Value>) {
        let old = self.cpsr(b);
        let svc = b.konst(Width::W8, u128::from(Mode::Svc.bits()));
        self.switch_mode(b, svc, g);
        let next = b.pc().wrapping_add(self.len());
        let lr = b.konst(Width::W32, u128::from(next));
        self.set(b, regs::r(14), lr, g);
        if let Some(slot) = Mode::Svc.spsr() {
            self.set(b, slot, old, g);
        }
        let zero = b.konst(Width::W1, 0);
        let one = b.konst(Width::W1, 1);
        self.set(b, regs::T, zero, g);
        self.set(b, regs::I, one, g);
        self.branch(b, Target::Const(0x08), Kind::Call, g);
    }

    /// Leaves for [`COND_FAIL`] unless condition `cond` (the four-bit A32
    /// field) holds; 1110 always holds and 1111 never does on ARMv4T.
    pub fn cond_guard(&mut self, b: &mut Builder, cond: u128) {
        let one = b.konst(Width::W1, 1);
        let holds = match cond >> 1 {
            _ if cond == 0b1110 => return,
            _ if cond == 0b1111 => b.konst(Width::W1, 0),
            0b000 => self.get(b, regs::Z),
            0b001 => self.get(b, regs::C),
            0b010 => self.get(b, regs::N),
            0b011 => self.get(b, regs::V),
            0b100 => {
                let c = self.get(b, regs::C);
                let z = self.get(b, regs::Z);
                let nz = b.bin(BinOp::Xor, z, one);
                b.and(c, nz)
            }
            0b101 => {
                let n = self.get(b, regs::N);
                let v = self.get(b, regs::V);
                b.cmp(CmpOp::Eq, n, v)
            }
            _ => {
                let n = self.get(b, regs::N);
                let v = self.get(b, regs::V);
                let z = self.get(b, regs::Z);
                let ge = b.cmp(CmpOp::Eq, n, v);
                let nz = b.bin(BinOp::Xor, z, one);
                b.and(ge, nz)
            }
        };
        let holds = if cond & 1 == 1 && cond != 0b1111 {
            b.bin(BinOp::Xor, holds, one)
        } else {
            holds
        };
        let fails = b.bin(BinOp::Xor, holds, one);
        b.guard(fails, COND_FAIL);
    }

    /// Records a data access of `size` bytes: the first of the instruction
    /// is nonsequential, the next ones sequential unless [`St::force_n`].
    pub fn data(&mut self, write: bool, size: u32) {
        let seq = !self.force_n
            && self
                .bus
                .iter()
                .any(|e| matches!(e, Bus::Read(..) | Bus::Write(..)));
        self.force_n = false;
        self.bus.push(if write {
            Bus::Write(seq, size)
        } else {
            Bus::Read(seq, size)
        });
    }

    /// Makes the next data access nonsequential (SWP's write).
    pub fn force_n(&mut self) {
        self.force_n = true;
    }

    /// Records `n` internal cycles.
    pub fn internal(&mut self, n: u32) {
        self.bus.push(Bus::I(n));
    }

    /// Records an internal cycle taken when `c` is 1 and ticks it.
    pub fn internal_if(&mut self, b: &mut Builder, c: Value) {
        b.tick_if(c, 1);
        self.bus.push(Bus::IDyn(1));
    }

    /// Records a branch to `t`, taken when `g` is 1 (always without `g`).
    pub fn branch(&mut self, b: &mut Builder, t: Target, k: Kind, g: Option<Value>) {
        let fall = b.pc().wrapping_add(self.len());
        // A second branch under its own condition (the two arms of a
        // dynamic `if`): the earlier one stays the target when `g` is 0.
        if let (Some(g), Some((old, _))) = (g, self.exit) {
            let old = match old {
                Next::Const(c) => b.konst(Width::W32, u128::from(c)),
                Next::Dynamic(v) => v,
                Next::Select {
                    cond,
                    taken,
                    not_taken,
                } => {
                    let t = b.konst(Width::W32, u128::from(taken));
                    let n = b.konst(Width::W32, u128::from(not_taken));
                    b.select(cond, t, n)
                }
            };
            let new = match t {
                Target::Const(c) => b.konst(Width::W32, u128::from(c)),
                Target::Dynamic(v) => v,
            };
            self.exit = Some((Next::Dynamic(b.select(g, new, old)), k));
            return;
        }
        let next = match (t, g) {
            (Target::Const(c), None) => Next::Const(c),
            (Target::Dynamic(v), None) => Next::Dynamic(v),
            (Target::Const(c), Some(g)) => Next::Select {
                cond: g,
                taken: c,
                not_taken: fall,
            },
            (Target::Dynamic(v), Some(g)) => {
                let f = b.konst(Width::W32, u128::from(fall));
                Next::Dynamic(b.select(g, v, f))
            }
        };
        self.exit = Some((next, k));
    }

    /// Whether the instruction writes the PC.
    pub fn branches(&self) -> bool {
        self.exit.is_some()
    }

    /// Emits the writes and the cycle cost, and gives the exit. The cost is
    /// every bus event at one cycle (no waitstates: the region map is the
    /// runtime's); the code side is the prefetch of the next instruction
    /// (1S) plus the refill after a PC write (1N + 1S).
    pub fn finish(mut self, b: &mut Builder) -> (Exit, Vec<Bus>) {
        let w = if self.thumb { 2 } else { 4 };
        let mut bus = vec![Bus::Code(true, w)];
        bus.append(&mut self.bus);
        if self.exit.is_some() {
            bus.push(Bus::Code(false, w));
            bus.push(Bus::Code(true, w));
        }
        let fixed: u32 = bus
            .iter()
            .map(|e| match e {
                Bus::I(n) => *n,
                Bus::IDyn(_) => 0,
                _ => 1,
            })
            .sum();
        b.tick(fixed);
        for (i, v) in self.cur.iter().enumerate() {
            if let Some(v) = v {
                b.set(Reg(i as u16), *v);
            }
        }
        let fall = b.pc().wrapping_add(self.len());
        let exit = match self.exit {
            None => Exit {
                next: Next::Const(fall),
                flow: Flow::Jump,
            },
            Some((next, k)) => Exit {
                next,
                flow: match k {
                    Kind::Jump => Flow::Jump,
                    Kind::Call => Flow::Call { ret: fall },
                    Kind::Return => Flow::Return,
                },
            },
        };
        (exit, bus)
    }
}

/// The two's complement value of the low `w` bits of `x`.
pub fn sint(x: u128, w: u32) -> i128 {
    if w == 0 || w >= 128 {
        return x as i128;
    }
    let s = 128 - w;
    ((x << s) as i128) >> s
}

/// The low `w` bits of `x` sign-extended to `n` bits.
pub fn sext(x: u128, w: u32, n: u32) -> u128 {
    (sint(x, w) as u128) & mask(n)
}

/// The all-ones value of `w` bits.
pub fn mask(w: u32) -> u128 {
    if w >= 128 {
        u128::MAX
    } else {
        (1u128 << w) - 1
    }
}

fn amount(n: i128) -> u32 {
    n.clamp(0, 255) as u32
}

/// `x >> n`, 0 once `n` reaches 128.
pub fn lshr(x: u128, n: i128) -> u128 {
    x.checked_shr(amount(n)).unwrap_or(0)
}

/// `x << n` within `w` bits.
pub fn shl(x: u128, n: i128, w: u32) -> u128 {
    x.checked_shl(amount(n)).unwrap_or(0) & mask(w)
}

/// Arithmetic `x >> n` within `w` bits.
pub fn ashr(x: u128, n: i128, w: u32) -> u128 {
    let s = sint(x, w);
    (s >> amount(n).min(127)) as u128 & mask(w)
}

/// Count of leading zeros of the low `w` bits of `x`.
pub fn clz(x: u128, w: u32) -> i128 {
    i128::from(w) - i128::from(128 - (x & mask(w)).leading_zeros())
}

/// Truncating division; 0 for division by zero.
pub fn tdiv(a: i128, b: i128) -> i128 {
    if b == 0 { 0 } else { a.wrapping_div(b) }
}

/// Remainder of truncating division.
pub fn tmod(a: i128, b: i128) -> i128 {
    if b == 0 { a } else { a.wrapping_rem(b) }
}

/// Floor division.
pub fn fdiv(a: i128, b: i128) -> i128 {
    if b == 0 { 0 } else { a.div_euclid(b) }
}

/// Euclidean remainder.
pub fn fmod(a: i128, b: i128) -> i128 {
    if b == 0 { a } else { a.rem_euclid(b) }
}

/// `2^n`, 0 outside 0 to 126.
pub fn pow2(n: i128) -> i128 {
    if (0..127).contains(&n) { 1i128 << n } else { 0 }
}

/// `a << n` on integers.
pub fn shl_int(a: i128, n: i128) -> i128 {
    a.checked_shl(amount(n)).unwrap_or(0)
}

/// `a >> n` on integers.
pub fn shr_int(a: i128, n: i128) -> i128 {
    a >> amount(n).min(127)
}

/// `x << n` on Sail integers held in 128 bits (see the A64 `rt`).
pub fn shl_int_ir(b: &mut Builder, x: Value, n: Value) -> Value {
    let w = Width::W128;
    let exact = b.bin(BinOp::Shl, x, n);
    let back = b.bin(BinOp::AShr, exact, n);
    let k128 = b.konst(w, 128);
    let in_range = b.cmp(CmpOp::Ult, n, k128);
    let same = b.cmp(CmpOp::Eq, back, x);
    let small = b.and(in_range, same);
    let zero = b.konst(w, 0);
    let is_zero = b.cmp(CmpOp::Eq, x, zero);
    let neg = b.cmp(CmpOp::Slt, x, zero);
    let hi_pos = b.konst(w, (u128::MAX >> 1) & !u128::from(u64::MAX));
    let hi_neg = b.konst(w, 1u128 << 127);
    let sat = b.select(neg, hi_neg, hi_pos);
    let big = b.select(is_zero, zero, sat);
    b.select(small, exact, big)
}
