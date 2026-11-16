//! The VR4300 frontend of the Nintendo 64 guest.
//!
//! Lifting is `relift-cpu-mips`'s, reshaped for the runtime's N64 layer
//! (`runtime/guests/n64/run`):
//!
//! - every load and store is an I/O access whose class the address map
//!   ([`io_map`]) gives: RDRAM is plain memory, anything else goes to the
//!   machine (`rl_n64_cpu_read`, `rl_n64_cpu_write`); an access the
//!   instruction makes only under a condition leaves through its guard when
//!   it is not in RDRAM, so a device sees only the accesses that happen;
//! - every instruction ticks its cycles (the lifter's), and every block
//!   ends in a sync point, where the runtime takes interrupts; code in SP
//!   memory (the boot code, uncached at its kseg1 address) also waits for
//!   the RCP bus at every fetch ([`SP_FETCH`]);
//! - a fetch that may start an instruction cache line calls
//!   `rl_mips_icache` with its address: a delay slot that starts a line
//!   here, and the first instruction of a block the program enters other
//!   than from the instruction before it, or that starts a line, through
//!   [`fetch_check`]; loads and stores call `rl_mips_dcache`;
//! - `JAL` and `JALR` with a link register are calls, which open a host
//!   frame (`rl_mips_call`); `JR` to the address on top of the frame stack
//!   returns from it;
//! - a read of Count asks the runtime for Count now (`rl_mips_count`);
//! - after a write of Status, Cause, Compare, Count or Wired the block ends
//!   its budget (`rl_n64_cp0_break`), so its sync point runs, and that sync
//!   point first applies the write (`rl_n64_cp0_sync`): the interrupt mask,
//!   the timer and an interrupt that is now due, with the block's register
//!   writes done;
//! - a TLB instruction outside a delay slot ends in a trap that runs after
//!   the block's register writes (`rl_n64_tlb`);
//! - `ERET` leaves before any effect to the runtime's return
//!   (`rl_mips_eret_to`), which closes an interrupt entry's host frame;
//! - a guard runs its instruction in the runtime (`rl_mips_guard`), and one
//!   in a delay slot also the branch (`rl_mips_guard_exit`).

use relift_cpu_mips::decode::Encoding;
use relift_cpu_mips::lift::{self, EXC_GUARD, FP_SLOW, IN_DELAY, LiftError, Options, Sys};
use relift_cpu_mips::regs;
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{
    AddrClass, Block, CryptoOp, Exit, Flow, FpOp, FpRound, Inst, IoMap, IoRange, Konst, Next, Op,
    Reg, RegFile, Value, Width,
};

/// The end of RDRAM: physical addresses below it are plain memory.
pub const RDRAM_END: u64 = 0x80_0000;

/// The intrinsic of a call's host frame: the return address.
pub const CALL: &str = "n64.call";
/// The intrinsic of a read of Count.
pub const COUNT: &str = "n64.count";
/// The intrinsic that ends the budget after a CP0 write the runtime must
/// see; the block's sync point then calls `rl_n64_cp0_sync`.
pub const CP0_SYNC: &str = "n64.cp0_sync";
/// The intrinsic of a fetch that may start an instruction cache line: its
/// address.
pub const ICACHE: &str = "n64.icache";

/// The bytes of an instruction cache line.
pub const ICACHE_LINE: u64 = 32;

/// The cycles an uncached fetch from SP memory waits for the RCP bus, as
/// any CPU read of an RCP device does.
pub const SP_FETCH: u32 = 20;

/// Whether `pc` is a kseg1 address in SP memory, where every fetch goes to
/// the RCP bus.
#[must_use]
pub fn in_sp_memory(pc: u64) -> bool {
    (0xFFFF_FFFF_A400_0000..0xFFFF_FFFF_A404_0000).contains(&pc)
}

/// The guard code of `ERET`.
pub const ERET_GUARD: u32 = 0x4000;
/// The trap code of a TLB instruction is this plus its operation (TLBR 0,
/// TLBWI 1, TLBWR 2, TLBP 3).
pub const TRAP_TLB: u32 = 0x200;

