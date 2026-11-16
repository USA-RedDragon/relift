//! The run-time side of the generated templates: `St` holds the lift-time
//! state of one block (pending register writes, the delay-slot and branch
//! state, the static exception) and turns the cut calls of the templates
//! into IR.
//!
//! - Register writes are pending until [`St::finish`]; a write under a
//!   guard becomes a select of the new and the old value.
//! - A run-time exception is a guard with code [`EXC_GUARD`] plus the
//!   Cause.ExcCode, plus [`IN_DELAY`] when the delay slot raised it; one
//!   whose condition is known at lift time is also kept as
//!   [`St::exception_static`].
//! - Addresses in kseg0 and kseg1 translate to `vaddr & 0x1FFF_FFFF`; any
//!   other address leaves through a TLBL or TLBS guard (the TLB is the
//!   runtime's).
//! - Loads and stores are I/O accesses of kind `Any`: the guest's I/O map
//!   decides at run time which physical addresses are plain memory. An
//!   access the instruction makes only under a condition (the delay slot of
//!   a likely branch, SC) reads and writes back plain memory unconditionally,
//!   selecting the old value; with [`Options::plain_below`] set, such an
//!   access to an address at or above it leaves first through the TLBL or
//!   TLBS guard, whose slow path makes it exactly.
//! - A condition that an earlier guard of the block already proved (the
//!   address error check before a load) is not a condition.
//! - COP1 arithmetic is one IR `FpOp` on the fast path, normal inputs to
//!   normal results with round-to-nearest and no enabled exceptions.
//!   Everything else leaves through the [`FP_SLOW`] guard (plus
//!   [`IN_DELAY`] in a delay slot) to the runtime's
//!   soft-float path, which knows every VR4300 rule. The FCR31 cause and
//!   flag bits of the fast path come from the intrinsic `mips_fcr31(op, fmt,
//!   fcr31, a, b)` and a compare's condition bit from `mips_fcmp(cond, fmt,
//!   fcr31, a, b)`; both give the new FCR31.
//! - The CP0 Random register is the intrinsic `mips_random()`, and the TLB
//!   instructions and CACHE are `mips_tlb(op, run)` and `mips_cache(op,
//!   vaddr, run)`, where `run` is 1 when the instruction is not annulled.

use relift_ir::{
    BinOp, Builder, CmpOp, Endian, FpOp, FpRound, IoKind, Op, Reg, UnOp, Value, Width,
};

use crate::vr4300::regs;

/// Where a branch goes when it is taken.
#[derive(Clone, Copy, Debug)]
pub enum Target {
    /// A known address.
    Const(u64),
    /// An address computed in the block.
    Dynamic(Value),
}

/// The kind of a memory access, for the TLB exception it raises.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    /// A load.
    LoadData,
    /// A store.
    StoreData,
}

/// The model's exceptions (the overlay's `Exception` enum).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exc {
    /// Interrupt.
    Interrupt,
    /// TLB modification.
    TLBMod,
    /// TLB miss or invalid on a load or fetch.
    TLBL,
    /// TLB miss or invalid on a store.
    TLBS,
    /// Address error on a load or fetch.
    AdEL,
    /// Address error on a store.
    AdES,
    /// System call.
    Sys,
    /// Breakpoint.
    Bp,
    /// Reserved instruction.
    ResI,
    /// Coprocessor unusable.
    CpU,
    /// Integer overflow.
    Ov,
    /// Trap.
    Tr,
    /// The model's CP2 exception, not raised on the VR4300.
    C2E,
    /// The model's CP2 trap, not raised on the VR4300.
    C2Trap,
    /// 64-bit TLB refill on a load.
    XTLBRefillL,
    /// 64-bit TLB refill on a store.
    XTLBRefillS,
    /// 64-bit TLB invalid on a load.
    XTLBInvL,
    /// 64-bit TLB invalid on a store.
    XTLBInvS,
    /// Machine check.
    MCheck,
    /// Floating-point exception.
    FPE,
}

