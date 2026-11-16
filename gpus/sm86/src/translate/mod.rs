//! SASS to SPIR-V.
//!
//! Registers, predicates, uniform registers and uniform predicates are
//! function variables; control flow is rebuilt into structured SPIR-V by
//! [`cfg`]. Each opcode family lives in its own module, and every module
//! answers `None` for opcodes it does not know, so [`Ctx::inst`] asks them in
//! turn.

mod calls;
mod cfg;
mod denorm;
mod float;
mod fma;
mod fp64;
mod host;
mod int;
mod io;
mod mem;
mod mma;
mod mufu;
mod simt;
mod tex;
mod uniform;

pub use host::{
    Atomic64Path, CLOCK_SEED_OFFSET, Clock, ClockPath, Denorm, GUEST_MEMORY_SET, GlobalPath,
    HostProfile, HostReport, LOCK_BINDING, LOCK_WORDS, SEGMENT_COUNT_DEFAULT, SEGMENT_COUNT_SPEC,
    SEGMENT_SHIFT_DEFAULT, SEGMENT_SHIFT_SPEC, Warp, WarpPath,
};
pub use io::vtg::{GsInput, Pipeline, TESS_LEVEL_SPEC, TessDomain, TessOutput, TessSpacing};
pub use tex::{
    Dim, ImageDim, ImageKind, Kind, SAMPLER_BINDING, TEXTURE_KIND_SPEC, image_binding,
    texture_binding,
};

use crate::program::{Program, Stage};
use crate::spirv::{Module, op};
use crate::{CBank, Inst, Operand, RegMods, Word};
use std::collections::{BTreeMap, BTreeSet};

pub(crate) const UNIFORM: u32 = 2;
pub(crate) const INPUT: u32 = 1;
pub(crate) const OUTPUT: u32 = 3;
pub(crate) const FUNCTION: u32 = 7;
pub(crate) const PHYSICAL_STORAGE_BUFFER: u32 = 5349;
const CBUF_VEC4S: u32 = 4096;
const EXECUTION_MODE_MAXIMALLY_RECONVERGES: u32 = 6023;

/// Why a program could not be translated: the first construct the
/// translator does not handle.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unsupported(pub String);

impl std::fmt::Display for Unsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Unsupported {}

pub(crate) type Res<T> = Result<T, Unsupported>;

pub(crate) fn bad<T>(s: impl Into<String>) -> Res<T> {
    Err(Unsupported(s.into()))
}

/// Interpolation of a fragment input.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Interp {
    Perspective,
    Flat,
    Linear,
}

/// What a translated shader reads and writes, for the pipeline layout.
#[derive(Clone, Debug, Default)]
pub struct Interface {
    pub inputs: BTreeMap<u32, Option<Interp>>,
    pub outputs: BTreeSet<u32>,
    /// Constant banks read, by bank number (see [`cbuf_binding`]).
    pub cbufs: BTreeSet<u32>,
    pub color_targets: BTreeSet<u32>,
    /// The program dereferences 64-bit global addresses (buffer device
    /// addresses).
    pub global_memory: bool,
    /// Bytes of shared memory addressed (compute), rounded up to a word.
    pub shared_bytes: u32,
    /// Bytes of local (per-thread) memory addressed.
    pub local_bytes: u32,
    /// Texture handle reads: (bank, byte offset) of each bound handle.
    pub textures: BTreeSet<(u32, u32)>,
    /// Bindings of set [`TEXTURE_SET`] the program declares.
    pub texture_bindings: BTreeSet<u32>,
    /// Specialization constant id per bound handle whose texture's numeric
    /// kind the runtime picks (see [`Kind`]).
    pub texture_kinds: BTreeMap<(u32, u32), u32>,
    /// SPIR-V BuiltIn numbers of the built-in inputs and outputs used.
    pub builtins: BTreeSet<u32>,
    /// Length of the ClipDistance output (the highest clip distance the SPH
    /// output map names, plus one); the guest's pipeline state says which of
    /// them clip and which cull.
    pub clip_distances: u32,
    /// The fragment shader writes depth (SPH output map).
    pub writes_depth: bool,
    /// The fragment shader writes the sample mask (SPH output map).
    pub writes_sample_mask: bool,
    /// The fragment shader kills invocations (OpDemoteToHelperInvocation:
    /// the device needs shaderDemoteToHelperInvocation).
    pub kills: bool,
    /// The compute program takes implicit-LOD derivatives across quads of
    /// four consecutive invocations (DerivativeGroupLinearKHR: the device
    /// needs computeDerivativeGroupLinear and a workgroup size that is a
    /// multiple of four).
    pub compute_derivatives: bool,
    /// Patch locations read (tessellation evaluation) or written
    /// (tessellation control).
    pub patch_inputs: BTreeSet<u32>,
    pub patch_outputs: BTreeSet<u32>,
    /// Output vertices of a tessellation control program, or the most a
    /// geometry program emits.
    pub output_vertices: u32,
    /// The fallbacks taken for the target host.
    pub host: HostReport,
}