/// The address map: RDRAM is memory, everything else the machine's.
#[must_use]
pub fn io_map() -> IoMap {
    IoMap {
        ranges: vec![IoRange {
            first: 0,
            last: RDRAM_END - 1,
            read: AddrClass::Memory,
            write: AddrClass::Memory,
        }],
        default: AddrClass::Device,
    }
}

/// The context fields of the CP0 slots, in slot order from `CP0Index`.
const CP0_FIELDS: [&str; 23] = [
    "cp0_index",
    "cp0_entry_lo0",
    "cp0_entry_lo1",
    "cp0_context",
    "cp0_page_mask",
    "cp0_wired",
    "cp0_bad_vaddr",
    "cp0_count",
    "cp0_entry_hi",
    "cp0_compare",
    "cp0_status",
    "cp0_cause",
    "cp0_epc",
    "cp0_config",
    "cp0_ll_addr",
    "cp0_watch_lo",
    "cp0_watch_hi",
    "cp0_xcontext",
    "cp0_parity_err",
    "cp0_tag_lo",
    "cp0_error_epc",
    "cp0_latch",
    "cp0_ll_bit",
];

/// The `rl_ctx` field of slot `r` (`RL_MIPS_SLOTS` in `relift/mips/abi.h`).
#[must_use]
pub fn field(r: Reg) -> String {
    let i = r.index();
    match i {
        0..=31 => format!("r[{i}]"),
        32 => "hi".into(),
        33 => "lo".into(),
        34..=65 => format!("f[{}]", i - 34),
        66 => "fcr31".into(),
        _ => CP0_FIELDS.get(i - 67).copied().unwrap_or("?").into(),
    }
}

/// The frontend.
#[derive(Clone, Copy, Debug, Default)]
pub struct N64;

fn failure(word: u32, e: &LiftError) -> LiftFailure {
    LiftFailure {
        insn: u64::from(word),
        message: e.to_string(),
    }
}

fn push(b: &mut Block, op: Op, width: Option<Width>) -> Value {
    b.insts.push(Inst { op, width });
    Value((b.insts.len() - 1) as u32)
}

fn konst(b: &mut Block, w: Width, v: u64) -> Value {
    push(b, Op::Const(Konst::new(u128::from(v))), Some(w))
}

/// Makes the constant targets of `b`'s exit that `local` rejects computed
/// addresses, so the emitter sends the transfer through dispatch rather
/// than to a lifted function by name: code the runtime binds on its own
/// (another code run) may hold other bytes by the time the transfer runs.
pub fn dispatch_exit(b: &mut Block, local: impl Fn(u64) -> bool) {
    let w = b.addr_width;
    let sync = matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)).then(|| b.insts.pop());
    match b.exit.next {
        Next::Const(t) if matches!(b.exit.flow, Flow::Call { .. } | Flow::Jump) && !local(t) => {
            let v = konst(b, w, t);
            b.exit.next = Next::Dynamic(v);
        }
        Next::Select {
            cond,
            taken,
            not_taken,
        } if matches!(b.exit.flow, Flow::Jump) && (!local(taken) || !local(not_taken)) => {
            let t = konst(b, w, taken);
            let n = konst(b, w, not_taken);
            let v = push(b, Op::Select(cond, t, n), Some(w));
            b.exit.next = Next::Dynamic(v);
        }
        _ => {}
    }
    b.insts.extend(sync.flatten());
}

/// What the reshape does to a lifted instruction.
struct Shape {
    /// `ERET`: the guard that leaves to the runtime's return.
    eret: bool,
    /// The TLB operation the block's trap runs, taken out of the block.
    tlb_trap: Option<u32>,
    /// The return address of a call.
    call: Option<u64>,
}

