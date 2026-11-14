//! One guest function as one C function.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_guest::{Frontend, Home};
use relift_ir::{Block, Flow, Next, Op, Reg};

use crate::value::{Emitted, cty, emit_op, reg, val};
use crate::{Function, Insn, Target};

/// What one function emitted.
pub(crate) struct Emitted1 {
    pub(crate) text: String,
    pub(crate) callees: BTreeSet<u64>,
    pub(crate) undefined: usize,
    /// The C expression of the function's `rl_fn_flags`.
    pub(crate) flags: &'static str,
}

fn label(pc: u64) -> String {
    format!("L_{pc:x}")
}

struct Ctx<'a> {
    fe: &'a dyn Frontend,
    f: &'a Function<'a>,
    targets: &'a BTreeMap<u64, Target>,
    callees: BTreeSet<u64>,
    hoisted: BTreeSet<Reg>,
    /// The landing-pad check's sp local, when the function has resume points.
    lp_sp: Option<String>,
}

/// The C local holding whether hoisted guard register `r` is nonzero.
fn flag(r: Reg) -> String {
    format!("g{}", reg(r))
}

impl Ctx<'_> {
    fn inside(&self, t: u64) -> bool {
        self.f.insts.contains_key(&t) && (t == self.f.entry || !self.targets.contains_key(&t))
    }

    /// A transfer to the constant `t` that leaves no frame behind.
    fn jump(&mut self, t: u64) -> String {
        if self.inside(t) {
            return format!("goto {};", label(t));
        }
        if let Some(target) = self.targets.get(&t) {
            self.callees.insert(t);
            return format!("RL_STORE(); RL_MUSTTAIL return {}(c, mem);", target.name);
        }
        format!("RL_STORE(); RL_MUSTTAIL return rl_indirect(c, UINT64_C({t:#x}))(c, mem);")
    }
}

/// The block as the emitter writes it: without the values nothing
/// observable uses.
fn prepare(b: &Block) -> Block {
    relift_ir::dce(b)
}

/// The return addresses of the calls in `f` that are resume points.
fn resume_points(f: &Function<'_>, targets: &BTreeMap<u64, Target>) -> BTreeSet<u64> {
    f.insts
        .values()
        .filter_map(|i| match i {
            Insn::Lifted { block: b, .. } => match (b.exit.flow, b.exit.next) {
                (Flow::Call { ret }, Next::Const(t))
                    if targets.get(&t).is_some_and(|t| t.resume_point) =>
                {
                    Some(ret)
                }
                _ => None,
            },
            Insn::Failed(_) => None,
        })
        .collect()
}