/// Translation options.
#[derive(Clone, Debug, Default)]
pub struct Options {
    /// The device the program is translated for.
    pub host: HostProfile,
    /// Build MUFU RCP, RSQ, SQRT, EX2 and LG2 from the integer model of the
    /// SM86 unit instead of the GLSL operations, which only the NVIDIA driver
    /// lowers to the same results (see the `mufu` module). Hosts other
    /// than NVIDIA always use the model.
    pub exact_mufu: bool,
    /// Numeric kind of the format of each fragment render target the
    /// pipeline binds (a target not named is float); the output variable of
    /// a target has that type (see the `io` module).
    pub color_kinds: BTreeMap<u32, Kind>,
    /// Pipeline state of the tessellation and geometry stages.
    pub pipeline: Pipeline,
}

/// A translated program.
pub struct Shader {
    pub stage: Stage,
    pub spirv: Vec<u32>,
    pub interface: Interface,
}

/// First constant bank the application owns; banks below it are the
/// driver's.
pub const FIRST_USER_CBUF: u32 = 3;

/// Descriptor set of user constant banks (`c[3 + n]` is binding `n`).
pub const USER_CBUF_SET: u32 = 0;
/// Descriptor set of the driver's constant banks (`c[n]`, n < 3, is
/// binding `n`).
pub const DRIVER_CBUF_SET: u32 = 1;
/// Descriptor set of the texture and sampler pools.
pub const TEXTURE_SET: u32 = 2;

/// (descriptor set, binding) of constant bank `bank`.
pub fn cbuf_binding(bank: u32) -> (u32, u32) {
    match bank.checked_sub(FIRST_USER_CBUF) {
        Some(n) => (USER_CBUF_SET, n),
        None => (DRIVER_CBUF_SET, bank),
    }
}

/// The program after decoding: instructions with their words.
pub(crate) struct Code {
    pub insts: Vec<Inst>,
    pub words: Vec<Word>,
    /// BRA.DIV and BRA.CONV folded to their fast paths.
    pub fast_paths: usize,
}

pub(crate) struct Ctx {
    pub m: Module,
    pub glsl: u32,
    pub stage: Stage,
    pub regs: BTreeMap<u8, u32>,
    pub preds: BTreeMap<u8, u32>,
    pub uregs: BTreeMap<u8, u32>,
    pub upreds: BTreeMap<u8, u32>,
    pub inputs: BTreeMap<u32, u32>,
    pub outputs: BTreeMap<u32, u32>,
    pub cbufs: BTreeMap<u32, u32>,
    pub cbuf_block: Option<u32>,
    pub builtins: BTreeMap<u32, u32>,
    pub colors: BTreeMap<u32, u32>,
    pub globals: Vec<u32>,
    pub caps: BTreeSet<u32>,
    pub exts: BTreeSet<&'static str>,
    pub iface: Interface,
    pub word: Word,
    pub pc: u64,
    pub sph_words: [u32; 32],
    pub tex: tex::Prep,
    pub io: io::State,
    /// Function-scope `u32` variables beyond the registers, zeroed at entry.
    pub locals: Vec<u32>,
    pub exact_mufu: bool,
    pub host: HostProfile,
    pub denorm: denorm::State,
    /// Helper functions, emitted after `main`.
    pub funcs: Vec<u32>,
    /// The last clock value read, low and high words.
    pub clock_vars: Option<(u32, u32)>,
    /// Per instruction, whether control flow may reach it with only part
    /// of the warp (see `mem::lanes::divergent`).
    pub divergent: Vec<bool>,
    /// Which global accesses must be coherent (see `mem::order`).
    pub mem: mem::order::Plan,
    /// The warp emulation state on [`WarpPath::Simt`].
    pub simt: Option<simt::State>,
    /// Instructions (by index) whose warp operation gives the same result
    /// in any warp (see [`uniform`]), translated per lane.
    pub invariant: BTreeSet<usize>,
}