/// Rebuilds `b` for the N64 layer: Count reads, the `ERET` guard, a TLB
/// instruction's intrinsic left to its trap, the call's host frame and the
/// closing sync point.
fn reshape(b: &mut Block, s: &Shape) {
    let old = std::mem::take(&mut b.insts);
    let mut map: Vec<Value> = Vec::with_capacity(old.len());
    for inst in old {
        let m = |v: &Value| map[v.index()];
        let op = match inst.op {
            Op::Get(r) if r == regs::CP0_COUNT => Op::intrinsic(COUNT, Vec::new()),
            Op::Tick(n) => {
                if s.eret {
                    let t = konst(b, Width::W1, 1);
                    push(b, Op::Guard(t, ERET_GUARD), None);
                }
                Op::Tick(n)
            }
            Op::Intrinsic(ref call) if s.tlb_trap.is_some() && call.name == "mips_tlb" => {
                map.push(Value(0));
                continue;
            }
            op => op.map_values(&m),
        };
        let v = push(b, op, inst.width);
        map.push(v);
    }
    b.exit.next = match b.exit.next {
        Next::Dynamic(v) => Next::Dynamic(map[v.index()]),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => Next::Select {
            cond: map[cond.index()],
            taken,
            not_taken,
        },
        next => next,
    };
    if let Some(ret) = s.call {
        let r = konst(b, Width::W64, ret);
        push(b, Op::intrinsic(CALL, vec![r]), None);
    }
    push(b, Op::Sync, None);
}

/// Inserts `ops` (which may use each other's values, numbered from `at`) in
/// front of `b.insts[at]`, renumbering the values from `at` on.
fn insert(b: &mut Block, at: usize, ops: Vec<Inst>) {
    let n = ops.len() as u32;
    let renum = |v: &Value| if v.index() >= at { Value(v.0 + n) } else { *v };
    let old = std::mem::take(&mut b.insts);
    let mut insts = Vec::with_capacity(old.len() + ops.len());
    for (i, inst) in old.into_iter().enumerate() {
        if i == at {
            insts.extend(ops.iter().cloned());
        }
        insts.push(Inst {
            op: inst.op.map_values(&renum),
            width: inst.width,
        });
    }
    if at == insts.len() {
        insts.extend(ops);
    }
    b.insts = insts;
    b.exit.next = match b.exit.next {
        Next::Dynamic(v) => Next::Dynamic(renum(&v)),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => Next::Select {
            cond: renum(&cond),
            taken,
            not_taken,
        },
        next => next,
    };
}

/// Inserts the fetch of `pc` ([`ICACHE`]) in front of `b.insts[at]`.
fn insert_fetch(b: &mut Block, at: usize, pc: u64) {
    let ops = vec![
        Inst {
            op: Op::Const(Konst::new(u128::from(pc))),
            width: Some(Width::W64),
        },
        Inst {
            op: Op::intrinsic(ICACHE, vec![Value(at as u32)]),
            width: None,
        },
    ];
    insert(b, at, ops);
}

/// Makes the block of the instruction at `pc` fetch it through the
/// instruction cache first: after its guards (a guard's slow path does its
/// own fetch) and before any other effect.
pub fn fetch_check(b: &mut Block, pc: u64) {
    let at = b
        .insts
        .iter()
        .rposition(|i| matches!(i.op, Op::Guard(..)))
        .map_or(0, |g| g + 1);
    insert_fetch(b, at, pc);
}