/// Emits `f`; `targets` are every direct-call target of the module.
pub(crate) fn emit_function(
    fe: &dyn Frontend,
    f: &Function<'_>,
    targets: &BTreeMap<u64, Target>,
) -> Emitted1 {
    let resumes = resume_points(f, targets);
    let lp_reg = fe.stack_pointer().filter(|_| !resumes.is_empty());
    let blocks: BTreeMap<u64, Block> = f
        .insts
        .iter()
        .filter_map(|(&pc, i)| match i {
            Insn::Lifted { block, .. } => Some((pc, prepare(block))),
            Insn::Failed(_) => None,
        })
        .collect();
    let mut used: BTreeSet<Reg> = BTreeSet::new();
    let mut written: BTreeSet<Reg> = BTreeSet::new();
    let mut has_fp = false;
    let mut hoisted: BTreeSet<Reg> = BTreeSet::new();
    for b in blocks.values() {
        for inst in &b.insts {
            match inst.op {
                Op::Get(r) => {
                    used.insert(r);
                }
                Op::Set(r, _) => {
                    used.insert(r);
                    written.insert(r);
                }
                Op::Fp(..) => has_fp = true,
                Op::Guard(_, code) => {
                    if let Some(r) = fe.guard_state(code) {
                        hoisted.insert(r);
                    }
                }
                _ => {}
            }
        }
    }
    if has_fp {
        used.extend(fe.fp_state());
    }
    used.extend(hoisted.iter().copied());
    used.extend(lp_reg);
    let regs = fe.regs();
    let mut t = String::new();
    let _ = writeln!(t, "\nRL_TEXT RL_LIFTED({})\n{{", f.name);
    for &r in &used {
        let ty = cty(regs.width(r));
        let _ = match fe.home(r) {
            Home::Field(lv) => writeln!(t, "    {ty} {} = {lv};", reg(r)),
            Home::Flag { word, bit } => {
                writeln!(t, "    {ty} {} = ({ty})(({word} >> {bit}) & 1u);", reg(r))
            }
        };
    }
    for &r in &hoisted {
        let _ = writeln!(t, "    int {} = {} != 0;", flag(r), reg(r));
    }
    t.push_str("    int rc;\n    (void)rc;\n");
    t.push_str("#define RL_STORE() do {");
    for &r in &written {
        let _ = match fe.home(r) {
            Home::Field(lv) => write!(t, " {lv} = {};", reg(r)),
            Home::Flag { word, bit } => write!(
                t,
                " {word} = ({word} & ~(1u << {bit})) | ((uint32_t){} << {bit});",
                reg(r)
            ),
        };
    }
    t.push_str(" } while (0)\n#define RL_RELOAD() do {");
    for &r in &used {
        let ty = cty(regs.width(r));
        let _ = match fe.home(r) {
            Home::Field(lv) => write!(t, " {} = {lv};", reg(r)),
            Home::Flag { word, bit } => {
                write!(t, " {} = ({ty})(({word} >> {bit}) & 1u);", reg(r))
            }
        };
    }
    for &r in &hoisted {
        let _ = write!(t, " {} = {} != 0;", flag(r), reg(r));
    }
    t.push_str(" } while (0)\n");
    let _ = writeln!(t, "    goto {};", label(f.entry));
    let mut cx = Ctx {
        fe,
        f,
        targets,
        callees: BTreeSet::new(),
        hoisted,
        lp_sp: lp_reg.map(reg),
    };
    let mut undefined = 0;
    for (&pc, i) in &f.insts {
        let _ = writeln!(t, "{}: {{", label(pc));
        let body = match (i, blocks.get(&pc)) {
            (Insn::Lifted { raw, .. }, Some(b)) => block(&mut cx, b, *raw),
            (Insn::Failed(e), _) => Err((e.insn, e.message.clone())),
            (Insn::Lifted { raw, .. }, None) => Err((*raw, "no block".into())),
        };
        match body {
            Ok(s) => t.push_str(&s),
            Err((insn, _)) => {
                undefined += 1;
                let _ = writeln!(t, "    RL_STORE();\n    return {};", fe.undefined(pc, insn));
            }
        }
        t.push_str("}\n");
    }
    if lp_reg.is_some() {
        t.push_str("lp_dispatch:\n    switch (c->pend.pc) {\n");
        for r in &resumes {
            let next = cx.jump(*r);
            let _ = writeln!(
                t,
                "    case UINT64_C({r:#x}):\n        c->pend.kind = RL_PEND_NONE;\n        RL_RELOAD();\n        {next}"
            );
        }
        let _ = writeln!(
            t,
            "    default:\n        break;\n    }}\n    rl_bad_resume(c, mem, UINT64_C({:#x}));",
            f.entry
        );
    }
    t.push_str("#undef RL_STORE\n#undef RL_RELOAD\n}\n");
    Emitted1 {
        text: t,
        callees: cx.callees,
        undefined,
        flags: if lp_reg.is_some() {
            "RL_FN_MAY_UNWIND | RL_FN_LANDING_PADS"
        } else {
            "RL_FN_MAY_UNWIND"
        },
    }
}

