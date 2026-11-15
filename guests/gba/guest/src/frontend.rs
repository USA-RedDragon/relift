//! The ARMv4T frontend of the Game Boy Advance guest.
//!
//! Lifting is `relift-cpu-arm32`'s, reshaped for the runtime's GBA layer
//! (`runtime/guests/gba/run`):
//!
//! - every load and store goes to the machine's bus (`rl_gba_cpu_read`,
//!   `rl_gba_cpu_write`), which charges the access's wait states;
//! - the instruction's bus cycles reach the machine in the ARM7TDMI's order:
//!   the fetch two instructions ahead first (`rl_gba_cpu_fetch`, before the
//!   condition check, since a failed instruction fetches too), then the data
//!   accesses, then the internal cycles (`rl_gba_cpu_idle` in place of the
//!   block's ticks), then a taken branch's refill (`rl_gba_cpu_refill`);
//! - a write of the I bit tells the runtime (`rl_arm32_set_i`), so the IRQ
//!   line is masked exactly while I is set;
//! - SWI is a trap into the high-level BIOS (`rl_gba_swi_trap`), which goes
//!   on at the next instruction;
//! - the second half of a Thumb BL pair calls the constant its first half
//!   puts in LR;
//! - every block ends in a sync point, where the runtime takes interrupts.

use std::collections::BTreeSet;

use relift_cpu_arm32::lift::{self, Bus, COND_FAIL, LiftError};
use relift_cpu_arm32::{decode, regs};
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{
    AddrClass, Block, CryptoOp, Exit, Flow, FpOp, Inst, IoKind, IoMap, Konst, Next, Op, Reg,
    RegFile, Value, Width,
};

/// Bit of a SWI trap code set for a SWI in Thumb state; bits 0 to 7 hold
/// the comment byte (`RL_GBA_SWI_THUMB`).
pub const SWI_THUMB: u32 = 0x100;

/// Set in the raw instruction unit of a Thumb instruction, above its
/// halfword, so the slow paths know the state.
pub const RAW_THUMB: u64 = 1 << 32;

/// The intrinsic of the fetch of the next ARM instruction.
pub const FETCH_ARM: &str = "arm32.fetch_arm";
/// The intrinsic of the fetch of the next Thumb instruction.
pub const FETCH_THUMB: &str = "arm32.fetch_thumb";
/// The intrinsic of a branch's pipeline refill: target, Thumb, taken.
pub const REFILL: &str = "arm32.refill";
/// The intrinsic of internal cycles: their count.
pub const IDLE: &str = "arm32.idle";
/// The intrinsic of internal cycles taken at run time: one bit and the count.
pub const IDLE_IF: &str = "arm32.idle_if";
/// The intrinsic of a write of the I bit.
pub const SET_I: &str = "arm32.set_i";
/// The intrinsic of a call's host frame: the return address.
pub const CALL: &str = "arm32.call";

/// The address map: every access reaches the machine's bus.
#[must_use]
pub fn device_map() -> IoMap {
    IoMap {
        ranges: Vec::new(),
        default: AddrClass::Device,
    }
}

/// The frontend.
#[derive(Clone, Debug, Default)]
pub struct Gba {
    /// The computed jumps that are calls (LR holds the next instruction).
    calls: BTreeSet<u32>,
}

fn failure(word: u32, e: &LiftError) -> LiftFailure {
    LiftFailure {
        insn: u64::from(word),
        message: e.to_string(),
    }
}

/// The instruction unit at `pc` in `image`, which starts at `base`.
fn unit(image: &[u8], base: u32, pc: u32, thumb: bool) -> Option<u32> {
    let off = usize::try_from(pc.checked_sub(base)?).ok()?;
    let n = if thumb { 2 } else { 4 };
    let b = image.get(off..off + n)?;
    Some(if thumb {
        u32::from(u16::from_le_bytes([b[0], b[1]]))
    } else {
        u32::from_le_bytes([b[0], b[1], b[2], b[3]])
    })
}

/// The target of the Thumb BL pair whose second half is `lo` at `pc` and
/// first half `hi` at `pc - 2`, or `None` when the halves are no pair.
#[must_use]
pub fn bl_pair_target(hi: u32, lo: u32, pc: u32) -> Option<u32> {
    if hi & 0xF800 != 0xF000 || lo & 0xF800 != 0xF800 {
        return None;
    }
    let off = ((hi & 0x7FF) << 21) as i32 >> 9;
    let lr = pc.wrapping_add(2).wrapping_add(off as u32);
    Some(lr.wrapping_add((lo & 0x7FF) << 1) & !1)
}

