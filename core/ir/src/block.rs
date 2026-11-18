use std::fmt;

use crate::{BinOp, Op, RegFile, UnOp, Value, Width};

/// One instruction: an operation and the width of its result, if it has one.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Inst {
    /// The operation.
    pub op: Op,
    /// The result width; `None` for operations that give no value.
    pub width: Option<Width>,
}

const _: () = assert!(std::mem::size_of::<Inst>() == 32);

/// Where control goes when a block exits.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Next {
    /// A known address.
    Const(u64),
    /// `taken` when `cond` (one bit) is 1, else `not_taken`.
    Select {
        /// The condition.
        cond: Value,
        /// The address when the condition holds.
        taken: u64,
        /// The address when it does not.
        not_taken: u64,
    },
    /// An address computed in the block, of the block's address width.
    Dynamic(Value),
}

/// What kind of control transfer the exit is, for analysis. The interpreter
/// treats every kind as a jump to [`Exit::next`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Flow {
    /// Straight-line or a branch within or between functions.
    Jump,
    /// A call that returns to `ret`.
    Call {
        /// The return address.
        ret: u64,
    },
    /// A return to a caller.
    Return,
    /// A trap to the runtime (a system call, a breakpoint) with the
    /// frontend's code; `next` is where execution resumes.
    Trap(u32),
}

/// How a block ends.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Exit {
    /// The next program counter.
    pub next: Next,
    /// The kind of transfer.
    pub flow: Flow,
}

/// The lifted form of guest code at one address: straight-line SSA and an exit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Block {
    /// The register file the block's `Get` and `Set` index.
    pub regs: RegFile,
    /// The width of guest addresses.
    pub addr_width: Width,
    /// The guest address of the first lifted instruction.
    pub pc: u64,
    /// The number of guest bytes lifted.
    pub len: u32,
    /// The instructions, in order; value `n` is the result of `insts[n]`.
    pub insts: Vec<Inst>,
    /// The exit.
    pub exit: Exit,
    /// Which mapping of the guest bytes at `pc` was lifted, for guests
    /// whose code at one address changes: a cartridge ROM bank behind a
    /// bank controller, an overlay, or code copied to RAM (where the guest
    /// may use a hash of the bytes). The value is the guest's own; `None`
    /// means the address holds one piece of code for the whole run, and it
    /// is always `None` for guests without banking. `(pc, bank)` is the
    /// block's identity ([`Block::key`]): blocks with one `pc` and
    /// different banks are different code. Exit targets are plain guest
    /// addresses; which bank one reaches is decided by the mapping at run
    /// time, or by an analysis that tracks the bank-switch writes.
    pub bank: Option<u64>,
}

/// A type error in a block, found by [`Block::verify`] or the builder.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VerifyError {
    /// The index of the offending instruction, or `None` for the exit.
    pub at: Option<usize>,
    /// What is wrong.
    pub message: String,
}

impl fmt::Display for VerifyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.at {
            Some(i) => write!(f, "instruction {i}: {}", self.message),
            None => write!(f, "exit: {}", self.message),
        }
    }
}

impl std::error::Error for VerifyError {}

fn err<T>(message: String) -> Result<T, String> {
    Err(message)
}

