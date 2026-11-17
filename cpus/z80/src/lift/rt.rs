//! What the generated templates call: the guest state as the instruction
//! sees it while it runs, its bus accesses (memory, I/O ports) at their
//! T-states, its clock and its exit. The hand-written side of the cut line
//! (`cpus/z80/gen/src/cut.rs`); it holds no instruction semantics.

use relift_ir::{Builder, Endian, Flow, IoKind, Next, Reg, Value, Width};

use super::{HALF, HALT, PORT_IN, PORT_OUT};
use crate::regs;

/// Why a template did not lift: never.
pub type LiftError = core::convert::Infallible;

/// Where a branch goes.
#[derive(Clone, Copy, Debug)]
pub enum Target {
    /// A known address.
    Const(u16),
    /// A computed address.
    Dynamic(Value),
}

/// The kind of a branch.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A jump.
    Jump,
    /// A call returning to the next instruction.
    Call,
    /// A return.
    Return,
}

enum Active {
    Always,
    Never,
    If(Value),
}

/// The guest state of one instruction while its template runs.
pub struct St {
    bytes: [u8; 4],
    pc: u16,
    taken: bool,
    live: bool,
    cond: Option<Value>,
    elapsed: u32,
    guarded: Vec<(u32, Value)>,
    cur: Vec<Option<Value>>,
    exit: Option<(Target, Option<Value>, Kind)>,
    bad: Option<&'static str>,
    /// The instruction bytes fetched: offset from PC and T-state.
    pub fetches: Vec<(u16, u32)>,
    len: u16,
    /// Bit `k` set when the byte at offset `k` is an opcode or prefix byte,
    /// which a live template takes from the bytes lifted.
    opcodes: u8,
}

/// How an instruction left off.
pub struct Finished {
    /// Where execution goes.
    pub next: Next,
    /// The transfer kind.
    pub flow: Flow,
    /// The instruction length in bytes.
    pub len: u16,
    /// T-states charged so far.
    pub elapsed: u32,
}

impl St {
    /// The state for the instruction `bytes` at `pc`; `taken` lifts the
    /// taken continuation of a conditional `CALL` or `RET`, which starts
    /// with `elapsed` T-states charged.
    pub fn new(bytes: [u8; 4], pc: u16, taken: bool, elapsed: u32) -> St {
        St {
            bytes,
            pc,
            taken,
            live: !taken,
            cond: None,
            elapsed,
            guarded: Vec::new(),
            cur: Vec::new(),
            exit: None,
            bad: None,
            fetches: Vec::new(),
            len: 1,
            opcodes: 0b1,
        }
    }

    /// Marks the bytes whose offsets have their bit set in `mask` as opcode
    /// or prefix bytes.
    pub fn set_opcodes(&mut self, mask: u8) {
        self.opcodes = mask;
    }

    /// The address an interrupt entry returns to.
    pub fn ret(&mut self, b: &mut Builder) -> Value {
        self.get(b, regs::PC)
    }

    /// The instruction's address.
    pub fn pc(&self) -> u128 {
        u128::from(self.pc)
    }

    fn after(&self) -> u64 {
        u64::from(self.pc.wrapping_add(self.len))
    }

    /// The instruction byte at `addr`.
    pub fn fetch(&self, addr: u128) -> u128 {
        let off = (addr as u16).wrapping_sub(self.pc);
        u128::from(self.bytes.get(usize::from(off)).copied().unwrap_or(0))
    }

    /// The instruction byte at `addr` in a live template: an opcode or
    /// prefix byte as lifted, an operand byte read from memory in the
    /// machine cycle starting at T-state `t` when the instruction runs. The
    /// taken half of a conditional call reads its operands again, untimed,
    /// since the half before the decision already charged those cycles.
    pub fn operand(&mut self, b: &mut Builder, addr: u128, t: u32, g: Option<Value>) -> Value {
        let off = (addr as u16).wrapping_sub(self.pc);
        if off < 8 && self.opcodes & (1 << off) != 0 {
            return b.konst(Width::W8, self.fetch(addr));
        }
        let a = b.konst(Width::W16, addr & 0xFFFF);
        if self.taken && !self.live {
            return b.io_load(a, Width::W8, Endian::Little, IoKind::Any);
        }
        self.read(b, a, t, g)
    }

