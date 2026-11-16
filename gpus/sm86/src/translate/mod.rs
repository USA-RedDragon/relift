use crate::program::{Program, Stage};
use crate::spirv::{Module, glsl, op};
use crate::{CBank, Inst, Operand, RegMods};
use std::collections::{BTreeMap, BTreeSet};

const UNIFORM: u32 = 2;
const INPUT: u32 = 1;
const OUTPUT: u32 = 3;
const FUNCTION: u32 = 7;
const CBUF_VEC4S: u32 = 4096;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unsupported(pub String);

impl std::fmt::Display for Unsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Unsupported {}

type Res<T> = Result<T, Unsupported>;

fn bad<T>(s: impl Into<String>) -> Res<T> {
    Err(Unsupported(s.into()))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Interp {
    Perspective,
    Flat,
    Linear,
}

#[derive(Clone, Debug, Default)]
pub struct Interface {
    pub inputs: BTreeMap<u32, Option<Interp>>,
    pub outputs: BTreeSet<u32>,
    pub cbufs: BTreeSet<u32>,
    pub color_targets: BTreeSet<u32>,
}

pub struct Shader {
    pub stage: Stage,
    pub spirv: Vec<u32>,
    pub interface: Interface,
}

pub const FIRST_USER_CBUF: u32 = 3;

pub fn cbuf_binding(bank: u32) -> Option<u32> {
    bank.checked_sub(FIRST_USER_CBUF)
}

struct Ctx {
    m: Module,
    glsl: u32,
    stage: Stage,
    regs: BTreeMap<u8, u32>,
    preds: BTreeMap<u8, u32>,
    inputs: BTreeMap<u32, u32>,
    outputs: BTreeMap<u32, u32>,
    cbufs: BTreeMap<u32, u32>,
    cbuf_block: Option<u32>,
    frag_coord: Option<u32>,
    position: Option<u32>,
    colors: BTreeMap<u32, u32>,
    iface: Interface,
}

impl Ctx {
    fn u32t(&mut self) -> u32 {
        self.m.t_u32()
    }

    fn f32t(&mut self) -> u32 {
        self.m.t_f32()
    }

    fn reg_var(&mut self, n: u8) -> u32 {
        if let Some(&v) = self.regs.get(&n) {
            return v;
        }
        let v = self.m.id();
        self.regs.insert(n, v);
        v
    }

    fn pred_var(&mut self, n: u8) -> u32 {
        if let Some(&v) = self.preds.get(&n) {
            return v;
        }
        let v = self.m.id();
        self.preds.insert(n, v);
        v
    }

    fn load_reg(&mut self, n: u8) -> u32 {
        if n == 255 {
            return self.m.c_u32(0);
        }
        let v = self.reg_var(n);
        let t = self.u32t();
        self.m.emit(op::LOAD, t, &[v])
    }

    fn store_reg(&mut self, n: u8, val: u32) {
        if n == 255 {
            return;
        }
        let v = self.reg_var(n);
        self.m.emit_void(op::STORE, &[v, val]);
    }

    fn as_float(&mut self, u: u32) -> u32 {
        let t = self.f32t();
        self.m.emit(op::BITCAST, t, &[u])
    }

    fn as_bits(&mut self, f: u32) -> u32 {
        let t = self.u32t();
        self.m.emit(op::BITCAST, t, &[f])
    }

    fn ext(&mut self, ty: u32, f: u32, args: &[u32]) -> u32 {
        let mut a = vec![self.glsl, f];
        a.extend_from_slice(args);
        self.m.emit(op::EXT_INST, ty, &a)
    }

    fn load_pred(&mut self, n: u8, not: bool) -> u32 {
        let v = if n == 7 {
            self.m.c_bool(true)
        } else {
            let var = self.pred_var(n);
            let t = self.m.t_bool();
            self.m.emit(op::LOAD, t, &[var])
        };
        if not {
            let t = self.m.t_bool();
            self.m.emit(op::LOGICAL_NOT, t, &[v])
        } else {
            v
        }
    }

    fn cbuf_var(&mut self, bank: u32) -> Res<u32> {
        if let Some(&v) = self.cbufs.get(&bank) {
            return Ok(v);
        }
        let Some(binding) = cbuf_binding(bank) else {
            return bad(format!("driver constant bank c[{bank:#x}]"));
        };
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
        self.m.deco(var, 34, &[0]);
        self.m.deco(var, 33, &[binding]);
        self.cbufs.insert(bank, var);
        self.iface.cbufs.insert(bank);
        Ok(var)
    }

    fn load_cbuf(&mut self, bank: u32, off: i64) -> Res<u32> {
        if off < 0 || off % 4 != 0 || off >= i64::from(CBUF_VEC4S) * 16 {
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

    fn raw(&mut self, o: &Operand) -> Res<(u32, RegMods)> {
        Ok(match o {
            Operand::Reg(n, m) => (self.load_reg(*n), *m),
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
                bank: CBank::Imm(b),
                index: None,
                offset,
                mods,
                ..
            } => (self.load_cbuf(*b, *offset)?, *mods),
            o => return bad(format!("operand {o}")),
        })
    }

    fn fsrc(&mut self, o: &Operand) -> Res<u32> {
        let (u, m) = self.raw(o)?;
        if m.not || m.swz != crate::Swz::None {
            return bad(format!("float operand mods {o}"));
        }
        let mut f = self.as_float(u);
        let t = self.f32t();
        if m.abs {
            f = self.ext(t, glsl::F_ABS, &[f]);
        }
        if m.neg {
            f = self.m.emit(op::F_NEGATE, t, &[f]);
        }
        Ok(f)
    }

    fn usrc(&mut self, o: &Operand) -> Res<u32> {
        let (u, m) = self.raw(o)?;
        if m.neg || m.abs || m.not || m.swz != crate::Swz::None {
            return bad(format!("int operand mods {o}"));
        }
        Ok(u)
    }

    fn dst_reg(o: &Operand) -> Res<u8> {
        match o {
            Operand::Reg(n, _) => Ok(*n),
            o => bad(format!("dst {o}")),
        }
    }

    fn sat(&mut self, f: u32, on: bool) -> u32 {
        if !on {
            return f;
        }
        let t = self.f32t();
        let z = self.m.c_f32(0);
        let one = self.m.c_f32(1.0f32.to_bits());
        self.ext(t, glsl::N_CLAMP, &[f, z, one])
    }

    fn float_mods_ok(i: &Inst, allowed: &[&str]) -> Res<()> {
        for m in &i.mods {
            if !allowed.contains(m) {
                return bad(format!("{} modifier {m}", i.mnemonic));
            }
        }
        Ok(())
    }

    fn input_var(&mut self, loc: u32) -> u32 {
        if let Some(&v) = self.inputs.get(&loc) {
            return v;
        }
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        let p = self.m.t_ptr(INPUT, v4);
        let var = self.m.global(p, INPUT);
        self.m.deco(var, 30, &[loc]);
        self.inputs.insert(loc, var);
        var
    }

    fn output_var(&mut self, loc: u32) -> u32 {
        if let Some(&v) = self.outputs.get(&loc) {
            return v;
        }
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        let p = self.m.t_ptr(OUTPUT, v4);
        let var = self.m.global(p, OUTPUT);
        self.m.deco(var, 30, &[loc]);
        self.outputs.insert(loc, var);
        self.iface.outputs.insert(loc);
        var
    }

    fn position_var(&mut self) -> u32 {
        if let Some(v) = self.position {
            return v;
        }
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        let p = self.m.t_ptr(OUTPUT, v4);
        let var = self.m.global(p, OUTPUT);
        self.m.deco(var, 11, &[0]);
        self.position = Some(var);
        var
    }

    fn frag_coord_var(&mut self) -> u32 {
        if let Some(v) = self.frag_coord {
            return v;
        }
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        let p = self.m.t_ptr(INPUT, v4);
        let var = self.m.global(p, INPUT);
        self.m.deco(var, 11, &[15]);
        self.frag_coord = Some(var);
        var
    }

    fn component_ptr(&mut self, var: u32, class: u32, c: u32) -> u32 {
        let f = self.f32t();
        let p = self.m.t_ptr(class, f);
        let ci = self.m.c_u32(c);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, ci])
    }

    fn attr_slot(&mut self, off: u32, store: bool) -> Res<(u32, u32)> {
        let class = if store { OUTPUT } else { INPUT };
        if self.stage == Stage::Fragment && !store && off == 0x7c {
            let v = self.frag_coord_var();
            return Ok((self.component_ptr(v, INPUT, 3), 0));
        }
        if store && (0x70..0x80).contains(&off) && self.stage != Stage::Fragment {
            let v = self.position_var();
            return Ok((self.component_ptr(v, OUTPUT, (off - 0x70) / 4), 0));
        }
        if (0x80..0x280).contains(&off) {
            let loc = (off - 0x80) / 16;
            let c = (off % 16) / 4;
            let v = if store {
                self.output_var(loc)
            } else {
                self.input_var(loc)
            };
            return Ok((self.component_ptr(v, class, c), loc + 1));
        }
        bad(format!("attribute {off:#x}"))
    }

    fn attr_operand(o: &Operand) -> Res<u32> {
        match o {
            Operand::Attr {
                index: None,
                offset,
                patch: false,
            } => Ok(*offset),
            o => bad(format!("attribute operand {o}")),
        }
    }

    fn width(i: &Inst) -> u32 {
        if i.has("64") {
            2
        } else if i.has("96") {
            3
        } else if i.has("128") {
            4
        } else {
            1
        }
    }

    fn inst(&mut self, i: &Inst) -> Res<bool> {
        let f = self.f32t();
        match i.mnemonic {
            "NOP" => {}
            "EXIT" => {
                if !i.ops.is_empty() {
                    return bad("predicated EXIT");
                }
                return Ok(true);
            }
            "FADD" | "FMUL" | "FFMA" => {
                Ctx::float_mods_ok(i, &["FTZ", "SAT", "RZ", "RM", "RP"])?;
                let d = Ctx::dst_reg(&i.ops[0])?;
                let a = self.fsrc(&i.ops[1])?;
                let b = self.fsrc(&i.ops[2])?;
                let r = match i.mnemonic {
                    "FADD" => self.m.emit(op::F_ADD, f, &[a, b]),
                    "FMUL" => self.m.emit(op::F_MUL, f, &[a, b]),
                    _ => {
                        let c = self.fsrc(&i.ops[3])?;
                        self.ext(f, glsl::FMA, &[a, b, c])
                    }
                };
                if i.mnemonic != "FFMA" {
                    self.m.deco(r, 42, &[]);
                }
                let r = self.sat(r, i.has("SAT"));
                let u = self.as_bits(r);
                self.store_reg(d, u);
            }
            "MOV" => {
                if i.ops.len() != 2 {
                    return bad("MOV with lane mask");
                }
                let d = Ctx::dst_reg(&i.ops[0])?;
                let v = self.usrc(&i.ops[1])?;
                self.store_reg(d, v);
            }
            "MUFU" => {
                let d = Ctx::dst_reg(&i.ops[0])?;
                let a = self.fsrc(&i.ops[1])?;
                let kind = i.mods.first().copied().unwrap_or("");
                let r = match kind {
                    "RCP" => {
                        let one = self.m.c_f32(1.0f32.to_bits());
                        self.m.emit(op::F_DIV, f, &[one, a])
                    }
                    "RSQ" => self.ext(f, glsl::INVERSE_SQRT, &[a]),
                    "SQRT" => self.ext(f, glsl::SQRT, &[a]),
                    "EX2" => self.ext(f, glsl::EXP2, &[a]),
                    "LG2" => self.ext(f, glsl::LOG2, &[a]),
                    "SIN" | "COS" => {
                        let k = self.m.c_f32(std::f32::consts::TAU.to_bits());
                        let x = self.m.emit(op::F_MUL, f, &[a, k]);
                        self.ext(f, if kind == "SIN" { glsl::SIN } else { glsl::COS }, &[x])
                    }
                    k => return bad(format!("MUFU.{k}")),
                };
                if i.mods.len() > 1 {
                    return bad("MUFU modifiers");
                }
                let u = self.as_bits(r);
                self.store_reg(d, u);
            }
            "ALD" => {
                let d = Ctx::dst_reg(&i.ops[0])?;
                let off = Ctx::attr_operand(&i.ops[1])?;
                for k in 0..Ctx::width(i) {
                    let (p, loc) = self.attr_slot(off + 4 * k, false)?;
                    if loc > 0 {
                        self.iface.inputs.insert(loc - 1, None);
                    }
                    let v = self.m.emit(op::LOAD, f, &[p]);
                    let u = self.as_bits(v);
                    self.store_reg(d.wrapping_add(k as u8), u);
                }
            }
            "AST" => {
                let off = Ctx::attr_operand(&i.ops[0])?;
                let s = Ctx::dst_reg(&i.ops[1])?;
                for k in 0..Ctx::width(i) {
                    let (p, _) = self.attr_slot(off + 4 * k, true)?;
                    let u = self.load_reg(s.wrapping_add(k as u8));
                    let v = self.as_float(u);
                    self.m.emit_void(op::STORE, &[p, v]);
                }
            }
            "IPA" => {
                if self.stage != Stage::Fragment {
                    return bad("IPA outside a fragment shader");
                }
                if i.mods.iter().any(|m| *m != "PASS" && *m != "CONSTANT") || i.ops.len() != 2 {
                    return bad(format!("IPA form {i}"));
                }
                let d = Ctx::dst_reg(&i.ops[0])?;
                let off = Ctx::attr_operand(&i.ops[1])?;
                let (p, loc) = self.attr_slot(off, false)?;
                let mut v = self.m.emit(op::LOAD, f, &[p]);
                if loc > 0 {
                    let mode = if i.has("CONSTANT") {
                        Interp::Flat
                    } else {
                        Interp::Perspective
                    };
                    let e = self.iface.inputs.entry(loc - 1).or_insert(Some(mode));
                    if *e != Some(mode) {
                        return bad("mixed interpolation on one attribute");
                    }
                    if mode == Interp::Perspective {
                        let fc = self.frag_coord_var();
                        let wp = self.component_ptr(fc, INPUT, 3);
                        let w = self.m.emit(op::LOAD, f, &[wp]);
                        v = self.m.emit(op::F_MUL, f, &[v, w]);
                    }
                }
                let u = self.as_bits(v);
                self.store_reg(d, u);
            }
            m => return bad(format!("opcode {m}")),
        }
        Ok(false)
    }

    fn guarded(&mut self, i: &Inst) -> Res<bool> {
        let Some(g) = &i.guard else {
            return self.inst(i);
        };
        let Operand::Pred(n, not) = g else {
            return bad("uniform guard");
        };
        if i.mnemonic == "EXIT" || i.mnemonic == "BRA" {
            return bad("predicated control flow");
        }
        let c = self.load_pred(*n, *not);
        let then = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[c, then, merge]);
        self.m.emit_void(op::LABEL, &[then]);
        self.inst(i)?;
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        Ok(false)
    }

    fn fragment_outputs(&mut self, targets: u32) {
        let f = self.f32t();
        let mut reg = 0u8;
        for t in 0..8 {
            let mask = (targets >> (4 * t)) & 0xf;
            if mask == 0 {
                continue;
            }
            let v4 = self.m.t_vec(f, 4);
            let p = self.m.t_ptr(OUTPUT, v4);
            let var = self.m.global(p, OUTPUT);
            self.m.deco(var, 30, &[t]);
            self.colors.insert(t, var);
            self.iface.color_targets.insert(t);
            for c in 0..4 {
                if mask & (1 << c) == 0 {
                    continue;
                }
                let u = self.load_reg(reg);
                reg += 1;
                let v = self.as_float(u);
                let ptr = self.component_ptr(var, OUTPUT, c);
                self.m.emit_void(op::STORE, &[ptr, v]);
            }
        }
    }
}