impl Exc {
    /// The Cause.ExcCode number.
    pub fn code(self) -> u32 {
        match self {
            Exc::Interrupt => 0,
            Exc::TLBMod => 1,
            Exc::TLBL | Exc::XTLBRefillL | Exc::XTLBInvL => 2,
            Exc::TLBS | Exc::XTLBRefillS | Exc::XTLBInvS => 3,
            Exc::AdEL => 4,
            Exc::AdES => 5,
            Exc::Sys => 8,
            Exc::Bp => 9,
            Exc::ResI => 10,
            Exc::CpU => 11,
            Exc::Ov => 12,
            Exc::Tr => 13,
            Exc::FPE => 15,
            Exc::C2E | Exc::C2Trap => 18,
            Exc::MCheck => 24,
        }
    }
}

/// The guard code of an exception is this plus its ExcCode.
pub const EXC_GUARD: u32 = 0x1000;
/// Added to an exception's or the FPU slow path's guard code when the
/// delay slot left.
pub const IN_DELAY: u32 = 0x100;
/// The guard code of an FPU operation the fast path does not take.
pub const FP_SLOW: u32 = 0x2000;

/// `x` of width `from` sign-extended to `to` bits.
pub fn sext(x: u128, from: u32, to: u32) -> u128 {
    let s = 128 - from;
    let v = (((x << s) as i128) >> s) as u128;
    if to >= 128 {
        v
    } else {
        v & ((1u128 << to) - 1)
    }
}

/// `x` of width `w` as a signed integer.
pub fn sint(x: u128, w: u32) -> i128 {
    let s = 128 - w;
    ((x << s) as i128) >> s
}

/// A CP0 effect the runtime carries out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sys {
    /// TLBR (0), TLBWI (1), TLBWR (2) or TLBP (3).
    Tlb(u32),
    /// CACHE with its operation field.
    Cache(u32),
    /// A write to Status, Cause or Compare: the block ends and the runtime
    /// checks for interrupts.
    Cp0Sync,
}

/// How the lifter treats memory.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Options {
    /// Physical addresses below this are plain memory, where a read has no
    /// effect; a conditional access to any other address leaves through
    /// its guard. `None`: every address is plain memory.
    pub plain_below: Option<u64>,
}

/// The lift-time state of one block.
pub struct St {
    opts: Options,
    proved: Vec<Value>,
    pc: u64,
    in_delay: bool,
    pending: Vec<(Reg, Value)>,
    slot_guard: Option<Value>,
    branch: Option<(Target, Option<Value>)>,
    annul: Option<Option<Value>>,
    has_slot: bool,
    jump: Option<Value>,
    exception: Option<Exc>,
    sys: Vec<Sys>,
}

impl St {
    /// The state for the instruction at `pc`.
    pub fn new(pc: u64) -> St {
        St::with_options(pc, Options::default())
    }

    /// The state for the instruction at `pc`, lifted with `opts`.
    pub fn with_options(pc: u64, opts: Options) -> St {
        St {
            opts,
            proved: Vec::new(),
            pc,
            in_delay: false,
            pending: Vec::new(),
            slot_guard: None,
            branch: None,
            annul: None,
            has_slot: false,
            jump: None,
            exception: None,
            sys: Vec::new(),
        }
    }

    /// Moves on to the delay slot at `pc + 4` of the branch lifted so far:
    /// its effects are guarded by "not annulled".
    pub fn enter_delay_slot(&mut self, b: &mut Builder) {
        self.in_delay = true;
        self.pc += 4;
        self.slot_guard = match self.annul {
            Some(Some(g)) => Some(b.not(g)),
            _ => None,
        };
    }

    /// Whether the instruction lifted so far has a delay slot.
    pub fn has_delay_slot(&self) -> bool {
        self.has_slot
    }

    /// The branch target and the condition under which it is taken (`None`:
    /// always).
    pub fn branch_target(&self) -> Option<(Target, Option<Value>)> {
        self.branch
    }

    /// Whether a likely branch always annuls its slot.
    pub fn annuls_always(&self) -> bool {
        matches!(self.annul, Some(None))
    }

    /// The target of a jump without a delay slot (ERET).
    pub fn jump(&self) -> Option<Value> {
        self.jump
    }

    /// The exception raised whenever the block runs, if any.
    pub fn exception_static(&self) -> Option<Exc> {
        self.exception
    }