    /// Records the fetch of the instruction byte at `addr` in the machine
    /// cycle that starts at T-state `t`.
    pub fn fetched(&mut self, addr: u128, t: u32, g: Option<Value>) {
        if g.is_some() {
            self.bad = Some("an instruction fetch under a run-time condition");
        }
        let off = (addr as u16).wrapping_sub(self.pc);
        self.len = self.len.max(off + 1);
        if self.live {
            self.fetches.push((off, t));
        }
    }

    fn active(&mut self, g: Option<Value>) -> Active {
        if !self.live {
            return Active::Never;
        }
        let Some(g) = g else {
            return Active::Always;
        };
        match (self.taken, self.cond) {
            (_, None) => Active::If(g),
            (false, Some(c)) if c == g => Active::Never,
            (false, Some(_)) => Active::Always,
            (true, Some(c)) if c == g => Active::Always,
            (true, Some(_)) => Active::Never,
        }
    }

    /// The current value of `r`.
    pub fn get(&mut self, b: &mut Builder, r: Reg) -> Value {
        if let Some(Some(v)) = self.cur.get(r.index()) {
            return *v;
        }
        b.get(r)
    }

    /// Writes `v` to `r`, under the condition `g` when given.
    pub fn set(&mut self, b: &mut Builder, r: Reg, v: Value, g: Option<Value>) {
        let v = match self.active(g) {
            Active::Always => v,
            Active::Never => return,
            Active::If(g) => {
                let old = self.get(b, r);
                b.select(g, v, old)
            }
        };
        b.set(r, v);
        if self.cur.len() <= r.index() {
            self.cur.resize(r.index() + 1, None);
        }
        self.cur[r.index()] = Some(v);
    }

    fn charge_to(&mut self, b: &mut Builder, t: u32) {
        if t > self.elapsed {
            b.tick(t - self.elapsed);
            self.elapsed = t;
        }
    }

    fn bus(&mut self, b: &mut Builder, t: u32, g: Option<Value>) -> bool {
        match self.active(g) {
            Active::Always => {
                self.charge_to(b, t);
                true
            }
            Active::Never => false,
            Active::If(_) => {
                self.bad = Some("a bus access under a run-time condition");
                false
            }
        }
    }

    /// Reads the byte at `addr` in the machine cycle starting at `t`.
    pub fn read(&mut self, b: &mut Builder, addr: Value, t: u32, g: Option<Value>) -> Value {
        if !self.bus(b, t, g) {
            return b.konst(Width::W8, 0);
        }
        b.io_load(addr, Width::W8, Endian::Little, IoKind::Any)
    }

    /// Writes the byte `v` to `addr` in the machine cycle starting at `t`.
    pub fn write(&mut self, b: &mut Builder, addr: Value, v: Value, t: u32, g: Option<Value>) {
        if self.bus(b, t, g) {
            b.io_store(addr, v, Endian::Little, IoKind::Any);
        }
    }

    /// Reads I/O port `port` in the machine cycle starting at `t`.
    pub fn port_in(&mut self, b: &mut Builder, port: Value, t: u32, g: Option<Value>) -> Value {
        if !self.bus(b, t, g) {
            return b.konst(Width::W8, 0);
        }
        b.intrinsic(PORT_IN, &[port], Some(Width::W8))
    }

    /// Writes `v` to I/O port `port` in the machine cycle starting at `t`.
    pub fn port_out(&mut self, b: &mut Builder, port: Value, v: Value, t: u32, g: Option<Value>) {
        if self.bus(b, t, g) {
            b.intrinsic(PORT_OUT, &[port, v], None);
        }
    }

