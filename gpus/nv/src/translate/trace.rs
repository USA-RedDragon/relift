//! Trace mode: a translation that records, for each guest instruction,
//! the first invocation whose register write meets a condition (a NaN,
//! an infinity, a denormal, or any value of one register at one
//! instruction), with the value written, the values the instruction read
//! and the invocation's id, in a storage buffer the runtime or a replay
//! reads back.
//!
//! The record is written without branches, so the program's control flow
//! and results stay as they are: each checked write adds the condition (0
//! or 1) to the instruction's hit count and to the buffer's sequence
//! counter with atomics, and stores its record to the instruction's slot
//! when it was the first hit, or else to a trash slot past the last
//! instruction. The first producer in program order is the lowest PC with
//! hits; in time, the lowest sequence number.
//!
//! Buffer layout (32-bit words): a header of [`TRACE_HEADER_WORDS`]
//! (word 0 the sequence counter), then one slot of [`TRACE_SLOT_WORDS`] per
//! instruction index (PC / 16) plus the trash slot. A slot holds the hit
//! count, the sequence number of the first hit, the PC, the destination
//! (register number, or `0x100 + n` for a uniform register), the value
//! written, three words of invocation id, and four (source, value) pairs:
//! a source is a register number, `0x100 + n` for a uniform register,
//! `0x8000_0000 | bank << 16 | offset` for a constant bank word, or
//! `0xffff_ffff` for none.
//!
//! Not traced: programs on the software warp path and register writes
//! made inside helper functions (none today).

use super::{Ctx, Res, Unsupported, bad};
use crate::spirv::op;

/// Binding, in [`GUEST_MEMORY_SET`](super::GUEST_MEMORY_SET), of the trace
/// buffer.
pub const TRACE_BINDING: u32 = 7;
/// Words before the first slot.
pub const TRACE_HEADER_WORDS: u32 = 16;
/// Words per slot.
pub const TRACE_SLOT_WORDS: u32 = 16;
/// Source pairs per slot.
const SOURCES: usize = 4;
/// Words of each stage's region of the trace buffer (see
/// [`Trace::stage_base`]): a program of up to 16383 instructions.
pub const TRACE_STAGE_WORDS: u32 = TRACE_HEADER_WORDS + TRACE_SLOT_WORDS * 16384;
/// Destination of a [`Trace::miss`] record.
pub const TRACE_MISS: u32 = 0x200;
/// Source id of a constant bank word.
pub const TRACE_SOURCE_CBUF: u32 = 0x8000_0000;
/// Register number offset of uniform registers in sources and destinations.
pub const TRACE_UREG: u32 = 0x100;
/// Destination of a `handle` record: the texture handle word a texture or
/// surface instruction read from a register (one loaded from memory, such as
/// a material table): the texture id in bits 0..20, the sampler id above.
pub const TRACE_HANDLE: u32 = 0x202;

const STORAGE_BUFFER: u32 = 12;
const SCOPE_DEVICE: u32 = 1;
const TRACE_KEY: u32 = 0x7_0000;
const BUILTIN_FRAG_COORD: u32 = 15;
const BUILTIN_VERTEX_INDEX: u32 = 42;
const BUILTIN_INSTANCE_INDEX: u32 = 43;
const BUILTIN_GLOBAL_INVOCATION_ID: u32 = 28;
const INPUT: u32 = 1;

/// What trace mode checks (see [`Trace::parse`]).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Trace {
    pub nan: bool,
    pub inf: bool,
    pub denorm: bool,
    /// Every write (with `pc` and `reg`, one register at one instruction).
    pub any: bool,
    /// A global memory access whose address lies in no pool of the pool
    /// table nor in a storage buffer slot (graphics programs), which goes
    /// through the guest address as a device address: recorded with
    /// destination [`TRACE_MISS`], the address's low word as the value and
    /// its high word as the first source.
    pub miss: bool,
    /// The handle word of every texture or surface read whose handle comes
    /// from a register (recorded with destination [`TRACE_HANDLE`]).
    pub handle: bool,
    /// Only the instruction at this byte address.
    pub pc: Option<u64>,
    /// Only writes of this register (`TRACE_UREG + n` for a uniform one).
    pub reg: Option<u32>,
}