    /// The CP0 effects, in order.
    pub fn sys_effects(&self) -> &[Sys] {
        &self.sys
    }

    /// Whether `v` is 1 whenever the block gets past its earlier guards.
    fn holds(&self, b: &Builder, v: Value) -> bool {
        match b.op(v) {
            Some(Op::Un(UnOp::Not, x)) => self.proved.contains(x),
            Some(Op::Bin(BinOp::And, x, y)) => {
                let (x, y) = (*x, *y);
                self.holds(b, x) && self.holds(b, y)
            }
            _ => false,
        }
    }

    /// `g` without the parts the earlier guards proved.
    fn simplify(&self, b: &Builder, g: Option<Value>) -> Option<Value> {
        let v = g?;
        if self.holds(b, v) {
            return None;
        }
        match b.op(v) {
            Some(Op::Bin(BinOp::And, x, y)) if self.holds(b, *x) => Some(*y),
            Some(Op::Bin(BinOp::And, x, y)) if self.holds(b, *y) => Some(*x),
            _ => Some(v),
        }
    }

    /// The template guard `g` and the slot guard together.
    fn guard(&self, b: &mut Builder, g: Option<Value>) -> Option<Value> {
        let g = self.simplify(b, g);
        match (g, self.slot_guard) {
            (Some(x), Some(y)) => Some(b.and(x, y)),
            (x, None) => x,
            (None, y) => y,
        }
    }

    /// A guard exit with `code` when `bad` holds under the guard `g`.
    fn exit_if(&mut self, b: &mut Builder, bad: Value, g: Option<Value>, code: u32) {
        let c = match self.guard(b, g) {
            Some(g) => b.and(g, bad),
            None => bad,
        };
        b.guard(c, code);
        if self.simplify(b, g).is_none() {
            self.proved.push(bad);
        }
    }

    fn exc_code(&self, e: Exc) -> u32 {
        EXC_GUARD + e.code() + self.delay_flag()
    }

    fn fp_slow(&self) -> u32 {
        FP_SLOW + self.delay_flag()
    }

    fn delay_flag(&self) -> u32 {
        if self.in_delay { IN_DELAY } else { 0 }
    }

    fn get(&mut self, b: &mut Builder, r: Reg) -> Value {
        match self.pending.iter().find(|(p, _)| *p == r) {
            Some((_, v)) => *v,
            None => b.get(r),
        }
    }

    fn set(&mut self, b: &mut Builder, r: Reg, v: Value, g: Option<Value>) {
        let v = match self.guard(b, g) {
            Some(g) => {
                let old = self.get(b, r);
                b.select(g, v, old)
            }
            None => v,
        };
        match self.pending.iter_mut().find(|(p, _)| *p == r) {
            Some(p) => p.1 = v,
            None => self.pending.push((r, v)),
        }
    }

    fn named(name: &str) -> Reg {
        regs::slot(name).unwrap_or_else(|| panic!("the register file has no {name}"))
    }

    /// The instruction's address.
    pub fn pc(&self) -> u128 {
        u128::from(self.pc)
    }

    /// Whether the instruction is a delay slot.
    pub fn in_delay(&self) -> bool {
        self.in_delay
    }

    /// General register `n`.
    pub fn gpr(&mut self, b: &mut Builder, n: u8) -> Value {
        if n == 0 {
            return b.konst(Width::W64, 0);
        }
        self.get(b, regs::gpr(n))
    }

    /// Writes general register `n` (a write to `r0` is dropped).
    pub fn set_gpr(&mut self, b: &mut Builder, n: u8, v: Value, g: Option<Value>) {
        if n != 0 {
            self.set(b, regs::gpr(n), v, g);
        }
    }

    /// FPU register `n`, all 64 bits.
    pub fn fpr(&mut self, b: &mut Builder, n: u8) -> Value {
        self.get(b, regs::fpr(n))
    }

    /// Writes FPU register `n`.
    pub fn set_fpr(&mut self, b: &mut Builder, n: u8, v: Value, g: Option<Value>) {
        self.set(b, regs::fpr(n), v, g);
    }