    /// The instruction takes `n` T-states in all.
    pub fn cycles(&mut self, b: &mut Builder, n: u32, g: Option<Value>) {
        match self.active(g) {
            Active::Always => self.charge_to(b, n),
            Active::Never => {}
            Active::If(g) => self.guarded.push((n, g)),
        }
    }

    /// The condition of a conditional `CALL` or `RET`.
    pub fn decide(&mut self, b: &mut Builder, c: Value) -> Value {
        if self.taken {
            self.live = true;
            let one = b.konst(Width::W1, 1);
            self.cond = Some(one);
            one
        } else {
            self.cond = Some(c);
            c
        }
    }

    /// Records a branch to `t`, taken when `g` is 1 (always without `g`).
    pub fn branch(&mut self, _b: &mut Builder, t: Target, k: Kind, g: Option<Value>) {
        match self.active(g) {
            Active::Never => {}
            Active::Always => self.exit = Some((t, None, k)),
            Active::If(g) => self.exit = Some((t, Some(g), k)),
        }
    }

    /// `HALT`: the per-CPU layer waits for an interrupt and gives the R the
    /// NOPs it ran leave.
    pub fn halt(&mut self, b: &mut Builder, g: Option<Value>) {
        if let Active::Always = self.active(g) {
            let iff1 = self.get(b, regs::IFF1);
            let r = self.get(b, regs::R);
            let r = b.intrinsic(HALT, &[iff1, r], Some(Width::W8));
            self.set(b, regs::R, r, None);
        }
    }

    /// Charges the run-time part of the clock and gives the exit.
    ///
    /// # Errors
    ///
    /// The template did something `St` cannot express.
    pub fn finish(mut self, b: &mut Builder) -> Result<Finished, &'static str> {
        if let Some(e) = self.bad {
            return Err(e);
        }
        if !self.guarded.is_empty() {
            let base = self.guarded.iter().map(|(n, _)| *n).min().unwrap_or(0);
            self.charge_to(b, base);
            for (n, g) in std::mem::take(&mut self.guarded) {
                if n > base {
                    b.tick_if(g, n - base);
                }
            }
        }
        let after = self.after();
        if !self.taken
            && let Some(cond) = self.cond
        {
            return Ok(Finished {
                next: Next::Select {
                    cond,
                    taken: HALF | u64::from(self.pc),
                    not_taken: after,
                },
                flow: Flow::Jump,
                len: self.len,
                elapsed: self.elapsed,
            });
        }
        let (next, kind) = match self.exit {
            None => (Next::Const(after), Kind::Jump),
            Some((Target::Const(c), None, k)) => (Next::Const(u64::from(c)), k),
            Some((Target::Dynamic(v), None, k)) => (Next::Dynamic(v), k),
            Some((Target::Const(c), Some(g), k)) => (
                Next::Select {
                    cond: g,
                    taken: u64::from(c),
                    not_taken: after,
                },
                k,
            ),
            Some((Target::Dynamic(v), Some(g), Kind::Jump)) => {
                let w = b.width(v);
                let n = b.konst(w, u128::from(after));
                (Next::Dynamic(b.select(g, v, n)), Kind::Jump)
            }
            Some((Target::Dynamic(_), Some(_), _)) => {
                return Err("a computed call or return under a run-time condition");
            }
        };
        let flow = match kind {
            Kind::Jump => Flow::Jump,
            Kind::Call => Flow::Call { ret: after },
            Kind::Return => Flow::Return,
        };
        Ok(Finished {
            next,
            flow,
            len: self.len,
            elapsed: self.elapsed,
        })
    }
}

/// The low `w` bits of `x` sign-extended to `n` bits.
pub fn sext(x: u128, w: u32, n: u32) -> u128 {
    let s = 128 - w;
    let v = (((x << s) as i128) >> s) as u128;
    if n >= 128 { v } else { v & ((1u128 << n) - 1) }
}