fn push(b: &mut Block, op: Op, width: Option<Width>) -> Value {
    b.insts.push(Inst { op, width });
    Value((b.insts.len() - 1) as u32)
}

fn konst(b: &mut Block, w: Width, v: u64) -> Value {
    push(b, Op::Const(Konst::new(u128::from(v))), Some(w))
}

fn is_const(b: &Block, v: Value, want: u64) -> bool {
    matches!(b.insts[v.index()].op, Op::Const(k) if k.get() == u128::from(want))
}

fn last_set(b: &Block, r: Reg) -> Option<Value> {
    b.insts.iter().rev().find_map(|i| match i.op {
        Op::Set(x, v) if x == r => Some(v),
        _ => None,
    })
}

impl Gba {
    /// The frontend.
    #[must_use]
    pub fn new() -> Gba {
        Gba::default()
    }

    /// The frontend, with the computed jumps at `calls` lifted as calls.
    #[must_use]
    pub fn with_calls(calls: BTreeSet<u32>) -> Gba {
        Gba { calls }
    }

    /// Lifts the instruction at `pc` in Thumb (`thumb`) or ARM state from
    /// `image`, loaded at `base`, for the GBA layer.
    ///
    /// # Errors
    ///
    /// The address is outside the image, or the instruction does not lift.
    pub fn lift_at(
        &self,
        image: &[u8],
        base: u32,
        pc: u32,
        thumb: bool,
    ) -> Result<Block, LiftFailure> {
        let word = unit(image, base, pc, thumb).ok_or_else(|| LiftFailure {
            insn: 0,
            message: format!("{pc:#010x} is outside the image"),
        })?;
        let l = lift::lift(word, pc, thumb).map_err(|e| failure(word, &e))?;
        let mut b = l.block;
        let len = if thumb { 2u64 } else { 4 };
        let fall = u64::from(pc) + len;
        if thumb
            && b.exit.flow == (Flow::Call { ret: fall })
            && let Next::Dynamic(_) = b.exit.next
            && let Some(hi) = pc.checked_sub(2).and_then(|p| unit(image, base, p, true))
            && let Some(t) = bl_pair_target(hi, word, pc)
        {
            b.exit.next = Next::Const(u64::from(t));
        }
        if b.exit.flow == Flow::Return {
            b.exit.flow = Flow::Jump;
        }
        if b.exit.flow == Flow::Jump
            && matches!(b.exit.next, Next::Dynamic(_))
            && self.calls.contains(&pc)
        {
            b.exit.flow = Flow::Call { ret: fall };
        }
        let swi = b.exit.flow == (Flow::Call { ret: fall }) && b.exit.next == Next::Const(0x08);
        if swi {
            let number = if thumb {
                word & 0xFF
            } else {
                (word >> 16) & 0xFF
            };
            b.exit = Exit {
                next: Next::Const(fall),
                flow: Flow::Trap(number | if thumb { SWI_THUMB } else { 0 }),
            };
        }
        reshape(&mut b, &l.bus, pc, thumb, swi);
        Ok(b)
    }
}

/// The address before alignment, when instruction `k` aligns one: the
/// templates clear the low bits (`concat(extract(x, n), 0)`), and the bus
/// is given the address the CPU puts out, which the machine aligns except
/// for the 8-bit backup, whose byte lane it selects.
fn unaligned(b: &Block, k: usize) -> Option<Value> {
    let Op::Concat(h, l) = b.insts[k].op else {
        return None;
    };
    let Op::Const(z) = b.insts[l.index()].op else {
        return None;
    };
    let Op::Extract(x, n) = b.insts[h.index()].op else {
        return None;
    };
    let lw = b.width_of(l)?.bits();
    (z.get() == 0 && u32::from(n) == lw && b.width_of(x) == Some(Width::W32)).then_some(x)
}