    /// The model register `name`.
    pub fn reg(&mut self, b: &mut Builder, name: &str) -> Value {
        self.get(b, St::named(name))
    }

    /// Writes the model register `name`.
    pub fn set_reg(&mut self, b: &mut Builder, name: &str, v: Value, g: Option<Value>) {
        self.set(b, St::named(name), v, g);
    }

    /// The CP0 Random register.
    pub fn random(&mut self, b: &mut Builder) -> Value {
        b.intrinsic("mips_random", &[], Some(Width::W64))
    }

    /// Raises `e` under `g`. `_addr` is the BadVAddr of an address or TLB
    /// exception, which the guard cannot carry yet: the runtime recomputes
    /// it.
    pub fn exception(&mut self, b: &mut Builder, e: Exc, _addr: Option<Value>, g: Option<Value>) {
        let code = self.exc_code(e);
        match self.guard(b, g) {
            None => {
                if self.exception.is_none() {
                    self.exception = Some(e);
                }
                let t = b.konst(Width::W1, 1);
                b.guard(t, code);
            }
            Some(c) => {
                b.guard(c, code);
                if let Some(g) = g
                    && self.simplify(b, Some(g)) == Some(g)
                {
                    self.proved.push(g);
                }
            }
        }
    }

    /// A branch to `t` under `g`, after the delay slot.
    pub fn branch(&mut self, b: &mut Builder, t: Target, g: Option<Value>) {
        let g = self.guard(b, g);
        self.branch = Some((t, g));
        self.has_slot = true;
    }

    /// A likely branch not taken under `g`: its slot is annulled.
    pub fn annul(&mut self, b: &mut Builder, _next: u64, g: Option<Value>) {
        let g = self.guard(b, g);
        self.annul = Some(g);
        self.has_slot = true;
    }

    /// A branch not taken: the slot runs, then the next instruction.
    pub fn delay_slot(&mut self, _b: &mut Builder, _g: Option<Value>) {
        self.has_slot = true;
    }

    /// A jump to `v` without a delay slot under `g` (ERET's two arms).
    pub fn jump_now(&mut self, b: &mut Builder, v: Value, g: Option<Value>) {
        let v = match (self.guard(b, g), self.jump) {
            (Some(c), Some(old)) => b.select(c, v, old),
            _ => v,
        };
        self.jump = Some(v);
    }

    /// Whether coprocessor `n` is usable: its Status.CU bit, and for CP0
    /// also kernel mode (KSU = 0, EXL or ERL).
    fn cop_usable(&mut self, b: &mut Builder, n: u32) -> Value {
        let st = self.get(b, regs::CP0_STATUS);
        let cu = b.extract(st, 28 + n, Width::W1);
        if n != 0 {
            return cu;
        }
        let ksu = b.extract(st, 3, Width::new(2));
        let z = b.konst(Width::new(2), 0);
        let k = b.cmp(CmpOp::Eq, ksu, z);
        let exl = b.extract(st, 1, Width::W1);
        let erl = b.extract(st, 2, Width::W1);
        let k = b.or(k, exl);
        let k = b.or(k, erl);
        b.or(k, cu)
    }

    /// Leaves with a coprocessor unusable exception when coprocessor `n` is
    /// not usable.
    pub fn check_cop(&mut self, b: &mut Builder, n: u32, g: Option<Value>) {
        let ok = self.cop_usable(b, n);
        let bad = b.not(ok);
        let code = self.exc_code(Exc::CpU);
        self.exit_if(b, bad, g, code);
    }

    /// The physical address of `va`: kseg0 and kseg1 only.
    pub fn translate(&mut self, b: &mut Builder, va: Value, a: Access, g: Option<Value>) -> Value {
        let hi = b.extract(va, 30, Width::new(34));
        let k = b.konst(Width::new(34), 0x3_FFFF_FFFE);
        let ok = b.cmp(CmpOp::Eq, hi, k);
        let bad = b.not(ok);
        let e = if a == Access::LoadData {
            Exc::TLBL
        } else {
            Exc::TLBS
        };
        let code = self.exc_code(e);
        self.exit_if(b, bad, g, code);
        let m = b.konst(Width::W64, 0x1FFF_FFFF);
        b.bin(BinOp::And, va, m)
    }