impl Ctx {
    pub fn u32t(&mut self) -> u32 {
        self.m.t_u32()
    }

    pub fn f32t(&mut self) -> u32 {
        self.m.t_f32()
    }

    pub fn boolt(&mut self) -> u32 {
        self.m.t_bool()
    }

    pub fn cap(&mut self, c: u32) {
        self.caps.insert(c);
    }

    fn var_in(map: &mut BTreeMap<u8, u32>, m: &mut Module, n: u8) -> u32 {
        *map.entry(n).or_insert_with(|| m.id())
    }

    pub fn load_reg(&mut self, n: u8) -> u32 {
        if n == 255 {
            return self.m.c_u32(0);
        }
        let v = Ctx::var_in(&mut self.regs, &mut self.m, n);
        let t = self.u32t();
        let v = self.lane_var(v, t);
        self.m.emit(op::LOAD, t, &[v])
    }

    pub fn store_reg(&mut self, n: u8, val: u32) {
        if n == 255 || self.collecting() {
            return;
        }
        let v = Ctx::var_in(&mut self.regs, &mut self.m, n);
        let t = self.u32t();
        let v = self.lane_var(v, t);
        self.m.emit_void(op::STORE, &[v, val]);
    }

    /// The variable itself, or on [`WarpPath::Simt`] the current lane's
    /// entry of it.
    fn lane_var(&mut self, var: u32, ty: u32) -> u32 {
        if self.simt.is_some() {
            self.lane_ptr(var, ty)
        } else {
            var
        }
    }

    /// The first pass of a warp operation on [`WarpPath::Simt`], which
    /// writes nothing.
    fn collecting(&self) -> bool {
        self.simt_pass() == Some(simt::Pass::Collect)
    }

    pub fn load_ureg(&mut self, n: u8) -> u32 {
        if n == 63 {
            return self.m.c_u32(0);
        }
        let v = Ctx::var_in(&mut self.uregs, &mut self.m, n);
        let t = self.u32t();
        self.m.emit(op::LOAD, t, &[v])
    }

    pub fn store_ureg(&mut self, n: u8, val: u32) {
        if n == 63 || self.collecting() {
            return;
        }
        let v = Ctx::var_in(&mut self.uregs, &mut self.m, n);
        self.m.emit_void(op::STORE, &[v, val]);
    }

    pub fn load_pred(&mut self, n: u8, not: bool) -> u32 {
        let v = if n == 7 {
            self.m.c_bool(true)
        } else {
            let var = Ctx::var_in(&mut self.preds, &mut self.m, n);
            let t = self.m.t_bool();
            let var = self.lane_var(var, t);
            self.m.emit(op::LOAD, t, &[var])
        };
        self.not_if(v, not)
    }

    pub fn store_pred(&mut self, n: u8, val: u32) {
        if n == 7 || self.collecting() {
            return;
        }
        let v = Ctx::var_in(&mut self.preds, &mut self.m, n);
        let t = self.m.t_bool();
        let v = self.lane_var(v, t);
        self.m.emit_void(op::STORE, &[v, val]);
    }

    pub fn load_upred(&mut self, n: u8, not: bool) -> u32 {
        let v = if n == 7 {
            self.m.c_bool(true)
        } else {
            let var = Ctx::var_in(&mut self.upreds, &mut self.m, n);
            let t = self.m.t_bool();
            self.m.emit(op::LOAD, t, &[var])
        };
        self.not_if(v, not)
    }

    pub fn store_upred(&mut self, n: u8, val: u32) {
        if n == 7 || self.collecting() {
            return;
        }
        let v = Ctx::var_in(&mut self.upreds, &mut self.m, n);
        self.m.emit_void(op::STORE, &[v, val]);
    }