/// Checks `op` with the given result width against the widths of earlier
/// values; `widths[n]` is the result width of value `n`.
pub(crate) fn check_op(
    op: &Op,
    width: Option<Width>,
    widths: &[Option<Width>],
    regs: &RegFile,
    addr_width: Width,
) -> Result<(), String> {
    let w = |v: Value| -> Result<Width, String> {
        match widths.get(v.index()) {
            Some(Some(w)) => Ok(*w),
            Some(None) => err(format!("value {} has no result", v.0)),
            None => err(format!("value {} is not defined before use", v.0)),
        }
    };
    let need = || width.ok_or_else(|| "missing result width".to_string());
    let none = || {
        if width.is_some() {
            err("operation gives no value but has a width".to_string())
        } else {
            Ok(())
        }
    };
    let same = |a: Width, b: Width| {
        if a == b {
            Ok(())
        } else {
            err(format!("width mismatch: {} and {}", a.bits(), b.bits()))
        }
    };
    let bytes = |x: Width| {
        x.bytes()
            .map(|_| ())
            .ok_or_else(|| format!("memory width {} is not whole bytes", x.bits()))
    };
    match op {
        Op::Const(c) => {
            let c = c.get();
            let r = need()?;
            if c & !r.mask() != 0 {
                return err(format!("constant {c:#x} does not fit {} bits", r.bits()));
            }
            Ok(())
        }
        Op::Get(reg) => {
            if reg.index() >= regs.len() {
                return err(format!("slot {} is not in the register file", reg.0));
            }
            same(need()?, regs.width(*reg))
        }
        Op::Set(reg, v) => {
            none()?;
            if reg.index() >= regs.len() {
                return err(format!("slot {} is not in the register file", reg.0));
            }
            same(w(*v)?, regs.width(*reg))
        }
        Op::Un(u, v) => {
            let a = w(*v)?;
            same(need()?, a)?;
            if *u == UnOp::ByteSwap {
                bytes(a)?;
            }
            Ok(())
        }
        Op::Bin(b, x, y) => {
            let a = w(*x)?;
            let c = w(*y)?;
            same(need()?, a)?;
            match b {
                BinOp::Shl | BinOp::LShr | BinOp::AShr | BinOp::RotR => Ok(()),
                _ => same(a, c),
            }
        }
        Op::Cmp(_, x, y) => {
            same(need()?, Width::W1)?;
            same(w(*x)?, w(*y)?)
        }
        Op::Select(c, x, y) => {
            same(w(*c)?, Width::W1)?;
            let a = w(*x)?;
            same(a, w(*y)?)?;
            same(need()?, a)
        }
        Op::Ext(_, v) => {
            if need()? < w(*v)? {
                return err("extension narrows".to_string());
            }
            Ok(())
        }
        Op::Extract(v, lo) => {
            if u32::from(*lo) + need()?.bits() > w(*v)?.bits() {
                return err("extract runs past the operand".to_string());
            }
            Ok(())
        }
        Op::Concat(h, l) => {
            let sum = w(*h)?.bits() + w(*l)?.bits();
            same(need()?, Width::try_new(sum).ok_or("concat wider than 128")?)
        }
        Op::Load { addr, .. } | Op::LoadExclusive { addr, .. } | Op::IoLoad { addr, .. } => {
            same(w(*addr)?, addr_width)?;
            bytes(need()?)
        }
        Op::Store { addr, value, .. } | Op::IoStore { addr, value, .. } => {
            none()?;
            same(w(*addr)?, addr_width)?;
            bytes(w(*value)?)
        }
        Op::StoreExclusive { addr, value, .. } => {
            same(need()?, Width::W1)?;
            same(w(*addr)?, addr_width)?;
            bytes(w(*value)?)
        }
        Op::ClearExclusive | Op::Fence(_) => none(),
        Op::Guard(c, _) => {
            none()?;
            same(w(*c)?, Width::W1)
        }
        Op::Fp(f, args) => {
            let r = need()?;
            let fp = |w: Width| w == Width::W16 || w == Width::W32 || w == Width::W64;
            if !fp(r) {
                return err("floating point is 16, 32 or 64 bits".to_string());
            }
            if args.len() != f.arity() {
                return err(format!("{f:?} takes {} operands", f.arity()));
            }
            for a in args {
                let aw = w(*a)?;
                if f.converts() {
                    if !fp(aw) {
                        return err("floating point is 16, 32 or 64 bits".to_string());
                    }
                } else {
                    same(aw, r)?;
                }
            }
            Ok(())
        }
        Op::Intrinsic(call) => {
            for a in &call.args {
                w(*a)?;
            }
            Ok(())
        }
        Op::Tick(_) | Op::Sync => none(),
        Op::TickIf(c, _) => {
            none()?;
            same(w(*c)?, Width::W1)
        }
        Op::AtomicRmw { addr, value, .. } => {
            same(w(*addr)?, addr_width)?;
            let r = need()?;
            same(w(*value)?, r)?;
            bytes(r)
        }
        Op::CompareSwap {
            addr,
            expected,
            value,
            ..
        } => {
            same(w(*addr)?, addr_width)?;
            let r = need()?;
            same(w(*value)?, r)?;
            same(w(*expected)?, r)?;
            bytes(r)
        }
        Op::Lane(l, args) => {
            let ws = args.iter().map(|a| w(*a)).collect::<Result<Vec<_>, _>>()?;
            l.check(&ws, width)
        }
        Op::Permute(p) => {
            same(need()?, Width::W128)?;
            same(w(p.a)?, Width::W128)?;
            if let Some(b) = p.b {
                same(w(b)?, Width::W128)?;
            }
            Ok(())
        }
        Op::Crypto(_, x) => {
            same(need()?, Width::W128)?;
            same(w(*x)?, Width::W128)
        }
        Op::Pac {
            data,
            modifier,
            key0,
            key1,
        } => {
            same(need()?, Width::W64)?;
            for v in [data, modifier, key0, key1] {
                same(w(*v)?, Width::W64)?;
            }
            Ok(())
        }
    }
}

impl Block {
    /// Checks every width and operand reference, and the exit.
    pub fn verify(&self) -> Result<(), VerifyError> {
        self.check(true)
    }

    /// As [`Block::verify`]; with `ops` false, for a block whose operations
    /// were each checked as they were added, only what spans operations:
    /// the order of guards and syncs, and the exit.
    pub(crate) fn check(&self, ops: bool) -> Result<(), VerifyError> {
        let mut widths = Vec::with_capacity(self.insts.len());
        let mut effect = false;
        for (i, inst) in self.insts.iter().enumerate() {
            if let Op::Sync = inst.op
                && i + 1 != self.insts.len()
            {
                return Err(VerifyError {
                    at: Some(i),
                    message: "a sync is not the last instruction".to_string(),
                });
            }
            if let Op::Guard(..) = inst.op {
                if effect {
                    return Err(VerifyError {
                        at: Some(i),
                        message: "a guard follows an operation with an effect".to_string(),
                    });
                }
            } else if inst.op.has_effect() && !matches!(inst.op, Op::Load { .. }) {
                effect = true;
            }
            if ops {
                check_op(&inst.op, inst.width, &widths, &self.regs, self.addr_width).map_err(
                    |message| VerifyError {
                        at: Some(i),
                        message,
                    },
                )?;
            }
            widths.push(inst.width);
        }
        let exit_err = |message: &str| VerifyError {
            at: None,
            message: message.to_string(),
        };
        let width_of = |v: Value| widths.get(v.index()).copied().flatten();
        match self.exit.next {
            Next::Const(_) => {}
            Next::Select { cond, .. } => {
                if width_of(cond) != Some(Width::W1) {
                    return Err(exit_err("the select condition is not one defined bit"));
                }
            }
            Next::Dynamic(v) => {
                if width_of(v) != Some(self.addr_width) {
                    return Err(exit_err("the target is not an address-width value"));
                }
            }
        }
        Ok(())
    }

    /// The block's identity: its address and bank.
    pub fn key(&self) -> (u64, Option<u64>) {
        (self.pc, self.bank)
    }

    /// The width of value `v`, if it is defined and has one.
    pub fn width_of(&self, v: Value) -> Option<Width> {
        self.insts.get(v.index()).and_then(|i| i.width)
    }
}