    /// Leaves through the guard `code` when the access at `pa` is
    /// conditional (`g`) and may not be plain memory.
    fn device_exit(&mut self, b: &mut Builder, pa: Value, g: Option<Value>, code: Exc) {
        let (Some(_), Some(limit)) = (g, self.opts.plain_below) else {
            return;
        };
        let k = b.konst(Width::W64, u128::from(limit));
        let dev = b.cmp(CmpOp::Ule, k, pa);
        let code = self.exc_code(code);
        b.guard(dev, code);
    }

    /// A big-endian load of `n` bytes at `pa` under `g`. A load of plain
    /// memory has no effect the block must order, so one under a guard
    /// runs unconditionally.
    pub fn load(
        &mut self,
        b: &mut Builder,
        pa: Value,
        n: u32,
        _ll: bool,
        g: Option<Value>,
    ) -> Value {
        let g = self.guard(b, g);
        self.device_exit(b, pa, g, Exc::TLBL);
        b.io_load(pa, Width::new(8 * n), Endian::Big, IoKind::Any)
    }

    /// A big-endian store of `n` bytes of `v` at `pa` under `g`.
    pub fn store(&mut self, b: &mut Builder, pa: Value, n: u32, v: Value, g: Option<Value>) {
        let g = self.guard(b, g);
        self.device_exit(b, pa, g, Exc::TLBS);
        let v = match g {
            Some(c) => {
                let old = b.io_load(pa, Width::new(8 * n), Endian::Big, IoKind::Any);
                b.select(c, v, old)
            }
            None => v,
        };
        b.io_store(pa, v, Endian::Big, IoKind::Any);
    }

    /// SC and SCD: the overlay calls this only when LLbit is set, and the
    /// store always succeeds.
    pub fn store_cond(
        &mut self,
        b: &mut Builder,
        pa: Value,
        n: u32,
        v: Value,
        g: Option<Value>,
    ) -> Value {
        self.store(b, pa, n, v, g);
        b.konst(Width::W1, 1)
    }

    /// The value 1 when the instruction is not annulled under `g`.
    fn runs(&mut self, b: &mut Builder, g: Option<Value>) -> Value {
        match self.guard(b, g) {
            Some(c) => c,
            None => b.konst(Width::W1, 1),
        }
    }

    /// TLBR (0), TLBWI (1), TLBWR (2) or TLBP (3).
    pub fn tlb(&mut self, b: &mut Builder, op: u32, g: Option<Value>) {
        let k = b.konst(Width::W32, u128::from(op));
        let r = self.runs(b, g);
        b.intrinsic("mips_tlb", &[k, r], None);
        self.sys.push(Sys::Tlb(op));
    }

    /// CACHE with operation `op` at `va`.
    pub fn cache(&mut self, b: &mut Builder, op: u32, va: Value, g: Option<Value>) {
        let k = b.konst(Width::W32, u128::from(op));
        let r = self.runs(b, g);
        b.intrinsic("mips_cache", &[k, va, r], None);
        self.sys.push(Sys::Cache(op));
    }

    /// A write to Status, Cause or Compare.
    pub fn cp0_sync(&mut self, _b: &mut Builder, _g: Option<Value>) {
        if !self.sys.contains(&Sys::Cp0Sync) {
            self.sys.push(Sys::Cp0Sync);
        }
    }