    pub fn not_if(&mut self, v: u32, not: bool) -> u32 {
        if not {
            let t = self.m.t_bool();
            self.m.emit(op::LOGICAL_NOT, t, &[v])
        } else {
            v
        }
    }

    /// A predicate operand (`P`, `UP`, with its negation).
    pub fn pred_operand(&mut self, o: &Operand) -> Res<u32> {
        match o {
            Operand::Pred(n, not) => Ok(self.load_pred(*n, *not)),
            Operand::UPred(n, not) => Ok(self.load_upred(*n, *not)),
            o => bad(format!("predicate operand {o}")),
        }
    }

    pub fn as_float(&mut self, u: u32) -> u32 {
        let t = self.f32t();
        self.m.emit(op::BITCAST, t, &[u])
    }

    pub fn as_bits(&mut self, f: u32) -> u32 {
        let t = self.u32t();
        self.m.emit(op::BITCAST, t, &[f])
    }

    pub fn ext(&mut self, ty: u32, f: u32, args: &[u32]) -> u32 {
        let mut a = vec![self.glsl, f];
        a.extend_from_slice(args);
        self.m.emit(op::EXT_INST, ty, &a)
    }

    pub fn select(&mut self, ty: u32, c: u32, a: u32, b: u32) -> u32 {
        self.m.emit(op::SELECT, ty, &[c, a, b])
    }

    pub fn bool_to_u32(&mut self, b: u32, t: u32) -> u32 {
        let u = self.u32t();
        let one = self.m.c_u32(t);
        let zero = self.m.c_u32(0);
        self.select(u, b, one, zero)
    }

    pub fn cbuf_var(&mut self, bank: u32) -> Res<u32> {
        if let Some(&v) = self.cbufs.get(&bank) {
            return Ok(v);
        }
        if bank > 31 {
            return bad(format!("constant bank c[{bank:#x}]"));
        }
        let st = match self.cbuf_block {
            Some(st) => st,
            None => {
                let u = self.u32t();
                let v4 = self.m.t_vec(u, 4);
                let arr = self.m.t_array(v4, CBUF_VEC4S);
                self.m.deco(arr, 6, &[16]);
                let st = self.m.t_struct_unique(&[arr]);
                self.m.deco(st, 2, &[]);
                self.m.member_deco(st, 0, 35, &[0]);
                self.cbuf_block = Some(st);
                st
            }
        };
        let p = self.m.t_ptr(UNIFORM, st);
        let var = self.m.global(p, UNIFORM);
        let (set, binding) = cbuf_binding(bank);
        self.m.deco(var, 34, &[set]);
        self.m.deco(var, 33, &[binding]);
        self.cbufs.insert(bank, var);
        self.globals.push(var);
        self.iface.cbufs.insert(bank);
        Ok(var)
    }