impl Trace {
    /// Parses `<cond>[,<cond>...][@<pc>][/R<n>|/UR<n>]`, where a condition
    /// is `nan`, `inf`, `denorm` or `any`; `@pc` is hex with or without
    /// `0x`. `nan,inf` checks float-producing instructions only;
    /// `any@0x340/R10` watches R10 as the instruction at 0x340 writes it.
    pub fn parse(s: &str) -> Result<Trace, String> {
        let mut t = Trace::default();
        let (rest, reg) = match s.split_once('/') {
            Some((a, r)) => (a, Some(r)),
            None => (s, None),
        };
        let (conds, pc) = match rest.split_once('@') {
            Some((a, p)) => (a, Some(p)),
            None => (rest, None),
        };
        for c in conds.split(',') {
            match c {
                "nan" => t.nan = true,
                "inf" => t.inf = true,
                "denorm" => t.denorm = true,
                "any" => t.any = true,
                "miss" => t.miss = true,
                "handle" => t.handle = true,
                "" => {}
                _ => return Err(format!("trace condition {c:?}")),
            }
        }
        if !(t.nan || t.inf || t.denorm || t.any || t.miss || t.handle) {
            return Err(format!("trace spec {s:?} names no condition"));
        }
        if let Some(p) = pc {
            let h = p.trim_start_matches("0x");
            t.pc = Some(u64::from_str_radix(h, 16).map_err(|e| format!("trace pc {p:?}: {e}"))?);
        }
        if let Some(r) = reg {
            let n = |x: &str| {
                x.parse::<u32>()
                    .map_err(|e| format!("trace register {r:?}: {e}"))
            };
            t.reg = Some(if let Some(x) = r.strip_prefix("UR") {
                TRACE_UREG + n(x)?
            } else if let Some(x) = r.strip_prefix('R') {
                n(x)?
            } else {
                return Err(format!("trace register {r:?}"));
            });
        }
        Ok(t)
    }

    /// Words of the trace buffer of a program of `insts` instructions.
    pub fn words(insts: usize) -> u32 {
        TRACE_HEADER_WORDS + TRACE_SLOT_WORDS * (insts as u32 + 1)
    }

    /// The word where stage `stage`'s region of the trace buffer starts:
    /// every stage of a draw has its own [`TRACE_STAGE_WORDS`] (vertex 0,
    /// fragment 1, geometry 2, tessellation control 3 and evaluation 4,
    /// compute 5, task 6, mesh 7, the runtime's stage numbers).
    pub fn stage_base(stage: super::Stage) -> u32 {
        use super::Stage;
        let k = match stage {
            Stage::Vertex => 0,
            Stage::Fragment => 1,
            Stage::Geometry => 2,
            Stage::TessControl => 3,
            Stage::TessEval => 4,
            Stage::Compute => 5,
            Stage::Task => 6,
            Stage::Mesh => 7,
        };
        k * TRACE_STAGE_WORDS
    }
}

/// Whether an instruction makes float values (the NaN, infinity and
/// denormal checks apply to it): arithmetic, conversions to float, input
/// interpolation, loads and texture fetches.
fn makes_floats(m: &str) -> bool {
    matches!(
        m,
        "FADD"
            | "FADD32I"
            | "FMUL"
            | "FMUL32I"
            | "FFMA"
            | "FFMA32I"
            | "FMNMX"
            | "FSEL"
            | "FSWZADD"
            | "FRND"
            | "F2F"
            | "I2F"
            | "I2FP"
            | "MUFU"
            | "IPA"
            | "ALD"
            | "LDG"
            | "LDS"
            | "LDL"
            | "LDC"
            | "LD"
            | "ULDC"
            | "TEX"
            | "TLD"
            | "TLD4"
            | "TXD"
            | "SULD"
    )
}

