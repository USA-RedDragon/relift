//! Stage inputs and outputs: ALD, AST, IPA and the fragment outputs the
//! SPH names.
//!
//! Attribute `a[0x80 + 16 n]` is location `n`; `a[0x70]` is the vertex
//! position; in a fragment shader `a[0x7c]` is FragCoord.w, which IPA.PASS
//! multiplies in (perspective-correct interpolation).

use super::{INPUT, Interp, OUTPUT, Res, bad};
use crate::program::Stage;
use crate::spirv::op;
use crate::{Inst, Operand};

use super::Ctx;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "ALD" => ald(c, i),
        "AST" => ast(c, i),
        "IPA" => ipa(c, i),
        _ => return None,
    })
}

impl Ctx {
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

    fn vec4_builtin(&mut self, b: u32, class: u32) -> u32 {
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        self.builtin(b, class, v4)
    }

    pub(crate) fn component_ptr(&mut self, var: u32, class: u32, c: u32) -> u32 {
        let f = self.f32t();
        let p = self.m.t_ptr(class, f);
        let ci = self.m.c_u32(c);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, ci])
    }

    fn attr_slot(&mut self, off: u32, store: bool) -> Res<(u32, u32)> {
        let class = if store { OUTPUT } else { INPUT };
        if self.stage == Stage::Fragment && !store && (0x70..0x80).contains(&off) {
            let v = self.vec4_builtin(15, INPUT);
            return Ok((self.component_ptr(v, INPUT, (off - 0x70) / 4), 0));
        }
        if store && (0x70..0x80).contains(&off) && self.stage != Stage::Fragment {
            let v = self.vec4_builtin(0, OUTPUT);
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

    pub(crate) fn fragment_outputs(&mut self, targets: u32) {
        let f = self.f32t();
        let mut reg = 0u8;
        for t in 0..8 {
            let mask = (targets >> (4 * t)) & 0xf;
            if mask == 0 {
                continue;
            }
            let var = match self.colors.get(&t) {
                Some(&v) => v,
                None => {
                    let v4 = self.m.t_vec(f, 4);
                    let p = self.m.t_ptr(OUTPUT, v4);
                    let var = self.m.global(p, OUTPUT);
                    self.m.deco(var, 30, &[t]);
                    self.colors.insert(t, var);
                    self.iface.color_targets.insert(t);
                    var
                }
            };
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

fn ald(c: &mut Ctx, i: &Inst) -> Res<()> {
    let f = c.f32t();
    let d = Ctx::dst_reg(&i.ops[0])?;
    let off = attr_operand(&i.ops[1])?;
    for k in 0..width(i) {
        let (p, loc) = c.attr_slot(off + 4 * k, false)?;
        if loc > 0 {
            c.iface.inputs.insert(loc - 1, None);
        }
        let v = c.m.emit(op::LOAD, f, &[p]);
        let u = c.as_bits(v);
        c.store_reg(d.wrapping_add(k as u8), u);
    }
    Ok(())
}

fn ast(c: &mut Ctx, i: &Inst) -> Res<()> {
    let off = attr_operand(&i.ops[0])?;
    let s = Ctx::dst_reg(&i.ops[1])?;
    for k in 0..width(i) {
        let (p, _) = c.attr_slot(off + 4 * k, true)?;
        let u = c.load_reg(s.wrapping_add(k as u8));
        let v = c.as_float(u);
        c.m.emit_void(op::STORE, &[p, v]);
    }
    Ok(())
}

fn ipa(c: &mut Ctx, i: &Inst) -> Res<()> {
    let f = c.f32t();
    if c.stage != Stage::Fragment {
        return bad("IPA outside a fragment shader");
    }
    if i.mods.iter().any(|m| *m != "PASS" && *m != "CONSTANT") || i.ops.len() != 2 {
        return bad(format!("IPA form {i}"));
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let off = attr_operand(&i.ops[1])?;
    let (p, loc) = c.attr_slot(off, false)?;
    let mut v = c.m.emit(op::LOAD, f, &[p]);
    if loc > 0 {
        let mode = if i.has("CONSTANT") {
            Interp::Flat
        } else {
            Interp::Perspective
        };
        let e = c.iface.inputs.entry(loc - 1).or_insert(Some(mode));
        if *e != Some(mode) {
            return bad("mixed interpolation on one attribute");
        }
        if mode == Interp::Perspective {
            let fc = c.vec4_builtin(15, INPUT);
            let wp = c.component_ptr(fc, INPUT, 3);
            let w = c.m.emit(op::LOAD, f, &[wp]);
            v = c.m.emit(op::F_MUL, f, &[v, w]);
        }
    }
    let u = c.as_bits(v);
    c.store_reg(d, u);
    Ok(())
}