/// The body of one instruction, or the raw word and reason it cannot be
/// emitted.
fn block(cx: &mut Ctx<'_>, b: &Block, raw: u64) -> Result<String, (u64, String)> {
    let mut s = String::new();
    let mut sets = Vec::new();
    for (i, inst) in b.insts.iter().enumerate() {
        match emit_op(cx.fe, b, i).map_err(|m| (raw, m))? {
            Emitted::Value(e) => {
                let ty = cty(inst.width.unwrap_or(relift_ir::Width::W1));
                let _ = writeln!(s, "    {ty} v{i} = {e};");
            }
            Emitted::Stmt(e) => {
                let _ = writeln!(s, "    {e};");
            }
            Emitted::Set(r, e) => sets.push((r, e)),
            Emitted::Guard(c, code) => {
                let cond = match cx.fe.guard_state(code) {
                    Some(r) if cx.hoisted.contains(&r) => flag(r),
                    _ => val(c),
                };
                let slow = cx
                    .fe
                    .guard_slow(code, b.pc, raw)
                    .unwrap_or_else(|| cx.fe.undefined(b.pc, raw));
                let next = cx.jump(b.pc + u64::from(b.len));
                let _ = writeln!(
                    s,
                    "    if (RL_UNLIKELY({cond})) {{\n        RL_STORE();\n        rc = {slow};\n        if (RL_UNLIKELY(rc))\n            return RL_PENDING;\n        RL_RELOAD();\n        {next}\n    }}"
                );
            }
        }
    }
    for (r, e) in sets {
        let _ = writeln!(s, "    {} = {e};", reg(r));
        if cx.hoisted.contains(&r) {
            let _ = writeln!(s, "    {} = {} != 0;", flag(r), reg(r));
        }
    }
    let check = match &cx.lp_sp {
        Some(sp) => format!(
            "    if (RL_UNLIKELY(rc)) {{\n        if (c->pend.sp == {sp})\n            goto lp_dispatch;\n        return RL_PENDING;\n    }}"
        ),
        None => "    if (RL_UNLIKELY(rc))\n        return RL_PENDING;".to_string(),
    };
    let after = |s: &mut String, call: String| {
        let _ = writeln!(
            s,
            "    RL_STORE();\n    rc = {call};\n{check}\n    RL_RELOAD();"
        );
    };
    match b.exit.flow {
        Flow::Return => s.push_str("    RL_STORE();\n    return RL_OK;\n"),
        Flow::Call { ret } => {
            let call = match b.exit.next {
                Next::Const(t) if cx.targets.contains_key(&t) => {
                    cx.callees.insert(t);
                    format!("{}(c, mem)", cx.targets[&t].name)
                }
                Next::Const(t) => format!("rl_indirect(c, UINT64_C({t:#x}))(c, mem)"),
                Next::Dynamic(v) => format!("rl_indirect(c, {})(c, mem)", val(v)),
                Next::Select { .. } => return Err((raw, "a conditional call".into())),
            };
            after(&mut s, call);
            let _ = writeln!(s, "    {}", cx.jump(ret));
        }
        Flow::Trap(code) => {
            after(&mut s, cx.fe.trap(code));
            match b.exit.next {
                Next::Const(t) => {
                    let _ = writeln!(s, "    {}", cx.jump(t));
                }
                _ => return Err((raw, "a trap with a computed resume".into())),
            }
        }
        Flow::Jump => match b.exit.next {
            Next::Const(t) => {
                let _ = writeln!(s, "    {}", cx.jump(t));
            }
            Next::Select {
                cond,
                taken,
                not_taken,
            } => {
                let (jt, jn) = (cx.jump(taken), cx.jump(not_taken));
                let _ = writeln!(s, "    if ({}) {{ {jt} }}\n    {jn}", val(cond));
            }
            Next::Dynamic(v) => {
                let known: Vec<u64> =
                    cx.f.tables
                        .get(&b.pc)
                        .map(|t| t.iter().copied().filter(|t| cx.inside(*t)).collect())
                        .unwrap_or_default();
                if !known.is_empty() {
                    let _ = writeln!(s, "    switch ({}) {{", val(v));
                    for t in known.iter().collect::<BTreeSet<_>>() {
                        let _ = writeln!(s, "    case UINT64_C({t:#x}): goto {};", label(*t));
                    }
                    s.push_str("    default: break;\n    }\n");
                }
                let _ = writeln!(
                    s,
                    "    RL_STORE();\n    RL_MUSTTAIL return rl_indirect(c, {})(c, mem);",
                    val(v)
                );
            }
        },
    }
    Ok(s)
}