/// Puts the fetch first, routes memory to the bus, turns the ticks into
/// internal cycles on the bus, adds the refill, reports writes of I and
/// closes the block with a sync point.
fn reshape(b: &mut Block, bus: &[Bus], pc: u32, thumb: bool, swi: bool) {
    let internal: u32 = bus
        .iter()
        .map(|e| match e {
            Bus::I(n) => *n,
            _ => 0,
        })
        .sum();
    let unaligned: Vec<Option<Value>> = (0..b.insts.len()).map(|k| unaligned(b, k)).collect();
    let old = std::mem::take(&mut b.insts);
    let at = konst(b, Width::W32, u64::from(pc));
    let name = if thumb { FETCH_THUMB } else { FETCH_ARM };
    let fetch = push(b, Op::intrinsic(name, vec![at]), None);
    let mut map: Vec<Value> = Vec::with_capacity(old.len());
    for inst in old {
        let m = |v: &Value| map[v.index()];
        let addr_of = |a: Value| m(&unaligned[a.index()].unwrap_or(a));
        let op = match inst.op {
            Op::Load { addr, endian, .. } => Op::IoLoad {
                addr: addr_of(addr),
                endian,
                kind: IoKind::Device,
            },
            Op::Store {
                addr,
                value,
                endian,
                ..
            } => Op::IoStore {
                addr: addr_of(addr),
                value: m(&value),
                endian,
                kind: IoKind::Device,
            },
            Op::Tick(_) if internal == 0 => {
                map.push(fetch);
                continue;
            }
            Op::Tick(_) => {
                let n = konst(b, Width::W32, u64::from(internal));
                Op::intrinsic(IDLE, vec![n])
            }
            Op::TickIf(cond, n) => {
                let n = konst(b, Width::W32, u64::from(n));
                Op::intrinsic(IDLE_IF, vec![m(&cond), n])
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
    let refills = bus.iter().any(|e| matches!(e, Bus::Code(false, _)));
    if refills && !swi {
        let fall = u64::from(pc) + if thumb { 2 } else { 4 };
        let (target, taken) = match (b.exit.flow, b.exit.next) {
            (_, Next::Const(t)) => (konst(b, Width::W32, t), konst(b, Width::W1, 1)),
            (_, Next::Select { cond, taken: t, .. }) => (konst(b, Width::W32, t), cond),
            (_, Next::Dynamic(v)) => match b.insts[v.index()].op {
                Op::Select(g, _, f) if is_const(b, f, fall) => (v, g),
                _ => (v, konst(b, Width::W1, 1)),
            },
        };
        let t = match last_set(b, regs::T) {
            Some(v) => v,
            None => push(b, Op::Get(regs::T), Some(Width::W1)),
        };
        push(b, Op::intrinsic(REFILL, vec![target, t, taken]), None);
    }
    if let Flow::Call { ret } = b.exit.flow
        && !swi
    {
        let r = konst(b, Width::W32, ret);
        push(b, Op::intrinsic(CALL, vec![r]), None);
    }
    if let Some(v) = last_set(b, regs::I) {
        push(b, Op::intrinsic(SET_I, vec![v]), None);
    }
    push(b, Op::Sync, None);
}

fn field(r: Reg) -> String {
    let i = r.index();
    match i {
        0..=14 => format!("c->r[{i}]"),
        15 => "c->n".into(),
        16 => "c->z".into(),
        17 => "c->c".into(),
        18 => "c->v".into(),
        19 => "c->t".into(),
        20 => "c->m".into(),
        21 => "c->i".into(),
        22 => "c->f".into(),
        23 => "c->psr_rsvd".into(),
        24..=28 => format!("c->spsr[{}]", i - 24),
        29..=35 => format!("c->bank_usr[{}]", i - 29),
        36..=42 => format!("c->bank_fiq[{}]", i - 36),
        43 | 44 => format!("c->bank_irq[{}]", i - 43),
        45 | 46 => format!("c->bank_svc[{}]", i - 45),
        47 | 48 => format!("c->bank_abt[{}]", i - 47),
        _ => format!("c->bank_und[{}]", i - 49),
    }
}

impl Frontend for Gba {
    fn granule(&self) -> u64 {
        2
    }

    fn regs(&self) -> RegFile {
        regs::regs()
    }

    fn stack_pointer(&self) -> Option<Reg> {
        Some(regs::r(13))
    }

    fn preserved(&self) -> Vec<Reg> {
        (4..12).chain([13]).map(regs::r).collect()
    }

    /// Lifts in ARM state at a word-aligned `pc` and in Thumb state
    /// otherwise; the build lifts through [`Gba::lift_at`], which is told
    /// the state.
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let pc32 = pc as u32;
        self.lift_at(bytes, pc32, pc32, pc32 & 3 != 0)
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/arm32/abi.h", "relift/gba/run.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        Home::Field(field(reg))
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Some(match (name, args) {
            (FETCH_ARM, [pc]) => format!("rl_gba_cpu_fetch(c, (uint32_t)({pc}), 0u)"),
            (FETCH_THUMB, [pc]) => format!("rl_gba_cpu_fetch(c, (uint32_t)({pc}), 1u)"),
            (REFILL, [t, thumb, taken]) => format!(
                "rl_gba_cpu_refill(c, (uint32_t)({t}), (unsigned)({thumb}), (unsigned)({taken}))"
            ),
            (IDLE, [n]) => format!("rl_gba_cpu_idle(c, (uint32_t)({n}))"),
            (IDLE_IF, [on, n]) => format!("rl_gba_cpu_idle(c, (uint32_t)({on}) * (uint32_t)({n}))"),
            (SET_I, [i]) => format!("rl_arm32_set_i(c, (uint32_t)({i}))"),
            (CALL, [r]) => format!("rl_arm32_call(c, (uint32_t)({r}))"),
            _ => return None,
        })
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_gba_swi_trap(c, mem, {code:#x}u)")
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
        _op: FpOp,
        _from: Width,
        _to: Width,
        _args: &[String],
        _reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        None
    }

    fn crypto(&self, _op: CryptoOp, _arg: &str) -> Option<String> {
        None
    }

    fn clock(&self) -> Option<&'static str> {
        Some("c->time")
    }

    fn io_calls(&self) -> (&'static str, &'static str) {
        ("rl_gba_cpu_read", "rl_gba_cpu_write")
    }

    fn fp_state(&self) -> Vec<Reg> {
        Vec::new()
    }

    fn guard_state(&self, _code: u32) -> Option<Reg> {
        None
    }

    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        (code == COND_FAIL).then(|| {
            format!(
                "rl_gba_cpu_cond_fail(c, mem, UINT64_C({:#x}))",
                pc + if insn & RAW_THUMB != 0 { 2 } else { 4 }
            )
        })
    }
}

/// Whether the Thumb halfword `h` is the first half of a BL pair.
#[must_use]
pub fn is_bl_prefix(h: u32) -> bool {
    decode::decode_t16(h as u16).is_some_and(|e| e.name() == "BLPRE_T1")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn arm(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    fn thumb(halves: &[u16]) -> Vec<u8> {
        halves.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    const BASE: u32 = 0x0800_0000;

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
    fn loads_and_stores_go_to_the_bus_and_the_block_ends_in_a_sync_point() {
        let img = arm(&[0xE591_0004, 0xE581_0008]);
        for pc in [BASE, BASE + 4] {
            let b = Gba::new().lift_at(&img, BASE, pc, false).expect("lifts");
            assert!(
                b.insts
                    .iter()
                    .any(|i| matches!(i.op, Op::IoLoad { .. } | Op::IoStore { .. }))
            );
            assert!(
                !b.insts
                    .iter()
                    .any(|i| matches!(i.op, Op::Load { .. } | Op::Store { .. }))
            );
            assert!(matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)));
            assert_eq!(intrinsics(&b)[0], FETCH_ARM);
        }
    }

    #[test]
    fn the_bus_gets_the_address_before_alignment() {
        let img = arm(&[0xE1C1_00B1]);
        let b = Gba::new().lift_at(&img, BASE, BASE, false).expect("lifts");
        let addr = b
            .insts
            .iter()
            .find_map(|i| match i.op {
                Op::IoStore { addr, .. } => Some(addr),
                _ => None,
            })
            .expect("a store");
        assert!(!matches!(b.insts[addr.index()].op, Op::Concat(..)));
    }

    #[test]
    fn the_fetch_comes_first_and_the_internal_cycles_after_the_data() {
        let img = arm(&[0xE591_0004]);
        let b = Gba::new().lift_at(&img, BASE, BASE, false).expect("lifts");
        assert!(!b.insts.iter().any(|i| matches!(i.op, Op::Tick(_))));
        assert_eq!(intrinsics(&b), [FETCH_ARM, IDLE]);
        let at = |f: &dyn Fn(&Op) -> bool| b.insts.iter().position(|i| f(&i.op)).expect("found");
        let load = at(&|o| matches!(o, Op::IoLoad { .. }));
        let idle = at(&|o| matches!(o, Op::Intrinsic(c) if c.name == IDLE));
        assert!(load < idle);
        let guarded = arm(&[0x0591_0004]);
        let b = Gba::new()
            .lift_at(&guarded, BASE, BASE, false)
            .expect("lifts");
        let fetch = b
            .insts
            .iter()
            .position(|i| matches!(&i.op, Op::Intrinsic(c) if c.name == FETCH_ARM));
        let guard = b.insts.iter().position(|i| matches!(i.op, Op::Guard(..)));
        assert!(fetch < guard);
    }

    #[test]
    fn thumb_shifts_by_register_take_an_internal_cycle() {
        for op in [0x40A0, 0x40E0, 0x4120, 0x41E0] {
            let b = Gba::new()
                .lift_at(&thumb(&[op]), BASE, BASE, true)
                .expect("lifts");
            assert_eq!(intrinsics(&b), [FETCH_THUMB, IDLE], "{op:#06x}");
        }
    }

    #[test]
    fn a_branch_refills_at_its_target() {
        let img = arm(&[0xEA00_0002]);
        let b = Gba::new().lift_at(&img, BASE, BASE, false).expect("lifts");
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 16));
        assert_eq!(intrinsics(&b), [FETCH_ARM, REFILL]);
        let img = thumb(&[0xD0FE]);
        let b = Gba::new().lift_at(&img, BASE, BASE, true).expect("lifts");
        assert_eq!(intrinsics(&b), [FETCH_THUMB, REFILL]);
    }

    #[test]
    fn swi_is_a_trap_into_the_bios_that_goes_on_after_it() {
        let img = arm(&[0xEF05_0000]);
        let b = Gba::new().lift_at(&img, BASE, BASE, false).expect("lifts");
        assert_eq!(b.exit.flow, Flow::Trap(0x05));
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 4));
        assert_eq!(intrinsics(&b), [FETCH_ARM, SET_I]);
        let img = thumb(&[0xDF0B]);
        let b = Gba::new().lift_at(&img, BASE, BASE, true).expect("lifts");
        assert_eq!(b.exit.flow, Flow::Trap(0x0B | SWI_THUMB));
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 2));
    }

    #[test]
    fn the_second_half_of_a_bl_pair_calls_a_constant() {
        let img = thumb(&[0xF000, 0xF80E]);
        let b = Gba::new()
            .lift_at(&img, BASE, BASE + 2, true)
            .expect("lifts");
        assert_eq!(b.exit.next, Next::Const(u64::from(BASE) + 4 + 0x1C));
        assert_eq!(intrinsics(&b), [FETCH_THUMB, REFILL, CALL]);
        assert_eq!(
            b.exit.flow,
            Flow::Call {
                ret: u64::from(BASE) + 4
            }
        );
        assert_eq!(
            bl_pair_target(0xF7FF, 0xFFFE, BASE + 2),
            Some(BASE + 4 - 0x1000 + 0xFFC)
        );
        assert!(is_bl_prefix(0xF000));
        assert!(!is_bl_prefix(0xF800));
    }

    #[test]
    fn msr_reports_the_i_bit() {
        let img = arm(&[0xE121_F000]);
        let b = Gba::new().lift_at(&img, BASE, BASE, false).expect("lifts");
        assert_eq!(intrinsics(&b), [FETCH_ARM, SET_I]);
    }

    #[test]
    fn every_slot_has_a_context_field() {
        let fe = Gba::new();
        let file = fe.regs();
        let names: Vec<String> = (0..file.len())
            .map(|i| match fe.home(Reg(i as u16)) {
                Home::Field(f) => f,
                Home::Flag { .. } => String::new(),
            })
            .collect();
        assert_eq!(names[13], "c->r[13]");
        assert_eq!(names[19], "c->t");
        assert_eq!(names[26], "c->spsr[2]");
        assert_eq!(names[34], "c->bank_usr[5]");
        assert_eq!(names[43], "c->bank_irq[0]");
        assert_eq!(names[50], "c->bank_und[1]");
        assert_eq!(names.len(), 51);
    }
}