impl N64 {
    /// Lifts the instruction `word` at `pc`, with `slot` the word after it
    /// (its delay slot when it has one), for the N64 layer.
    ///
    /// # Errors
    ///
    /// The instruction does not lift.
    pub fn lift_at(self, pc: u64, word: u32, slot: u32) -> Result<Block, LiftFailure> {
        let opts = Options {
            plain_below: Some(RDRAM_END),
        };
        let l = lift::lift_with(pc, word, slot, opts).map_err(|e| failure(word, &e))?;
        let mut b = l.block;
        let enc = l.encs[0];
        let after = pc.wrapping_add(u64::from(b.len));
        let plain = l.encs.len() == 1 && b.exit.next == Next::Const(after);
        let link = match enc {
            Encoding::Jal => true,
            Encoding::Jalr => (word >> 11) & 31 != 0,
            _ => false,
        };
        let tlb = l.sys.iter().find_map(|s| match s {
            Sys::Tlb(op) => Some(*op),
            _ => None,
        });
        let shape = Shape {
            eret: enc == Encoding::Eret,
            tlb_trap: tlb.filter(|_| plain),
            call: link.then_some(pc.wrapping_add(8)),
        };
        if link {
            b.exit.flow = Flow::Call {
                ret: pc.wrapping_add(8),
            };
        }
        let sync = l.sys.contains(&Sys::Cp0Sync)
            || b.insts.iter().any(
                |i| matches!(i.op, Op::Set(r, _) if r == regs::CP0_COUNT || r == regs::CP0_WIRED),
            );
        reshape(&mut b, &shape);
        if in_sp_memory(pc) {
            for i in &mut b.insts {
                if let Op::Tick(n) = &mut i.op {
                    *n += SP_FETCH;
                }
            }
        }
        let slot_pc = pc.wrapping_add(4);
        if l.encs.len() == 2
            && slot_pc.is_multiple_of(ICACHE_LINE)
            && let Some(t) = b.insts.iter().position(|i| matches!(i.op, Op::Tick(_)))
        {
            insert_fetch(&mut b, t + 1, slot_pc);
        }
        if let Some(op) = shape.tlb_trap {
            b.exit = Exit {
                next: Next::Const(after),
                flow: Flow::Trap(TRAP_TLB + op),
            };
        }
        if sync {
            let at = b.insts.len() - 1;
            b.insts.insert(
                at,
                Inst {
                    op: Op::intrinsic(CP0_SYNC, Vec::new()),
                    width: None,
                },
            );
        }
        Ok(b)
    }
}