    /// One 32-bit word of constant bank `bank` at constant byte offset `off`,
    /// taken modulo 64 KiB (the decoder prints the 16-bit field signed).
    pub fn load_cbuf(&mut self, bank: u32, off: i64) -> Res<u32> {
        let off = off & 0xffff;
        if off % 4 != 0 {
            return bad(format!("cbuf offset {off:#x}"));
        }
        let var = self.cbuf_var(bank)?;
        let zero = self.m.c_u32(0);
        let vi = self.m.c_u32((off / 16) as u32);
        let ci = self.m.c_u32(((off / 4) % 4) as u32);
        let u = self.u32t();
        let p = self.m.t_ptr(UNIFORM, u);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, zero, vi, ci]);
        Ok(self.m.emit(op::LOAD, u, &[ptr]))
    }

    /// One 32-bit word of constant bank `bank` at a byte address computed at
    /// run time; out-of-range reads give 0 as on the hardware.
    pub fn load_cbuf_dyn(&mut self, bank: u32, addr: u32) -> Res<u32> {
        let var = self.cbuf_var(bank)?;
        let u = self.u32t();
        let b = self.boolt();
        let limit = self.m.c_u32(CBUF_VEC4S * 16);
        let inb = self.m.emit(op::U_LESS_THAN, b, &[addr, limit]);
        let mask = self.m.c_u32(CBUF_VEC4S * 16 - 1);
        let a = self.m.emit(op::BITWISE_AND, u, &[addr, mask]);
        let four = self.m.c_u32(4);
        let two = self.m.c_u32(2);
        let three = self.m.c_u32(3);
        let vi = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[a, four]);
        let wi = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[a, two]);
        let ci = self.m.emit(op::BITWISE_AND, u, &[wi, three]);
        let zero = self.m.c_u32(0);
        let p = self.m.t_ptr(UNIFORM, u);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, zero, vi, ci]);
        let v = self.m.emit(op::LOAD, u, &[ptr]);
        Ok(self.select(u, inb, v, zero))
    }

    /// A constant-bank operand: its bank may be a uniform register and its
    /// offset may add a register.
    pub fn cbuf_operand(&mut self, bank: &CBank, index: Option<&Operand>, offset: i64) -> Res<u32> {
        let b = match bank {
            CBank::Imm(b) => *b,
            CBank::UReg(_) => return bad("constant bank in a uniform register"),
        };
        match index {
            None => self.load_cbuf(b, offset),
            Some(r) => {
                let base = match r {
                    Operand::Reg(n, _) => self.load_reg(*n),
                    Operand::UReg(n, _) => self.load_ureg(*n),
                    o => return bad(format!("cbuf index {o}")),
                };
                let u = self.u32t();
                let k = self.m.c_u32(offset as u32);
                let a = self.m.emit(op::I_ADD, u, &[base, k]);
                self.load_cbuf_dyn(b, a)
            }
        }
    }

    pub fn raw(&mut self, o: &Operand) -> Res<(u32, RegMods)> {
        Ok(match o {
            Operand::Reg(n, m) => (self.load_reg(*n), *m),
            Operand::UReg(n, m) => (self.load_ureg(*n), *m),
            Operand::F32(bits, neg) => {
                let c = self.m.c_u32(*bits);
                (
                    c,
                    RegMods {
                        neg: *neg,
                        ..RegMods::default()
                    },
                )
            }
            Operand::Int(v) => (self.m.c_u32(*v as u32), RegMods::default()),
            Operand::Hex(v) => (self.m.c_u32(*v as u32), RegMods::default()),
            Operand::CBuf {
                bank,
                index,
                offset,
                mods,
                ..
            } => (self.cbuf_operand(bank, index.as_deref(), *offset)?, *mods),
            o => return bad(format!("operand {o}")),
        })
    }

    /// The bits of a float operand with its sign modifiers applied as
    /// integer operations, so that a denormal reaches the instruction
    /// unchanged on a host that flushes float values.
    pub fn fsrc_bits(&mut self, o: &Operand) -> Res<u32> {
        let (u, m) = self.raw(o)?;
        if m.not || m.swz != crate::Swz::None {
            return bad(format!("float operand mods {o}"));
        }
        let ut = self.u32t();
        let mut u = u;
        if m.abs {
            let k = self.m.c_u32(0x7fff_ffff);
            u = self.m.emit(op::BITWISE_AND, ut, &[u, k]);
        }
        if m.neg {
            let k = self.m.c_u32(0x8000_0000);
            u = self.m.emit(op::BITWISE_XOR, ut, &[u, k]);
        }
        Ok(u)
    }

    pub fn usrc(&mut self, o: &Operand) -> Res<u32> {
        let (u, m) = self.raw(o)?;
        if m.neg || m.abs || m.not || m.swz != crate::Swz::None {
            return bad(format!("int operand mods {o}"));
        }
        Ok(u)
    }

    pub fn dst_reg(o: &Operand) -> Res<u8> {
        match o {
            Operand::Reg(n, _) => Ok(*n),
            o => bad(format!("dst {o}")),
        }
    }

    /// Writes `val` to a register or uniform register destination.
    pub fn store_dst(&mut self, o: &Operand, val: u32) -> Res<()> {
        match o {
            Operand::Reg(n, _) => self.store_reg(*n, val),
            Operand::UReg(n, _) => self.store_ureg(*n, val),
            o => return bad(format!("dst {o}")),
        }
        Ok(())
    }

    /// Writes a predicate or uniform predicate destination.
    pub fn store_pdst(&mut self, o: &Operand, val: u32) -> Res<()> {
        match o {
            Operand::Pred(n, false) => self.store_pred(*n, val),
            Operand::UPred(n, false) => self.store_upred(*n, val),
            o => return bad(format!("predicate dst {o}")),
        }
        Ok(())
    }

    pub fn mods_ok(i: &Inst, allowed: &[&str]) -> Res<()> {
        for m in &i.mods {
            if !allowed.contains(m) {
                return bad(format!("{} modifier {m}", i.mnemonic));
            }
        }
        Ok(())
    }

    /// A built-in variable of `class` with `ty` pointee, made once.
    pub fn builtin(&mut self, b: u32, class: u32, ty: u32) -> u32 {
        if let Some(&v) = self.builtins.get(&b) {
            return v;
        }
        let p = self.m.t_ptr(class, ty);
        let var = self.m.global(p, class);
        self.m.deco(var, 11, &[b]);
        if class == INPUT && self.stage == Stage::Fragment && ty != self.m.t_f32() {
            let fv4 = self.m.t_f32();
            let v4 = self.m.t_vec(fv4, 4);
            if ty != v4 {
                self.m.deco(var, 14, &[]);
            }
        }
        self.builtins.insert(b, var);
        self.globals.push(var);
        var
    }

    fn inst(&mut self, i: &Inst) -> Res<()> {
        match i.mnemonic {
            "NOP" | "BSSY" | "BSYNC" | "BREAK" | "DEPBAR" | "YIELD" => {
                return Ok(());
            }
            "LEPC" => {
                let n = Self::dst_reg(&i.ops[0])?;
                if n != 255 {
                    let lo = self.m.c_u32(self.pc as u32);
                    let hi = self.m.c_u32((self.pc >> 32) as u32);
                    self.store_reg(n, lo);
                    self.store_reg(n + 1, hi);
                }
                return Ok(());
            }
            _ => {}
        }
        for f in [
            mem::inst,
            fp64::inst,
            float::inst,
            int::inst,
            io::inst,
            tex::inst,
            mma::inst,
        ] {
            if let Some(r) = f(self, i) {
                return r;
            }
        }
        bad(format!("opcode {}", i.mnemonic))
    }

    /// One instruction under its guard predicate.
    pub(crate) fn guarded(&mut self, i: &Inst, w: Word, pc: u64) -> Res<()> {
        self.word = w;
        self.pc = pc;
        match self.simt_pass() {
            Some(simt::Pass::Collect) => {
                let g = match &i.guard {
                    Some(g) => Some(self.pred_operand(g)?),
                    None => None,
                };
                self.simt_collect_guard(g);
                return self.inst(i);
            }
            Some(simt::Pass::Replay) => return self.inst(i),
            _ => {}
        }
        let Some(g) = &i.guard else {
            return self.inst(i);
        };
        let c = self.pred_operand(g)?;
        let then = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[c, then, merge]);
        self.m.emit_void(op::LABEL, &[then]);
        self.inst(i)?;
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        Ok(())
    }

    /// What runs at every EXIT before the return.
    pub(crate) fn epilogue(&mut self) {
        if self.stage == Stage::Fragment {
            let targets = self.sph_words[18];
            self.fragment_outputs(targets);
        }
    }
}