pub fn translate(p: &Program) -> Res<Shader> {
    let stage = p.stage();
    if !matches!(stage, Stage::Vertex | Stage::Fragment) {
        return bad(format!("stage {stage:?}"));
    }
    let mut m = Module::new();
    let glsl_id = m.id();
    let mut c = Ctx {
        m,
        glsl: glsl_id,
        stage,
        regs: BTreeMap::new(),
        preds: BTreeMap::new(),
        inputs: BTreeMap::new(),
        outputs: BTreeMap::new(),
        cbufs: BTreeMap::new(),
        cbuf_block: None,
        frag_coord: None,
        position: None,
        colors: BTreeMap::new(),
        iface: Interface::default(),
    };
    let void = c.m.t_void();
    let fnt = c.m.t_fn(void);
    let main = c.m.id();
    c.m.emit_void(op::FUNCTION, &[void, main, 0, fnt]);
    let entry = c.m.id();
    c.m.emit_void(op::LABEL, &[entry]);
    let var_at = c.m.body.len();
    let mut exited = false;
    for (n, d) in p.decode().into_iter().enumerate() {
        let i = d.map_err(|e| Unsupported(format!("decode: {e}")))?;
        if exited {
            if i.mnemonic == "BRA"
                && matches!(i.ops.as_slice(), [Operand::Target(t)] if *t == 16 * n as i64)
            {
                break;
            }
            if i.mnemonic == "NOP" {
                continue;
            }
            return bad("code after EXIT");
        }
        exited = c.guarded(&i)?;
    }
    if !exited {
        return bad("no EXIT");
    }
    if stage == Stage::Fragment {
        let targets = p.sph.as_ref().map_or(0, |s| s.words[18]);
        c.fragment_outputs(targets);
    }
    c.m.emit_void(op::RETURN, &[]);
    c.m.emit_void(op::FUNCTION_END, &[]);
    let u = c.m.t_u32();
    let pu = c.m.t_ptr(FUNCTION, u);
    let b = c.m.t_bool();
    let pb = c.m.t_ptr(FUNCTION, b);
    let mut decls = Vec::new();
    for &v in c.regs.values() {
        crate::spirv::inst(&mut decls, op::VARIABLE, &[pu, v, FUNCTION]);
    }
    for &v in c.preds.values() {
        crate::spirv::inst(&mut decls, op::VARIABLE, &[pb, v, FUNCTION]);
    }
    let zero = c.m.c_u32(0);
    let fals = c.m.c_bool(false);
    let mut inits = Vec::new();
    for &v in c.regs.values() {
        crate::spirv::inst(&mut inits, op::STORE, &[v, zero]);
    }
    for &v in c.preds.values() {
        crate::spirv::inst(&mut inits, op::STORE, &[v, fals]);
    }
    decls.extend(inits);
    c.m.body.splice(var_at..var_at, decls);
    let mut iface: Vec<u32> = Vec::new();
    iface.extend(c.inputs.values());
    iface.extend(c.outputs.values());
    iface.extend(c.colors.values());
    iface.extend(c.frag_coord);
    iface.extend(c.position);
    for (loc, &var) in &c.inputs {
        if c.iface.inputs.get(loc) == Some(&Some(Interp::Flat)) {
            c.m.deco(var, 14, &[]);
        }
    }
    crate::spirv::inst(&mut c.m.caps, op::CAPABILITY, &[1]);
    let mut e = vec![glsl_id];
    e.extend(crate::spirv::str_words("GLSL.std.450"));
    crate::spirv::inst(&mut c.m.ext, op::EXT_INST_IMPORT, &e);
    crate::spirv::inst(&mut c.m.head, op::MEMORY_MODEL, &[0, 1]);
    let model = if stage == Stage::Fragment { 4 } else { 0 };
    let mut ep = vec![model, main];
    ep.extend(crate::spirv::str_words("main"));
    ep.extend(iface);
    crate::spirv::inst(&mut c.m.head, op::ENTRY_POINT, &ep);
    if stage == Stage::Fragment {
        crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &[main, 7]);
    }
    let iface = c.iface;
    Ok(Shader {
        stage,
        spirv: c.m.finish(),
        interface: iface,
    })
}
