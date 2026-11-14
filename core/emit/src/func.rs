//! One guest function as one C function.

use std::collections::BTreeSet;
use std::fmt::Write as _;

use relift_guest::{Frontend, Home};
use relift_ir::{Block, Flow, Next, Op, Reg};

use crate::value::{Emitted, cty, emit_op, reg, val};
use crate::{Function, Insn, fn_name};

/// What one function emitted.
pub(crate) struct Emitted1 {
    pub(crate) text: String,
    pub(crate) callees: BTreeSet<u64>,
    pub(crate) undefined: usize,
}

fn label(pc: u64) -> String {
    format!("L_{pc:x}")
}

struct Ctx<'a> {
    fe: &'a dyn Frontend,
    f: &'a Function<'a>,
    entries: &'a BTreeSet<u64>,
    callees: BTreeSet<u64>,
}

impl Ctx<'_> {
    fn inside(&self, t: u64) -> bool {
        self.f.insts.contains_key(&t) && (t == self.f.entry || !self.entries.contains(&t))
    }

    /// A transfer to the constant `t` that leaves no frame behind.
    fn jump(&mut self, t: u64) -> String {
        if self.inside(t) {
            return format!("goto {};", label(t));
        }
        if self.entries.contains(&t) {
            self.callees.insert(t);
            return format!("RL_STORE(); RL_MUSTTAIL return {}(c, mem);", fn_name(t));
        }
        format!("RL_STORE(); RL_MUSTTAIL return rl_indirect(c, UINT64_C({t:#x}))(c, mem);")
    }
}

/// Emits `f`; `entries` are every function entry of the module.
pub(crate) fn emit_function(
    fe: &dyn Frontend,
    f: &Function<'_>,
    entries: &BTreeSet<u64>,
) -> Emitted1 {
    let mut used: BTreeSet<Reg> = BTreeSet::new();
    let mut written: BTreeSet<Reg> = BTreeSet::new();
    let mut has_fp = false;
    for i in f.insts.values() {
        if let Insn::Lifted { block: b, .. } = i {
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
                    _ => {}
                }
            }
        }
    }
    if has_fp {
        used.extend(fe.fp_state());
    }
    let regs = fe.regs();
    let mut t = String::new();
    let _ = writeln!(t, "\nRL_LIFTED({})\n{{", fn_name(f.entry));
    for &r in &used {
        let ty = cty(regs.width(r));
        let _ = match fe.home(r) {
            Home::Field(lv) => writeln!(t, "    {ty} {} = {lv};", reg(r)),
            Home::Flag { word, bit } => {
                writeln!(t, "    {ty} {} = ({ty})(({word} >> {bit}) & 1u);", reg(r))
            }
        };
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
    t.push_str(" } while (0)\n");
    let _ = writeln!(t, "    goto {};", label(f.entry));
    let mut cx = Ctx {
        fe,
        f,
        entries,
        callees: BTreeSet::new(),
    };
    let mut undefined = 0;
    for (&pc, i) in &f.insts {
        let _ = writeln!(t, "{}: {{", label(pc));
        let body = match i {
            Insn::Lifted { block: b, raw } => block(&mut cx, b, *raw),
            Insn::Failed(e) => Err((e.insn, e.message.clone())),
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
    t.push_str("#undef RL_STORE\n#undef RL_RELOAD\n}\n");
    Emitted1 {
        text: t,
        callees: cx.callees,
        undefined,
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
        }
    }
    for (r, e) in sets {
        let _ = writeln!(s, "    {} = {e};", reg(r));
    }
    let after = |s: &mut String, call: String| {
        let _ = writeln!(
            s,
            "    RL_STORE();\n    rc = {call};\n    if (RL_UNLIKELY(rc))\n        return RL_PENDING;\n    RL_RELOAD();"
        );
    };
    match b.exit.flow {
        Flow::Return => s.push_str("    RL_STORE();\n    return RL_OK;\n"),
        Flow::Call { ret } => {
            let call = match b.exit.next {
                Next::Const(t) if cx.entries.contains(&t) => {
                    cx.callees.insert(t);
                    format!("{}(c, mem)", fn_name(t))
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