/// Instructions that read a texture or surface through a handle.
fn reads_images(m: &str) -> bool {
    matches!(
        m,
        "TEX" | "TLD" | "TLD4" | "TXD" | "TMML" | "TXQ" | "SULD" | "SUST" | "SUATOM" | "SURED"
    )
}

/// Per-translation trace state.
#[derive(Default)]
pub(crate) struct State {
    pub spec: Option<Trace>,
    /// Slots before the trash slot (instructions).
    pub slots: u32,
    var: Option<u32>,
    /// The instruction being translated is checked.
    on: bool,
    pc: u64,
    /// Sources the instruction read so far: (id, value, block).
    srcs: Vec<(u32, u32, u32)>,
}

impl Ctx {
    /// Starts instruction `mnemonic` at `pc`.
    pub(crate) fn trace_begin(&mut self, mnemonic: &str, pc: u64) {
        let on = match &self.trace.spec {
            Some(t) if self.simt.is_none() => {
                t.pc.is_none_or(|p| p == pc)
                    && (t.any
                        || t.miss
                        || makes_floats(mnemonic)
                        || (t.handle && reads_images(mnemonic)))
            }
            _ => false,
        };
        self.trace.on = on;
        self.trace.pc = pc;
        self.trace.srcs.clear();
    }

    /// Notes a value the instruction read (see the module's source ids).
    pub(crate) fn trace_source(&mut self, id: u32, val: u32) {
        if self.trace.on && self.trace.srcs.len() < 16 {
            let b = self.m.block();
            self.trace.srcs.push((id, val, b));
        }
    }