    /// The COP1 operation `op` (the funct field) on format `fmt` of `p` and
    /// `q` (only `p` for most), as an IR `FpOp` on the fast path. See the
    /// module documentation for the slow path.
    pub fn fp(
        &mut self,
        b: &mut Builder,
        op: u32,
        fmt: u32,
        p: Value,
        q: Value,
        g: Option<Value>,
    ) -> Value {
        let single = fmt == 0x10;
        let int_src = fmt == 0x14 || fmt == 0x15;
        let w = if single || fmt == 0x14 {
            Width::W32
        } else {
            Width::W64
        };
        let x = b.extract(p, 0, w);
        let y = b.extract(q, 0, w);
        let Some((fop, ow)) = fp_op(op, int_src, w) else {
            let t = b.konst(Width::W1, 1);
            let code = self.fp_slow();
            self.exit_if(b, t, g, code);
            return b.konst(Width::W64, 0);
        };
        let fcr = self.get(b, regs::FCR31);
        let mut slow = {
            let en = b.extract(fcr, 7, Width::new(5));
            let z = b.konst(Width::new(5), 0);
            b.cmp(CmpOp::Ne, en, z)
        };
        if matches!(op, 0..=4 | 32 | 33 | 36 | 37) {
            let rm = b.extract(fcr, 0, Width::new(2));
            let z = b.konst(Width::new(2), 0);
            let c = b.cmp(CmpOp::Ne, rm, z);
            slow = b.or(slow, c);
        }
        if fmt == 0x15 && matches!(op, 32 | 33) {
            let c = long_out_of_range(b, x);
            slow = b.or(slow, c);
        }
        if !int_src {
            let nargs = if matches!(op, 0..=3) { 2 } else { 1 };
            for v in [x, y].into_iter().take(nargs) {
                let c = not_normal(b, v, w);
                slow = b.or(slow, c);
            }
        }
        let args: Vec<Value> = if fop.arity() == 2 {
            vec![x, y]
        } else {
            vec![x]
        };
        let r = b.fp(fop, &args, ow);
        if matches!(op, 8..=15 | 36 | 37) && !int_src {
            let c = int_out_of_range(b, x, w, matches!(op, 8..=11 | 37));
            slow = b.or(slow, c);
        }
        if matches!(op, 2 | 3) || (op == 32 && !int_src) {
            let c = underflowed(b, x, w, r, ow);
            slow = b.or(slow, c);
        }
        if matches!(op, 0..=4 | 32 | 33) {
            let c = not_normal(b, r, ow);
            slow = b.or(slow, c);
        }
        let code = self.fp_slow();
        self.exit_if(b, slow, g, code);
        let opk = b.konst(Width::W32, u128::from(op));
        let fk = b.konst(Width::W32, u128::from(fmt));
        let x64 = b.zext(x, Width::W64);
        let y64 = b.zext(y, Width::W64);
        let new = b.intrinsic("mips_fcr31", &[opk, fk, fcr, x64, y64], Some(Width::W32));
        self.set(b, regs::FCR31, new, g);
        if ow == Width::W64 {
            r
        } else {
            b.zext(r, Width::W64)
        }
    }

    /// C.cond.fmt: sets the FCR31 condition bit on the fast path (normal or
    /// zero operands).
    pub fn fp_cmp(
        &mut self,
        b: &mut Builder,
        cond: u32,
        fmt: u32,
        x: Value,
        y: Value,
        g: Option<Value>,
    ) {
        let w = if fmt == 0x10 { Width::W32 } else { Width::W64 };
        let x = b.extract(x, 0, w);
        let y = b.extract(y, 0, w);
        let fcr = self.get(b, regs::FCR31);
        let c1 = not_normal(b, x, w);
        let c2 = not_normal(b, y, w);
        let slow = b.or(c1, c2);
        let code = self.fp_slow();
        self.exit_if(b, slow, g, code);
        let ck = b.konst(Width::W32, u128::from(cond));
        let fk = b.konst(Width::W32, u128::from(fmt));
        let x64 = b.zext(x, Width::W64);
        let y64 = b.zext(y, Width::W64);
        let new = b.intrinsic("mips_fcmp", &[ck, fk, fcr, x64, y64], Some(Width::W32));
        self.set(b, regs::FCR31, new, g);
    }

    /// Writes the pending registers, in slot order.
    pub fn finish(&mut self, b: &mut Builder) {
        let mut p = std::mem::take(&mut self.pending);
        p.sort_by_key(|(r, _)| r.index());
        for (r, v) in p {
            b.set(r, v);
        }
    }
}