/// BRA.DIV and BRA.CONV pick between a fast path that needs the warp
/// converged and a slow path that makes it so before calling a helper.
/// Subgroup operations are already defined for any set of active lanes,
/// so the fast path is taken: BRA.DIV never branches and BRA.CONV always
/// does. That keeps the guest's lanes together only where the host
/// reconverges as the guest does, so a program with a fast path asks for
/// maximal reconvergence (see `warp_plan`).
fn converged(mut i: Inst) -> Inst {
    if i.mnemonic != "BRA" {
        return i;
    }
    if i.has("DIV") {
        return Inst::new("NOP");
    }
    if i.has("CONV") {
        i.mods.retain(|m| *m != "CONV");
        i.ops.retain(|o| matches!(o, Operand::Target(_)));
    }
    i
}

fn decode_all(p: &Program) -> Res<Code> {
    let mut insts = Vec::new();
    let mut fast_paths = 0;
    for (n, d) in p.decode().into_iter().enumerate() {
        let i = d.map_err(|e| Unsupported(format!("decode at {:#x}: {e}", 16 * n)))?;
        if i.mnemonic == "BRA" && (i.has("DIV") || i.has("CONV")) {
            fast_paths += 1;
        }
        insts.push(converged(i));
    }
    Ok(Code {
        insts,
        words: p.words.clone(),
        fast_paths,
    })
}