    fn trace_var(&mut self) -> u32 {
        if let Some(v) = self.trace.var {
            return v;
        }
        let u = self.u32t();
        let arr = self.m.id();
        crate::spirv::inst(&mut self.m.types, op::TYPE_RUNTIME_ARRAY, &[arr, u]);
        self.m.deco(arr, 6, &[4]);
        let st = self.m.t_struct_unique(&[arr]);
        self.m.deco(st, 2, &[]);
        self.m.member_deco(st, 0, 35, &[0]);
        let p = self.m.t_ptr(STORAGE_BUFFER, st);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, 34, &[super::GUEST_MEMORY_SET]);
        self.m.deco(var, 33, &[TRACE_BINDING]);
        self.globals.push(var);
        self.builtins.insert(TRACE_KEY, var);
        self.trace.var = Some(var);
        self.iface.trace_words =
            Trace::stage_base(self.stage) + Trace::words(self.trace.slots as usize);
        var
    }

    /// Three words naming the invocation: the global invocation id, the
    /// vertex and instance index, or the fragment's window position.
    fn trace_invocation(&mut self) -> [u32; 3] {
        let u = self.u32t();
        let zero = self.m.c_u32(0);
        match self.stage {
            super::Stage::Compute => {
                let v3 = self.m.t_vec(u, 3);
                let var = self.builtin(BUILTIN_GLOBAL_INVOCATION_ID, INPUT, v3);
                let v = self.m.emit(op::LOAD, v3, &[var]);
                [0, 1, 2].map(|k| self.m.emit(op::COMPOSITE_EXTRACT, u, &[v, k]))
            }
            super::Stage::Vertex => {
                let i = self.m.t_i32();
                let vv = self.builtin(BUILTIN_VERTEX_INDEX, INPUT, i);
                let iv = self.builtin(BUILTIN_INSTANCE_INDEX, INPUT, i);
                let a = self.m.emit(op::LOAD, i, &[vv]);
                let b = self.m.emit(op::LOAD, i, &[iv]);
                let a = self.m.emit(op::BITCAST, u, &[a]);
                let b = self.m.emit(op::BITCAST, u, &[b]);
                [a, b, zero]
            }
            super::Stage::Fragment => {
                let f = self.f32t();
                let v4 = self.m.t_vec(f, 4);
                let var = self.builtin(BUILTIN_FRAG_COORD, INPUT, v4);
                let v = self.m.emit(op::LOAD, v4, &[var]);
                let x = self.m.emit(op::COMPOSITE_EXTRACT, f, &[v, 0]);
                let y = self.m.emit(op::COMPOSITE_EXTRACT, f, &[v, 1]);
                let x = self.m.emit(op::CONVERT_F_TO_U, u, &[x]);
                let y = self.m.emit(op::CONVERT_F_TO_U, u, &[y]);
                [x, y, zero]
            }
            _ => [zero, zero, zero],
        }
    }

    /// After a write of `val` to register `reg` (see the module's
    /// destination numbers) under the instruction's store guard: the
    /// check and the record.
    pub(crate) fn trace_write(&mut self, reg: u32, val: u32) {
        if !self.trace.on || self.collecting() {
            return;
        }
        let Some(t) = self.trace.spec.clone() else {
            return;
        };
        if t.reg.is_some_and(|r| r != reg) {
            return;
        }
        // A handle record is written only under `handle`; with `handle` as the only condition, nothing else is.
        let only_handles = t.handle && !(t.any || t.nan || t.inf || t.denorm);
        if (reg == TRACE_HANDLE && !t.handle) || (reg != TRACE_HANDLE && only_handles) {
            return;
        }
        let u = self.u32t();
        let b = self.boolt();
        let mut cond = if t.any || reg == TRACE_HANDLE {
            Some(self.m.c_bool(true))
        } else {
            None
        };
        let mut or = |c: &mut Ctx, x: u32| {
            cond = Some(match cond {
                Some(y) => c.m.emit(op::LOGICAL_OR, b, &[y, x]),
                None => x,
            });
        };
        if !t.any && reg != TRACE_HANDLE {
            let abs_mask = self.m.c_u32(0x7fff_ffff);
            let inf = self.m.c_u32(0x7f80_0000);
            let abs = self.m.emit(op::BITWISE_AND, u, &[val, abs_mask]);
            if t.nan {
                let x = self.m.emit(op::U_GREATER_THAN, b, &[abs, inf]);
                or(self, x);
            }
            if t.inf {
                let x = self.m.emit(op::I_EQUAL, b, &[abs, inf]);
                or(self, x);
            }
            if t.denorm {
                let zero = self.m.c_u32(0);
                let ex = self.m.emit(op::BITWISE_AND, u, &[val, inf]);
                let ez = self.m.emit(op::I_EQUAL, b, &[ex, zero]);
                let nz = self.m.emit(op::I_NOT_EQUAL, b, &[abs, zero]);
                let x = self.m.emit(op::LOGICAL_AND, b, &[ez, nz]);
                or(self, x);
            }
        }
        let Some(mut cond) = cond else {
            return; // only `miss`: no register check
        };
        if let Some(g) = self.store_guard {
            cond = self.m.emit(op::LOGICAL_AND, b, &[cond, g]);
        }
        let block = self.m.block();
        let srcs: Vec<(u32, u32)> = self
            .trace
            .srcs
            .iter()
            .filter(|s| s.2 == block)
            .map(|s| (s.0, s.1))
            .take(SOURCES)
            .collect();
        self.trace_record(cond, reg, val, &srcs);
    }

    /// A global access of `ty` at the address `addr` (a 64-bit value) that
    /// goes to the device without a storage buffer route: a
    /// [`Trace::miss`] record when the pool table holds no piece with the
    /// address. Every such access is checked, whatever the path that
    /// chose it: a graphics access whose slot and pool searches failed, a
    /// compute access (no search outside trace mode), a routed access its
    /// slot cannot take and a size the search does not handle.
    pub(crate) fn trace_raw_access(&mut self, addr: u32, ty: u32) {
        if !self.trace.on
            || self.collecting()
            || !self.host.bda
            || !self.trace.spec.as_ref().is_some_and(|t| t.miss)
        {
            return;
        }
        let bytes = self.access_bytes(ty);
        if self.route.is_some() && bytes.is_power_of_two() {
            return;
        }
        let (found, _, _) = self.pool_search(addr);
        let b = self.boolt();
        let missed = self.m.emit(op::LOGICAL_NOT, b, &[found]);
        let (lo, hi) = self.addr_words(addr);
        self.trace_record(missed, TRACE_MISS, lo, &[(TRACE_MISS + 1, hi)]);
    }

    /// The record of the instruction being translated when `cond` holds: its
    /// destination id `dst`, the value and up to four (source, value) pairs.
    fn trace_record(&mut self, cond: u32, dst: u32, val: u32, srcs: &[(u32, u32)]) {
        let u = self.u32t();
        let b = self.boolt();
        let var = self.trace_var();
        let pu = self.m.t_ptr(STORAGE_BUFFER, u);
        let zero = self.m.c_u32(0);
        let scope = self.m.c_u32(SCOPE_DEVICE);
        let sem = self.m.c_u32(0);
        let k = (self.trace.pc / 16) as u32;
        let k = if k < self.trace.slots {
            k
        } else {
            self.trace.slots
        };
        // Only an invocation that meets the condition touches the buffer, and only the first hit of an instruction
        // takes a sequence number and writes the record: a device-scope atomic on one shared word for every
        // invocation of every traced instruction (and every miss storing a record to the trash slot) made a large
        // fragment program run long enough for the host's watchdog to lose the device.
        let inv = self.trace_invocation();
        let none = self.m.c_u32(u32::MAX);
        let pc = self.m.c_u32(self.trace.pc as u32);
        let dst = self.m.c_u32(dst);
        let one = self.m.c_u32(1);
        let region = Trace::stage_base(self.stage);
        let hits_at = self
            .m
            .c_u32(region + TRACE_HEADER_WORDS + TRACE_SLOT_WORDS * k);
        let seq_at = self.m.c_u32(region);
        let (hit, first_l, merge_first, merge_hit) =
            (self.m.id(), self.m.id(), self.m.id(), self.m.id());
        self.m.emit_void(op::SELECTION_MERGE, &[merge_hit, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[cond, hit, merge_hit]);
        self.m.emit_void(op::LABEL, &[hit]);
        let hits = self.m.emit(op::ACCESS_CHAIN, pu, &[var, zero, hits_at]);
        let old = self.m.emit(ATOMIC_I_ADD, u, &[hits, scope, sem, one]);
        let was_first = self.m.emit(op::I_EQUAL, b, &[old, zero]);
        self.m.emit_void(op::SELECTION_MERGE, &[merge_first, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[was_first, first_l, merge_first]);
        self.m.emit_void(op::LABEL, &[first_l]);
        let seq_p = self.m.emit(op::ACCESS_CHAIN, pu, &[var, zero, seq_at]);
        let seq = self.m.emit(ATOMIC_I_ADD, u, &[seq_p, scope, sem, one]);
        let mut words = vec![seq, pc, dst, val, inv[0], inv[1], inv[2]];
        for n in 0..SOURCES {
            match srcs.get(n) {
                Some(&(id, v)) => {
                    let id = self.m.c_u32(id);
                    words.extend([id, v]);
                }
                None => words.extend([none, zero]),
            }
        }
        let base = region + TRACE_HEADER_WORDS + TRACE_SLOT_WORDS * k;
        for (n, w) in words.into_iter().enumerate() {
            let at = self.m.c_u32(base + 1 + n as u32);
            let p = self.m.emit(op::ACCESS_CHAIN, pu, &[var, zero, at]);
            self.m.emit_void(op::STORE, &[p, w]);
        }
        self.m.emit_void(op::BRANCH, &[merge_first]);
        self.m.emit_void(op::LABEL, &[merge_first]);
        self.m.emit_void(op::BRANCH, &[merge_hit]);
        self.m.emit_void(op::LABEL, &[merge_hit]);
    }
}

const ATOMIC_I_ADD: u16 = 234;

/// Checks a trace spec against a program: the instruction `@pc` names
/// must exist.
pub(crate) fn check(t: &Trace, insts: usize) -> Res<()> {
    if let Some(pc) = t.pc
        && (pc % 16 != 0 || pc / 16 >= insts as u64)
    {
        return bad(format!("trace pc {pc:#x} is no instruction of the program"));
    }
    Ok::<(), Unsupported>(())
}
