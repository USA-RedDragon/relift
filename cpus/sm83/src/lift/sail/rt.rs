//! What the generated templates call: the guest state as the instruction
//! sees it while it runs, its bus accesses and clock, its exit, and the
//! integer helpers their static code uses. This is the hand-written side of
//! the cut line (`cpus/sm83/gen/src/cut.rs`).

use relift_ir::{Builder, Endian, Flow, IoKind, Next, Reg, Value, Width};

use super::super::{BUG, HALF};
use crate::irq;
use crate::regs::{FC, FZ};

/// Why a template did not lift: never, as the SM83 model throws no
/// exception. A model change that throws makes the generated code fail to
/// build.
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

/// A call into the per-CPU layer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Irq {
    /// `EI`: the frontend places the enable after the next instruction.
    Ei,
    /// `DI`.
    Di,
    /// `RETI`'s enable.
    Reti,
    /// `HALT`.
    Halt,
    /// `STOP`.
    Stop,
}

/// Which part of a conditional `CALL` or `RET` is lifted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// Any other instruction: conditions are run-time values.
    Plain,
    /// Up to the decision: the not-taken path, and an exit that goes to the
    /// taken continuation when the condition holds.
    Decide,
    /// The taken continuation: the condition holds.
    Taken,
}

/// How a guard applies in the current mode.
enum Active {
    Always,
    Never,
    If(Value),
}

/// The guest state of one instruction while its template runs.
#[derive(Debug)]
pub struct St {
    bytes: [u8; 3],
    pc: u16,
    len: u16,
    view: u64,
    mode: Mode,
    cond: Option<Value>,
    elapsed: u32,
    guarded: Vec<(u32, Value)>,
    cur: Vec<Option<Value>>,
    exit: Option<(Next, Kind)>,
    control: bool,
    bad: Option<&'static str>,
}

/// How an instruction left off, for the frontend.
pub struct Finished {
    /// Where execution goes.
    pub next: Next,
    /// What kind of transfer it is.
    pub flow: Flow,
    /// Whether it ends the block's straight-line run (a transfer, `HALT`,
    /// `STOP`).
    pub control: bool,
}