fn be32(bytes: &[u8], at: usize) -> u32 {
    bytes
        .get(at..at + 4)
        .map_or(0, |w| u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
}

fn rmode(r: FpRound) -> u32 {
    match r {
        FpRound::TiesEven => 0,
        FpRound::TiesAway => 1,
        FpRound::Floor => 2,
        FpRound::Ceil => 3,
        FpRound::Zero => 4,
    }
}

impl Frontend for N64 {
    fn granule(&self) -> u64 {
        4
    }

    fn regs(&self) -> RegFile {
        regs::regs()
    }

    fn stack_pointer(&self) -> Option<Reg> {
        Some(regs::gpr(29))
    }

    fn preserved(&self) -> Vec<Reg> {
        (16..=23).chain([29, 30]).map(regs::gpr).collect()
    }

    /// Lifts the word at the start of `bytes` (big-endian) with the next
    /// word as its delay slot.
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        self.lift_at(pc, be32(bytes, 0), be32(bytes, 4))
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/mips/abi.h", "relift/n64/run.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        Home::Field(format!("c->{}", field(reg)))
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Some(match (name, args) {
            ("mips_fcr31", [op, fmt, fcr, a, b]) => format!(
                "rl_mips_fcr31((uint32_t)({op}), (uint32_t)({fmt}), (uint32_t)({fcr}), (uint64_t)({a}), (uint64_t)({b}))"
            ),
            ("mips_fcmp", [cond, fmt, fcr, a, b]) => format!(
                "rl_mips_fcmp((uint32_t)({cond}), (uint32_t)({fmt}), (uint32_t)({fcr}), (uint64_t)({a}), (uint64_t)({b}))"
            ),
            ("mips_random", []) => "rl_mips_random(c)".into(),
            ("mips_tlb", [op, run]) => {
                format!("rl_mips_tlb(c, (uint32_t)({op}), (uint32_t)({run}))")
            }
            ("mips_cache", [op, va, run]) => {
                format!("rl_mips_cache(c, (uint32_t)({op}), (uint64_t)({va}), (uint32_t)({run}))")
            }
            ("mips_dcache", [va, w, run]) => {
                format!("rl_mips_dcache(c, (uint64_t)({va}), (uint32_t)({w}), (uint32_t)({run}))")
            }
            (ICACHE, [pc]) => format!("rl_mips_icache(c, (uint64_t)({pc}))"),
            (COUNT, []) => "rl_mips_count(c)".into(),
            (CALL, [ret]) => format!("rl_mips_call(c, (uint64_t)({ret}))"),
            (CP0_SYNC, []) => "rl_n64_cp0_break(c)".into(),
            _ => return None,
        })
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_n64_tlb(c, mem, {}u)", code.wrapping_sub(TRAP_TLB))
    }

    fn sync_call(&self, b: &Block, next: &str) -> Option<String> {
        b.insts
            .iter()
            .any(|i| matches!(&i.op, Op::Intrinsic(c) if c.name == CP0_SYNC))
            .then(|| format!("rl_n64_cp0_sync(c, mem, {next})"))
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        format!(
            "rl_undefined(c, mem, UINT64_C({pc:#x}), {:#x}u)",
            insn as u32
        )
    }

    fn monitor(&self) -> &'static str {
        "0"
    }

    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        _reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        let (f, t) = (from.bits(), to.bits());
        let a = |i: usize| args.get(i).cloned().unwrap_or_default();
        Some(match op {
            FpOp::Add => format!("rl_mips_fadd{t}({}, {})", a(0), a(1)),
            FpOp::Sub => format!("rl_mips_fsub{t}({}, {})", a(0), a(1)),
            FpOp::Mul => format!("rl_mips_fmul{t}({}, {})", a(0), a(1)),
            FpOp::Div => format!("rl_mips_fdiv{t}({}, {})", a(0), a(1)),
            FpOp::Sqrt => format!("rl_mips_fsqrt{t}({})", a(0)),
            FpOp::ToSInt(r) => format!("rl_mips_f2i({}, {f}u, {t}u, {}u)", a(0), rmode(r)),
            FpOp::FromSInt => format!("rl_mips_i2f({}, {f}u, {t}u)", a(0)),
            FpOp::Convert => format!("rl_mips_fcvt({}, {f}u, {t}u)", a(0)),
            _ => return None,
        })
    }

    fn crypto(&self, _op: CryptoOp, _arg: &str) -> Option<String> {
        None
    }

    fn clock(&self) -> Option<&'static str> {
        Some("c->time")
    }

    fn io_calls(&self) -> (&'static str, &'static str) {
        ("rl_n64_cpu_read", "rl_n64_cpu_write")
    }

    fn fp_state(&self) -> Vec<Reg> {
        Vec::new()
    }

    fn guard_state(&self, _code: u32) -> Option<Reg> {
        None
    }

    fn guard_exit(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        if code == ERET_GUARD {
            return Some("rl_mips_eret_to(c, rl_mips_eret(c))".into());
        }
        (code & IN_DELAY != 0 && code & (EXC_GUARD | FP_SLOW) != 0).then(|| {
            format!(
                "rl_mips_guard_exit(c, mem, {code:#x}u, UINT64_C({pc:#x}), {:#x}u, {:#x}u)",
                insn as u32,
                (insn >> 32) as u32
            )
        })
    }

    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        (code & (EXC_GUARD | FP_SLOW) != 0).then(|| {
            format!(
                "rl_mips_guard(c, mem, {code:#x}u, UINT64_C({pc:#x}), {:#x}u)",
                insn as u32
            )
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PC: u64 = 0xFFFF_FFFF_8000_1000;

    fn ops(b: &Block) -> Vec<&Op> {
        b.insts.iter().map(|i| &i.op).collect()
    }

    fn intrinsics(b: &Block) -> Vec<&'static str> {
        b.insts
            .iter()
            .filter_map(|i| match &i.op {
                Op::Intrinsic(c) => Some(c.name),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn code_in_sp_memory_waits_for_the_rcp_bus_at_each_fetch() {
        let ticks = |pc: u64, word: u32| -> Vec<u32> {
            let b = N64.lift_at(pc, word, 0).expect("lifts");
            b.insts
                .iter()
                .filter_map(|i| match i.op {
                    Op::Tick(n) => Some(n),
                    _ => None,
                })
                .collect()
        };
        let addiu = 0x2442_0001;
        let beq = 0x1000_0002;
        assert_eq!(ticks(PC, addiu), [1]);
        assert_eq!(ticks(0xFFFF_FFFF_A400_0040, addiu), [1 + SP_FETCH]);
        assert_eq!(
            ticks(0xFFFF_FFFF_A400_0040, beq),
            [1 + SP_FETCH, 1 + SP_FETCH]
        );
    }

    #[test]
    fn every_block_ends_in_a_sync_point_and_verifies() {
        let lw = 0x8C22_0000;
        let b = N64.lift_at(PC, lw, 0).expect("lifts");
        b.verify().expect("verifies");
        assert!(matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)));
        assert!(ops(&b).iter().any(|o| matches!(o, Op::Tick(1))));
    }

    #[test]
    fn a_transfer_through_dispatch_keeps_its_sync_point_last() {
        let mut b = N64.lift_at(PC, 0x0C00_0800, 0).expect("lifts");
        dispatch_exit(&mut b, |_| false);
        assert!(matches!(b.exit.next, Next::Dynamic(_)));
        assert!(matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)));
        b.verify().expect("verifies");
    }

    #[test]
    fn jal_is_a_call_that_opens_a_frame() {
        let b = N64.lift_at(PC, 0x0C00_0800, 0).expect("lifts");
        b.verify().expect("verifies");
        assert_eq!(b.exit.flow, Flow::Call { ret: PC + 8 });
        assert_eq!(b.exit.next, Next::Const(0xFFFF_FFFF_8000_2000));
        assert!(intrinsics(&b).contains(&CALL));
        let jr = N64.lift_at(PC, 0x03E0_0008, 0).expect("lifts");
        assert_eq!(jr.exit.flow, Flow::Jump);
        let jalr0 = N64.lift_at(PC, 0x0080_0009, 0).expect("lifts");
        assert_eq!(jalr0.exit.flow, Flow::Jump, "JALR r0 links nothing");
    }

    #[test]
    fn count_reads_ask_the_runtime_and_cp0_writes_trap() {
        let mfc0_count = 0x4002_4800;
        let b = N64.lift_at(PC, mfc0_count, 0).expect("lifts");
        b.verify().expect("verifies");
        assert!(intrinsics(&b).contains(&COUNT));
        assert!(
            !ops(&b)
                .iter()
                .any(|o| matches!(o, Op::Get(r) if *r == regs::CP0_COUNT))
        );
        let mtc0_status = 0x4082_6000;
        let b = N64.lift_at(PC, mtc0_status, 0).expect("lifts");
        b.verify().expect("verifies");
        assert!(intrinsics(&b).contains(&CP0_SYNC));
        assert_eq!(
            N64.sync_call(&b, "n").as_deref(),
            Some("rl_n64_cp0_sync(c, mem, n)")
        );
        let mtc0_count = 0x4082_4800;
        let b = N64.lift_at(PC, mtc0_count, 0).expect("lifts");
        assert!(intrinsics(&b).contains(&CP0_SYNC));
        let lw = N64.lift_at(PC, 0x8C22_0000, 0).expect("lifts");
        assert!(N64.sync_call(&lw, "n").is_none());
        let tlbwi = 0x4200_0002;
        let b = N64.lift_at(PC, tlbwi, 0).expect("lifts");
        b.verify().expect("verifies");
        assert_eq!(b.exit.flow, Flow::Trap(TRAP_TLB + 1));
        assert!(!intrinsics(&b).contains(&"mips_tlb"));
        assert_eq!(N64.trap(TRAP_TLB + 1), "rl_n64_tlb(c, mem, 1u)");
    }

    #[test]
    fn eret_leaves_to_the_runtime_before_any_effect() {
        let b = N64.lift_at(PC, 0x4200_0018, 0).expect("lifts");
        b.verify().expect("verifies");
        let o = ops(&b);
        let g = o
            .iter()
            .position(|o| matches!(o, Op::Guard(_, c) if *c == ERET_GUARD))
            .expect("the ERET guard");
        let t = o
            .iter()
            .position(|o| matches!(o, Op::Tick(_)))
            .expect("a tick");
        assert!(g < t);
        assert_eq!(
            N64.guard_exit(ERET_GUARD, PC, 0).as_deref(),
            Some("rl_mips_eret_to(c, rl_mips_eret(c))")
        );
    }

    #[test]
    fn fetches_that_may_start_a_cache_line_go_through_the_icache() {
        let beq = 0x1000_0003;
        let lw = 0x8C22_0000;
        let b = N64.lift_at(PC + 0x1C, beq, lw).expect("lifts");
        b.verify().expect("verifies");
        let o = ops(&b);
        let tick = o
            .iter()
            .position(|o| matches!(o, Op::Tick(_)))
            .expect("a tick");
        let fetch = o
            .iter()
            .position(|o| matches!(o, Op::Intrinsic(c) if c.name == ICACHE))
            .expect("the slot's fetch");
        let load = o
            .iter()
            .position(|o| matches!(o, Op::IoLoad { .. }))
            .expect("the slot's load");
        assert!(tick < fetch && fetch < load);
        let b = N64.lift_at(PC, beq, lw).expect("lifts");
        assert!(
            !intrinsics(&b).contains(&ICACHE),
            "the slot is in the branch's line"
        );
        let mut b = N64.lift_at(PC, lw, 0).expect("lifts");
        fetch_check(&mut b, PC);
        b.verify().expect("verifies");
        let o = ops(&b);
        let guard = o
            .iter()
            .rposition(|o| matches!(o, Op::Guard(..)))
            .expect("a guard");
        let fetch = o
            .iter()
            .position(|o| matches!(o, Op::Intrinsic(c) if c.name == ICACHE))
            .expect("the fetch");
        assert!(guard < fetch);
        assert!(
            o[..fetch]
                .iter()
                .all(|o| !o.has_effect() || matches!(o, Op::Guard(..))),
            "no effect before the fetch"
        );
        assert_eq!(
            N64.intrinsic(ICACHE, &["p".into()]).as_deref(),
            Some("rl_mips_icache(c, (uint64_t)(p))")
        );
        assert!(intrinsics(&b).contains(&"mips_dcache"));
    }

    #[test]
    fn guards_name_their_slow_paths() {
        let code = EXC_GUARD + 2;
        assert_eq!(
            N64.guard_slow(code, PC, 0x8C22_0000).as_deref(),
            Some("rl_mips_guard(c, mem, 0x1002u, UINT64_C(0xffffffff80001000), 0x8c220000u)")
        );
        assert!(N64.guard_exit(code, PC, 0).is_none());
        let s = N64
            .guard_exit(code + IN_DELAY, PC, 0x8C22_0000_1000_0003)
            .expect("an exit");
        assert_eq!(
            s,
            "rl_mips_guard_exit(c, mem, 0x1102u, UINT64_C(0xffffffff80001000), 0x10000003u, 0x8c220000u)"
        );
    }

    #[test]
    fn the_fields_follow_the_c_context() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../runtime/cpus/mips/include/relift/mips/abi.h"
        );
        let text = std::fs::read_to_string(path).expect("reads abi.h");
        let start = text
            .find("#define RL_MIPS_SLOTS(X)")
            .expect("the slot list");
        let mut n = 0;
        for line in text[start..].lines().skip(1) {
            let line = line.trim().trim_end_matches('\\').trim();
            let Some(body) = line.strip_prefix("X(").and_then(|l| l.strip_suffix(')')) else {
                break;
            };
            let parts: Vec<&str> = body.split(',').map(str::trim).collect();
            assert_eq!(field(Reg(n)), parts[1], "slot {n}");
            n += 1;
        }
        assert_eq!(usize::from(n), regs::regs().len());
    }
}