/// The IR operation and result width of COP1 operation `op` on operands of
/// width `w` (integers when `int_src`), or `None` for one the overlay does
/// not send here.
fn fp_op(op: u32, int_src: bool, w: Width) -> Option<(FpOp, Width)> {
    let to_int = |r: FpRound, wide: bool| {
        Some((FpOp::ToSInt(r), if wide { Width::W64 } else { Width::W32 }))
    };
    let from = if int_src {
        FpOp::FromSInt
    } else {
        FpOp::Convert
    };
    match op {
        0 => Some((FpOp::Add, w)),
        1 => Some((FpOp::Sub, w)),
        2 => Some((FpOp::Mul, w)),
        3 => Some((FpOp::Div, w)),
        4 => Some((FpOp::Sqrt, w)),
        5 => Some((FpOp::Abs, w)),
        7 => Some((FpOp::Neg, w)),
        8 | 12 => to_int(FpRound::TiesEven, op == 8),
        9 | 13 => to_int(FpRound::Zero, op == 9),
        10 | 14 => to_int(FpRound::Ceil, op == 10),
        11 | 15 => to_int(FpRound::Floor, op == 11),
        32 => Some((from, Width::W32)),
        33 => Some((from, Width::W64)),
        36 => to_int(FpRound::TiesEven, false),
        37 => to_int(FpRound::TiesEven, true),
        _ => None,
    }
}

/// The exponent and mantissa widths of an IEEE value of width `w`, or
/// `None` for an integer result.
fn ieee(w: Width) -> Option<(u32, u32)> {
    match w {
        Width::W32 => Some((8, 23)),
        Width::W64 => Some((11, 52)),
        _ => None,
    }
}

/// 1 when `v`, an IEEE value of width `w`, is NaN, infinite or denormal
/// (always 0 for an integer width).
fn not_normal(b: &mut Builder, v: Value, w: Width) -> Value {
    let Some((eb, mb)) = ieee(w) else {
        return b.konst(Width::W1, 0);
    };
    let e = b.extract(v, mb, Width::new(eb));
    let ones = b.konst(Width::new(eb), (1u128 << eb) - 1);
    let zero = b.konst(Width::new(eb), 0);
    let m = b.extract(v, 0, Width::new(mb));
    let mz = b.konst(Width::new(mb), 0);
    let e1 = b.cmp(CmpOp::Eq, e, ones);
    let e0 = b.cmp(CmpOp::Eq, e, zero);
    let mnz = b.cmp(CmpOp::Ne, m, mz);
    let den = b.and(e0, mnz);
    b.or(e1, den)
}

/// 1 when converting `x` to a 32-bit integer (or a 64-bit one when `wide`)
/// is an unimplemented operation on the VR4300: |x| >= 2^31 (2^53).
fn int_out_of_range(b: &mut Builder, x: Value, w: Width, wide: bool) -> Value {
    let (eb, mb, bias) = if w == Width::W32 {
        (8, 23, 127u128)
    } else {
        (11, 52, 1023)
    };
    let e = b.extract(x, mb, Width::new(eb));
    let lim = b.konst(Width::new(eb), bias + if wide { 53 } else { 31 });
    b.cmp(CmpOp::Ule, lim, e)
}

/// 1 when converting the 64-bit integer `x` to a float is an unimplemented
/// operation on the VR4300: `x >= 2^55` or `x < -2^55`, that is, bits 55
/// to 63 are not all equal.
fn long_out_of_range(b: &mut Builder, x: Value) -> Value {
    let top = b.extract(x, 55, Width::new(9));
    let zero = b.konst(Width::new(9), 0);
    let ones = b.konst(Width::new(9), 0x1FF);
    let pos = b.cmp(CmpOp::Eq, top, zero);
    let neg = b.cmp(CmpOp::Eq, top, ones);
    let ok = b.or(pos, neg);
    b.not(ok)
}

/// 1 when the result `r` is zero but the operand `x` is not: an underflow
/// the fast path does not flush.
fn underflowed(b: &mut Builder, x: Value, w: Width, r: Value, ow: Width) -> Value {
    let zr = b.konst(ow, 0);
    let rm = b.konst(ow, (1u128 << (ow.bits() - 1)) - 1);
    let ra = b.and(r, rm);
    let rz = b.cmp(CmpOp::Eq, ra, zr);
    let m = b.konst(w, (1u128 << (w.bits() - 1)) - 1);
    let xa = b.and(x, m);
    let z = b.konst(w, 0);
    let xnz = b.cmp(CmpOp::Ne, xa, z);
    b.and(rz, xnz)
}