/// Translates one program to a SPIR-V module with one entry point `main`,
/// for the default host.
pub fn translate(p: &Program) -> Res<Shader> {
    translate_with(p, &Options::default())
}

/// Translates one program for the device `o.host`.
pub fn translate_with(p: &Program, o: &Options) -> Res<Shader> {
    match translate_once(p, o, false) {
        Err(Unsupported(e)) if e == cfg::TOO_MUCH_DUPLICATION => translate_once(p, o, true),
        r => r,
    }
}

/// One translation; `split` dispatches at shared joins instead of
/// copying them (see [`cfg`]).
fn translate_once(p: &Program, o: &Options, split: bool) -> Res<Shader> {
    let stage = p.stage();
    let code = decode_all(p)?;
    let mut m = Module::new();
    let glsl_id = m.id();
    let mut c = Ctx {
        m,
        glsl: glsl_id,
        stage,
        regs: BTreeMap::new(),
        preds: BTreeMap::new(),
        uregs: BTreeMap::new(),
        upreds: BTreeMap::new(),
        inputs: BTreeMap::new(),
        outputs: BTreeMap::new(),
        cbufs: BTreeMap::new(),
        cbuf_block: None,
        builtins: BTreeMap::new(),
        colors: BTreeMap::new(),
        globals: Vec::new(),
        caps: BTreeSet::from([1]),
        exts: BTreeSet::new(),
        iface: Interface::default(),
        word: Word(0),
        pc: 0,
        sph_words: p.sph.as_ref().map_or([0; 32], |s| s.words),
        tex: tex::Prep::new(&code.insts, &code.words),
        io: io::State::default(),
        locals: Vec::new(),
        exact_mufu: o.exact_mufu || !o.host.native_mufu(),
        host: o.host.clone(),
        denorm: denorm::State::default(),
        funcs: Vec::new(),
        clock_vars: None,
        divergent: Vec::new(),
        mem: mem::order::Plan::default(),
        simt: None,
        invariant: BTreeSet::new(),
    };
    c.iface.host.host = o.host.name;
    c.io.color_kinds = o.color_kinds.clone();
    c.io.vtg.pipe = o.pipeline.clone();
    io::vtg::prescan(&mut c, &code.insts)?;
    c.denorm_plan(&code.insts);
    c.warp_plan(&code);
    c.mem_plan(&code.insts);
    let void = c.m.t_void();
    let fnt = c.m.t_fn(void);
    let main = c.m.id();
    c.m.emit_void(op::FUNCTION, &[void, main, 0, fnt]);
    let entry = c.m.id();
    c.m.emit_void(op::LABEL, &[entry]);
    let var_at = c.m.body.len();
    c.tcs_prologue()?;
    let body = c.m.id();
    c.m.emit_void(op::BRANCH, &[body]);
    if c.iface.host.warp == WarpPath::Simt {
        simt::emit(&mut c, &code, body, split)?;
    } else {
        cfg::emit(&mut c, &code, body, split)?;
    }
    c.m.emit_void(op::FUNCTION_END, &[]);
    let funcs = std::mem::take(&mut c.funcs);
    c.m.body.extend(funcs);
    finish(c, main, entry, var_at)
}