impl St {
    /// The state for the instruction `bytes` at `pc` of `len` bytes, lifted
    /// in `mode`, with `elapsed` T-cycles already charged (the not-taken
    /// cost in a taken continuation). `view` is added to the addresses of
    /// continuations.
    pub fn new(bytes: [u8; 3], pc: u16, len: u16, mode: Mode, elapsed: u32, view: u64) -> St {
        St {
            bytes,
            pc,
            len,
            view,
            mode,
            cond: None,
            elapsed,
            guarded: Vec::new(),
            cur: Vec::new(),
            exit: None,
            control: false,
            bad: None,
        }
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

    fn active(&mut self, g: Option<Value>) -> Active {
        let Some(g) = g else {
            return Active::Always;
        };
        match (self.mode, self.cond) {
            (Mode::Plain, _) => Active::If(g),
            (Mode::Decide, Some(c)) if c == g => Active::Never,
            (Mode::Taken, Some(c)) if c == g => Active::Always,
            (Mode::Decide, Some(_)) => Active::Always,
            (Mode::Taken, Some(_)) => Active::Never,
            (_, None) => {
                self.bad = Some("a guard before the condition");
                Active::Never
            }
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

    /// Reads the byte at `addr` in M-cycle `cycle`.
    pub fn read(&mut self, b: &mut Builder, addr: Value, cycle: u32, g: Option<Value>) -> Value {
        match self.active(g) {
            Active::Always => {}
            Active::Never => return b.konst(Width::W8, 0),
            Active::If(_) => {
                self.bad = Some("a read under a run-time condition");
                return b.konst(Width::W8, 0);
            }
        }
        self.charge_to(b, 4 * (cycle - 1));
        b.io_load(addr, Width::W8, Endian::Little, IoKind::Any)
    }

    /// Writes the byte `v` to `addr` in M-cycle `cycle`.
    pub fn write(&mut self, b: &mut Builder, addr: Value, v: Value, cycle: u32, g: Option<Value>) {
        match self.active(g) {
            Active::Always => {}
            Active::Never => return,
            Active::If(_) => {
                self.bad = Some("a write under a run-time condition");
                return;
            }
        }
        self.charge_to(b, 4 * (cycle - 1));
        b.io_store(addr, v, Endian::Little, IoKind::Any);
    }

    /// The instruction takes `n` M-cycles in all.
    pub fn cycles(&mut self, b: &mut Builder, n: u32, g: Option<Value>) {
        match self.active(g) {
            Active::Always => self.charge_to(b, 4 * n),
            Active::Never => {}
            Active::If(g) => self.guarded.push((4 * n, g)),
        }
    }

    /// The condition `cc` (NZ, Z, NC, C) of a conditional instruction.
    pub fn cond(&mut self, b: &mut Builder, cc: u32) -> Value {
        let v = if self.mode == Mode::Taken {
            b.konst(Width::W1, 1)
        } else {
            let (flag, want) = match cc {
                0 => (FZ, false),
                1 => (FZ, true),
                2 => (FC, false),
                _ => (FC, true),
            };
            let f = self.get(b, flag);
            if want { f } else { b.not(f) }
        };
        self.cond = Some(v);
        v
    }

    /// Records a branch to `t`, taken when `g` is 1 (always without `g`).
    pub fn branch(&mut self, b: &mut Builder, t: Target, k: Kind, g: Option<Value>) {
        let _ = b;
        let next = match (self.active(g), t) {
            (Active::Never, _) => return,
            (Active::Always, Target::Const(c)) => Next::Const(u64::from(c)),
            (Active::Always, Target::Dynamic(v)) => Next::Dynamic(v),
            (Active::If(g), Target::Const(c)) => Next::Select {
                cond: g,
                taken: u64::from(c),
                not_taken: self.after(),
            },
            (Active::If(_), Target::Dynamic(_)) => {
                self.bad = Some("a computed branch under a run-time condition");
                return;
            }
        };
        self.control = true;
        self.exit = Some((next, k));
    }

    /// A call into the per-CPU layer.
    pub fn irq(&mut self, b: &mut Builder, i: Irq) {
        match i {
            Irq::Ei => {}
            Irq::Di => {
                b.intrinsic(irq::DI, &[], None);
            }
            Irq::Reti => {
                b.intrinsic(irq::RETI, &[], None);
            }
            Irq::Halt => {
                let skipped = b.intrinsic(irq::HALT, &[], Some(Width::W1));
                self.control = true;
                self.exit = Some((
                    Next::Select {
                        cond: skipped,
                        taken: BUG | u64::from(self.pc),
                        not_taken: self.after(),
                    },
                    Kind::Jump,
                ));
            }
            Irq::Stop => {
                let one_byte = b.intrinsic(irq::STOP, &[], Some(Width::W1));
                let two = b.not(one_byte);
                b.tick_if(two, 4);
                let after = self.after();
                self.control = true;
                self.exit = Some((
                    Next::Select {
                        cond: one_byte,
                        taken: after.wrapping_sub(1) & 0xFFFF,
                        not_taken: after,
                    },
                    Kind::Jump,
                ));
            }
        }
    }

    /// Charges the run-time part of the clock and gives the instruction's
    /// exit.
    ///
    /// # Errors
    ///
    /// The template did something `St` cannot express, named in the error.
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
        if self.mode == Mode::Decide {
            let cond = self.cond.ok_or("no condition")?;
            return Ok(Finished {
                next: Next::Select {
                    cond,
                    taken: HALF | self.view | u64::from(self.pc),
                    not_taken: after,
                },
                flow: Flow::Jump,
                control: false,
            });
        }
        let (next, kind) = self.exit.unwrap_or((Next::Const(after), Kind::Jump));
        let flow = match kind {
            Kind::Jump => Flow::Jump,
            Kind::Call => Flow::Call { ret: after },
            Kind::Return => Flow::Return,
        };
        Ok(Finished {
            next,
            flow,
            control: self.control,
        })
    }
}

/// The low `w` bits of `x` sign-extended to `n` bits.
pub fn sext(x: u128, w: u32, n: u32) -> u128 {
    let s = 128 - w;
    let v = (((x << s) as i128) >> s) as u128;
    if n >= 128 { v } else { v & ((1u128 << n) - 1) }
}