fn finish(mut c: Ctx, main: u32, _entry: u32, var_at: usize) -> Res<Shader> {
    let stage = c.stage;
    let u = c.m.t_u32();
    let pu = c.m.t_ptr(FUNCTION, u);
    let b = c.m.t_bool();
    let pb = c.m.t_ptr(FUNCTION, b);
    let mut decls = Vec::new();
    let ints: Vec<u32> = c
        .regs
        .values()
        .chain(c.uregs.values())
        .chain(c.locals.iter())
        .copied()
        .collect();
    let bools: Vec<u32> = c.preds.values().chain(c.upreds.values()).copied().collect();
    let zero = c.m.c_u32(0);
    let fals = c.m.c_bool(false);
    let mut vars: Vec<(u32, u32, u32)> = Vec::new();
    if let Some(st) = &c.simt {
        let lane_words: Vec<u32> = c.regs.values().chain(st.words.iter()).copied().collect();
        let lane_bools: Vec<u32> = c.preds.values().copied().collect();
        let au = c.m.t_array(u, 32);
        let ab = c.m.t_array(b, 32);
        let (pau, pab) = (c.m.t_ptr(FUNCTION, au), c.m.t_ptr(FUNCTION, ab));
        let (nu, nb) = (c.m.c_null(au), c.m.c_null(ab));
        vars.extend(lane_words.iter().map(|&v| (pau, v, nu)));
        vars.extend(lane_bools.iter().map(|&v| (pab, v, nb)));
        vars.extend(
            c.uregs
                .values()
                .chain(c.locals.iter())
                .map(|&v| (pu, v, zero)),
        );
        vars.extend(c.upreds.values().map(|&v| (pb, v, fals)));
    } else {
        vars.extend(ints.iter().map(|&v| (pu, v, zero)));
        vars.extend(bools.iter().map(|&v| (pb, v, fals)));
    }
    for &(t, v, _) in &vars {
        crate::spirv::inst(&mut decls, op::VARIABLE, &[t, v, FUNCTION]);
    }
    for &(_, v, z) in &vars {
        crate::spirv::inst(&mut decls, op::STORE, &[v, z]);
    }
    c.m.body.splice(var_at..var_at, decls);
    io::finish(&mut c, main);
    io::vtg::modes(&mut c, main)?;
    let mut iface: Vec<u32> = Vec::new();
    iface.extend(c.inputs.values());
    iface.extend(c.outputs.values());
    iface.extend(c.colors.values());
    iface.extend(c.globals.iter().copied());
    if c.iface.global_memory {
        c.cap(5347);
        c.exts.insert("SPV_KHR_physical_storage_buffer");
    }
    for &cap in &c.caps {
        crate::spirv::inst(&mut c.m.caps, op::CAPABILITY, &[cap]);
    }
    for e in &c.exts {
        crate::spirv::inst(&mut c.m.caps, op::EXTENSION, &crate::spirv::str_words(e));
    }
    let mut e = vec![c.glsl];
    e.extend(crate::spirv::str_words("GLSL.std.450"));
    crate::spirv::inst(&mut c.m.ext, op::EXT_INST_IMPORT, &e);
    let addressing = if c.iface.global_memory { 5348 } else { 0 };
    crate::spirv::inst(&mut c.m.head, op::MEMORY_MODEL, &[addressing, 1]);
    let model = match stage {
        Stage::Vertex => 0,
        Stage::TessControl => 1,
        Stage::TessEval => 2,
        Stage::Geometry => 3,
        Stage::Fragment => 4,
        Stage::Compute => 5,
    };
    let mut ep = vec![model, main];
    ep.extend(crate::spirv::str_words("main"));
    ep.extend(iface);
    crate::spirv::inst(&mut c.m.head, op::ENTRY_POINT, &ep);
    match stage {
        Stage::Fragment => crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &[main, 7]),
        Stage::Compute => {
            crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &[main, 17, 1, 1, 1]);
            if c.iface.compute_derivatives {
                crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &[main, 5290]);
            }
            let ws = if c.simt.is_some() {
                c.simt_workgroup_size()
            } else {
                c.workgroup_size()
            };
            c.m.deco(ws, 11, &[25]);
        }
        _ => {}
    }
    c.denorm_mode(main);
    if c.iface.host.max_reconvergence {
        crate::spirv::inst(
            &mut c.m.caps,
            op::EXTENSION,
            &crate::spirv::str_words("SPV_KHR_maximal_reconvergence"),
        );
        crate::spirv::inst(
            &mut c.m.modes,
            op::EXECUTION_MODE,
            &[main, EXECUTION_MODE_MAXIMALLY_RECONVERGES],
        );
    }
    let iface = c.iface;
    Ok(Shader {
        stage,
        spirv: c.m.finish(),
        interface: iface,
    })
}
