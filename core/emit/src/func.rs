//! One guest function as one C function, or as several when its C would
//! be too large for one.
//!
//! A split function keeps its own name as a stub that sets `c->pc` to the
//! guest entry and tail-calls the first piece. Each piece is a C function
//! with the lifted signature over a run of the function's instructions: it
//! loads the registers it uses from the context, switches on `c->pc` to the
//! instruction it is entered at, and leaves for another piece by storing the
//! registers it wrote, setting `c->pc` to the target and tail-calling that
//! piece. The context then holds the full state at every transfer, as at a
//! call boundary, so a piece is entered like a function.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_guest::{Frontend, Home};
use relift_ir::{Block, Flow, IoMap, Next, Op, Reg, Value};

use crate::time;
use crate::value::{Emitted, cty, emit_op, inlined, reg, val};
use crate::{Function, Insn, Target};

/// One C function of an emitted guest function.
pub(crate) struct Piece {
    /// The C name.
    pub(crate) name: String,
    /// The C definition.
    pub(crate) text: String,
    /// The lowest guest address of the instructions it holds; none for a stub.
    pub(crate) first_pc: Option<u64>,
}

/// What one function emitted.
pub(crate) struct Emitted1 {
    /// The C functions, the guest function's own name first; more than one
    /// when the function was split.
    pub(crate) pieces: Vec<Piece>,
    pub(crate) callees: BTreeSet<u64>,
    pub(crate) undefined: usize,
    /// The C expression of the function's `rl_fn_flags`.
    pub(crate) flags: &'static str,
}

impl Emitted1 {
    /// The bytes of C of all pieces.
    pub(crate) fn len(&self) -> usize {
        self.pieces.iter().map(|p| p.text.len()).sum()
    }
}

fn label(pc: u64) -> String {
    format!("L_{pc:x}")
}

/// How the instructions of a split function are divided.
struct Split {
    /// The piece of each instruction.
    piece_of: BTreeMap<u64, usize>,
    /// The C name of each piece.
    names: Vec<String>,
}

struct Ctx<'a> {
    fe: &'a dyn Frontend,
    f: &'a Function<'a>,
    targets: &'a BTreeMap<u64, Target>,
    callees: BTreeSet<u64>,
    hoisted: BTreeSet<Reg>,
    /// The landing-pad check's sp local, when the function has resume points.
    lp_sp: Option<String>,
    /// The division into pieces and the piece being written, when split.
    split: Option<(&'a Split, usize)>,
    /// The guest's I/O map.
    io: &'a IoMap,
    /// The C lvalue of the clock, when the piece keeps a budget local.
    clock: Option<&'static str>,
}

/// The C local holding whether hoisted guard register `r` is nonzero.
fn flag(r: Reg) -> String {
    format!("g{}", reg(r))
}

/// Whether a transfer to `t` stays in `f`: analysis puts in a function's
/// extent only code it owns, which may include an address that is also the
/// entry of another function (an address-taken label inside it), and a jump
/// there must keep the function's state rather than leave it.
fn inside(f: &Function<'_>, _targets: &BTreeMap<u64, Target>, t: u64) -> bool {
    f.insts.contains_key(&t)
}

impl Ctx<'_> {
    fn inside(&self, t: u64) -> bool {
        inside(self.f, self.targets, t)
    }

    /// A transfer to the constant `t` that leaves no frame behind.
    fn jump(&mut self, t: u64) -> String {
        if self.inside(t) {
            if let Some((s, k)) = self.split {
                let to = s.piece_of[&t];
                if to != k {
                    return format!(
                        "RL_STORE(); c->pc = UINT64_C({t:#x}); RL_MUSTTAIL return {}(c, mem);",
                        s.names[to]
                    );
                }
            }
            return format!("goto {};", label(t));
        }
        if let Some(target) = self.targets.get(&t) {
            self.callees.insert(t);
            return format!("RL_STORE(); RL_MUSTTAIL return {}(c, mem);", target.name);
        }
        format!("RL_STORE(); RL_MUSTTAIL return rl_indirect(c, UINT64_C({t:#x}))(c, mem);")
    }
}

/// The block as the emitter writes it: constants folded, operations that
/// give an operand unchanged dropped, and without the values nothing
/// observable uses.
fn prepare(b: &Block) -> Block {
    relift_ir::dce(&relift_ir::simplify(b))
}

/// The registers live after each instruction of `f`, by address, as a bit
/// set over the register file (bit `r % 64` of word `r / 64`), by backward
/// dataflow over the function's instructions.
///
/// Every register counts as live wherever the C stores the context: at
/// calls, traps, returns, tail calls, indirect jumps, guards and
/// instructions that do not lift. So a register is dead after an
/// instruction only when every path redefines it before it is read or
/// stored, and dropping its write there changes nothing observable.
fn live_out(
    f: &Function<'_>,
    targets: &BTreeMap<u64, Target>,
    blocks: &BTreeMap<u64, Block>,
    regs: usize,
) -> BTreeMap<u64, Vec<u64>> {
    let words = regs.div_ceil(64);
    let mut all = vec![u64::MAX; words];
    if !regs.is_multiple_of(64)
        && let Some(last) = all.last_mut()
    {
        *last = (1u64 << (regs % 64)) - 1;
    }
    let pcs: Vec<u64> = f.insts.keys().copied().collect();
    let n = pcs.len();
    let at = |t: u64| pcs.binary_search(&t).ok();
    let set = |v: &mut [u64], r: usize| v[r / 64] |= 1 << (r % 64);
    // Uses (reads, or all registers where the context is stored inside the
    // instruction), definitions and in-function successors; `None` for an
    // instruction whose exit may leave the function.
    let mut uses = vec![0u64; n * words];
    let mut defs = vec![0u64; n * words];
    let mut succs: Vec<Option<Vec<usize>>> = Vec::with_capacity(n);
    for (k, pc) in pcs.iter().enumerate() {
        let u = &mut uses[k * words..(k + 1) * words];
        let Some(b) = blocks.get(pc) else {
            u.copy_from_slice(&all);
            succs.push(None);
            continue;
        };
        let d = &mut defs[k * words..(k + 1) * words];
        let mut stores = false;
        for i in &b.insts {
            match i.op {
                Op::Get(r) => set(u, r.index()),
                Op::Set(r, _) => set(d, r.index()),
                Op::Guard(..) => stores = true,
                _ => {}
            }
        }
        if stores {
            u.copy_from_slice(&all);
        }
        let local = |t: u64| if inside(f, targets, t) { at(t) } else { None };
        succs.push(match (b.exit.flow, b.exit.next) {
            (Flow::Jump, Next::Const(t)) => local(t).map(|t| vec![t]),
            (
                Flow::Jump,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => local(taken).zip(local(not_taken)).map(|(a, b)| vec![a, b]),
            _ => None,
        });
    }
    let mut preds: Vec<Vec<usize>> = vec![Vec::new(); n];
    for (k, s) in succs.iter().enumerate() {
        for &t in s.iter().flatten() {
            preds[t].push(k);
        }
    }
    let mut live_in = vec![0u64; n * words];
    let mut out = vec![0u64; n * words];
    let mut queued = vec![true; n];
    let mut work: Vec<usize> = (0..n).collect();
    while let Some(k) = work.pop() {
        queued[k] = false;
        let mut o = vec![0u64; words];
        match &succs[k] {
            None => o.copy_from_slice(&all),
            Some(ts) => {
                for &t in ts {
                    for (a, b) in o.iter_mut().zip(&live_in[t * words..(t + 1) * words]) {
                        *a |= *b;
                    }
                }
            }
        }
        let mut changed = false;
        for w in 0..words {
            let li = uses[k * words + w] | (o[w] & !defs[k * words + w]);
            if live_in[k * words + w] != li {
                live_in[k * words + w] = li;
                changed = true;
            }
        }
        out[k * words..(k + 1) * words].copy_from_slice(&o);
        if changed {
            for &p in &preds[k] {
                if !queued[p] {
                    queued[p] = true;
                    work.push(p);
                }
            }
        }
    }
    pcs.iter()
        .enumerate()
        .map(|(k, &pc)| (pc, out[k * words..(k + 1) * words].to_vec()))
        .collect()
}

/// `blocks` without the register writes no later instruction can see
/// ([`live_out`]), and without the values only those writes used.
fn drop_dead_sets(
    fe: &dyn Frontend,
    f: &Function<'_>,
    targets: &BTreeMap<u64, Target>,
    blocks: BTreeMap<u64, Block>,
) -> BTreeMap<u64, Block> {
    let regs = fe.regs();
    let live = live_out(f, targets, &blocks, regs.len());
    blocks
        .into_iter()
        .map(|(pc, mut b)| {
            let mut dropped = false;
            if let Some(l) = live.get(&pc) {
                for i in &mut b.insts {
                    if let Op::Set(r, _) = i.op
                        && l[r.index() / 64] & (1 << (r.index() % 64)) == 0
                    {
                        // A read has no effect, so the pass below removes it.
                        i.op = Op::Get(r);
                        i.width = Some(regs.width(r));
                        dropped = true;
                    }
                }
            }
            if dropped {
                b = relift_ir::dce(&b);
            }
            (pc, b)
        })
        .collect()
}

/// The return addresses of the calls in `f` that are resume points.
fn resume_points(f: &Function<'_>, targets: &BTreeMap<u64, Target>) -> BTreeSet<u64> {
    f.pads
        .iter()
        .copied()
        .filter(|p| f.insts.contains_key(p))
        .chain(f.insts.values().filter_map(|i| match i {
            Insn::Lifted { block: b, .. } => match (b.exit.flow, b.exit.next) {
                (Flow::Call { ret }, Next::Const(t))
                    if targets.get(&t).is_some_and(|t| t.resume_point) =>
                {
                    Some(ret)
                }
                _ => None,
            },
            Insn::Failed(_) => None,
        }))
        .collect()
}

/// The constant addresses the instruction at `pc` may continue at.
fn successors(f: &Function<'_>, pc: u64, b: Option<&Block>) -> Vec<u64> {
    let Some(b) = b else {
        return Vec::new();
    };
    let mut out = vec![b.pc + u64::from(b.len)];
    match b.exit.next {
        Next::Const(t) => out.push(t),
        Next::Select {
            taken, not_taken, ..
        } => out.extend([taken, not_taken]),
        Next::Dynamic(_) => out.extend(f.tables.get(&pc).into_iter().flatten().copied()),
    }
    if let Flow::Call { ret } = b.exit.flow {
        out.push(ret);
    }
    out
}

/// Emits `f`; `targets` are every direct-call target of the module. A
/// function whose C is longer than `piece_bytes` is split into pieces of
/// about that size.
pub(crate) fn emit_function(
    fe: &dyn Frontend,
    f: &Function<'_>,
    targets: &BTreeMap<u64, Target>,
    piece_bytes: usize,
    io: &IoMap,
) -> Emitted1 {
    let resumes = resume_points(f, targets);
    let blocks: BTreeMap<u64, Block> = f
        .insts
        .iter()
        .filter_map(|(&pc, i)| match i {
            Insn::Lifted { block, .. } => Some((pc, prepare(block))),
            Insn::Failed(_) => None,
        })
        .collect();
    let blocks = drop_dead_sets(fe, f, targets, blocks);
    let all: Vec<u64> = f.insts.keys().copied().collect();
    let env = Env {
        fe,
        f,
        targets,
        blocks: &blocks,
        resumes: &resumes,
        io,
    };
    let whole = emit_piece(&env, &all, None);
    if whole.text.len() <= piece_bytes || all.len() < 2 {
        return Emitted1 {
            pieces: vec![Piece {
                name: f.name.clone(),
                text: whole.text,
                first_pc: None,
            }],
            callees: whole.callees,
            undefined: whole.undefined,
            flags: whole.flags,
        };
    }
    // Divide in address order at instruction boundaries, by the size each
    // instruction took in the whole function.
    let mut piece_of = BTreeMap::new();
    let (mut k, mut size) = (0, 0);
    for pc in &all {
        let n = whole.sizes.get(pc).copied().unwrap_or(0);
        if size > 0 && size + n > piece_bytes {
            k += 1;
            size = 0;
        }
        size += n;
        piece_of.insert(*pc, k);
    }
    let split = Split {
        piece_of,
        names: (0..=k)
            .map(|i| format!("{}_part{}", f.name, i + 1))
            .collect(),
    };
    let mut pieces = vec![Piece {
        name: f.name.clone(),
        text: format!(
            "\nRL_TEXT RL_LIFTED({})\n{{\n    c->pc = UINT64_C({:#x});\n    RL_MUSTTAIL return {}(c, mem);\n}}\n",
            f.name, f.entry, split.names[split.piece_of[&f.entry]]
        ),
        first_pc: None,
    }];
    let mut callees = BTreeSet::new();
    let mut undefined = 0;
    let mut flags = whole.flags;
    for (i, name) in split.names.iter().enumerate() {
        let pcs: Vec<u64> = all
            .iter()
            .copied()
            .filter(|pc| split.piece_of[pc] == i)
            .collect();
        let p = emit_piece(&env, &pcs, Some((&split, i)));
        callees.extend(p.callees);
        undefined += p.undefined;
        flags = p.flags;
        pieces.push(Piece {
            name: name.clone(),
            text: p.text,
            first_pc: pcs.first().copied(),
        });
    }
    Emitted1 {
        pieces,
        callees,
        undefined,
        flags,
    }
}

/// What every piece of a function is emitted from.
#[derive(Clone, Copy)]
struct Env<'a> {
    fe: &'a dyn Frontend,
    f: &'a Function<'a>,
    targets: &'a BTreeMap<u64, Target>,
    blocks: &'a BTreeMap<u64, Block>,
    resumes: &'a BTreeSet<u64>,
    io: &'a IoMap,
}

/// One emitted C function over the instructions `pcs` of `f`.
struct Written {
    text: String,
    callees: BTreeSet<u64>,
    undefined: usize,
    flags: &'static str,
    /// The bytes each instruction took.
    sizes: BTreeMap<u64, usize>,
}

fn emit_piece(env: &Env<'_>, pcs: &[u64], split: Option<(&Split, usize)>) -> Written {
    let Env {
        fe,
        f,
        targets,
        blocks,
        resumes,
        io,
    } = *env;
    let lp_reg = fe.stack_pointer().filter(|_| !resumes.is_empty());
    let mut used: BTreeSet<Reg> = BTreeSet::new();
    let mut written: BTreeSet<Reg> = BTreeSet::new();
    let mut has_fp = false;
    let clock = fe.clock().filter(|_| {
        pcs.iter()
            .filter_map(|pc| blocks.get(pc))
            .any(time::uses_clock)
    });
    let mut hoisted: BTreeSet<Reg> = BTreeSet::new();
    for b in pcs.iter().filter_map(|pc| blocks.get(pc)) {
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
    let name = split.map_or(f.name.as_str(), |(s, k)| s.names[k].as_str());
    let mut t = String::new();
    let _ = writeln!(t, "\nRL_TEXT RL_LIFTED({name})\n{{");
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
    if let Some(clk) = clock {
        let _ = writeln!(t, "    int64_t {} = {clk}.budget;", time::BUDGET);
    }
    t.push_str("    int rc;\n    (void)rc;\n");
    t.push_str("#define RL_STORE() do {");
    if let Some(clk) = clock {
        let _ = write!(t, " {clk}.budget = {};", time::BUDGET);
    }
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
    if let Some(clk) = clock {
        let _ = write!(t, " {} = {clk}.budget;", time::BUDGET);
    }
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
    match split {
        None => {
            let _ = writeln!(t, "    goto {};", label(f.entry));
        }
        Some((s, k)) => {
            // The instructions other pieces (and the stub) enter this one at.
            let own: BTreeSet<u64> = pcs.iter().copied().collect();
            let mut entries: BTreeSet<u64> = BTreeSet::new();
            for (&pc, &p) in &s.piece_of {
                let from = if p == k {
                    Vec::new()
                } else {
                    successors(f, pc, blocks.get(&pc))
                };
                entries.extend(
                    from.into_iter()
                        .filter(|t| own.contains(t) && inside(f, targets, *t)),
                );
            }
            if own.contains(&f.entry) {
                entries.insert(f.entry);
            }
            if s.piece_of.len() > own.len() {
                entries.extend(resumes.iter().filter(|r| own.contains(r)));
            }
            let first = entries.first().copied().or(pcs.first().copied());
            t.push_str("    switch (c->pc) {\n");
            for e in &entries {
                let _ = writeln!(t, "    case UINT64_C({e:#x}): goto {};", label(*e));
            }
            if let Some(e) = first {
                let _ = writeln!(t, "    default: goto {};", label(e));
            }
            t.push_str("    }\n");
        }
    }
    let mut cx = Ctx {
        fe,
        f,
        targets,
        callees: BTreeSet::new(),
        hoisted,
        lp_sp: lp_reg.map(reg),
        split,
        io,
        clock,
    };
    let mut undefined = 0;
    let mut sizes = BTreeMap::new();
    for (k, &pc) in pcs.iter().enumerate() {
        let start = t.len();
        let _ = writeln!(t, "{}: {{", label(pc));
        let body = match (&f.insts[&pc], blocks.get(&pc)) {
            (Insn::Lifted { raw, .. }, Some(b)) => block(&mut cx, b, *raw),
            (Insn::Failed(e), _) => Err((e.insn, e.message.clone())),
            (Insn::Lifted { raw, .. }, None) => Err((*raw, "no block".into())),
        };
        match body {
            // A jump to the next instruction written falls through.
            Ok(s) => match pcs.get(k + 1) {
                Some(&n) => {
                    let goto = format!("    goto {};\n", label(n));
                    t.push_str(s.strip_suffix(&goto).unwrap_or(&s));
                }
                None => t.push_str(&s),
            },
            Err((insn, _)) => {
                undefined += 1;
                let _ = writeln!(t, "    RL_STORE();\n    return {};", fe.undefined(pc, insn));
            }
        }
        t.push_str("}\n");
        sizes.insert(pc, t.len() - start);
    }
    if lp_reg.is_some() {
        t.push_str("lp_dispatch:\n    switch (c->pend.pc) {\n");
        for r in resumes {
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
    Written {
        text: t,
        callees: cx.callees,
        undefined,
        flags: if lp_reg.is_some() {
            "RL_FN_MAY_UNWIND | RL_FN_LANDING_PADS"
        } else {
            "RL_FN_MAY_UNWIND"
        },
        sizes,
    }
}

/// The body of one instruction, or the raw word and reason it cannot be
/// emitted.
fn block(cx: &mut Ctx<'_>, b: &Block, raw: u64) -> Result<String, (u64, String)> {
    let mut s = String::new();
    let mut sets = Vec::new();
    let inl = inlined(b);
    let val = |v: Value| inl[v.index()].clone().unwrap_or_else(|| val(v));
    for (i, inst) in b.insts.iter().enumerate() {
        if inl[i].is_some() {
            continue;
        }
        let call = cx.clock.filter(|_| matches!(inst.op, Op::Intrinsic(..)));
        if let Some(clk) = call {
            let _ = writeln!(s, "    {clk}.budget = {};", time::BUDGET);
        }
        let emitted = emit_op(cx.fe, b, i, &inl, cx.io).map_err(|m| (raw, m))?;
        match emitted {
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
        if let Some(clk) = call {
            let _ = writeln!(s, "    {} = {clk}.budget;", time::BUDGET);
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
    if cx.clock.is_some() && matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)) {
        let next = match b.exit.next {
            Next::Const(t) => format!("UINT64_C({t:#x})"),
            Next::Select {
                cond,
                taken,
                not_taken,
            } => format!(
                "({} ? UINT64_C({taken:#x}) : UINT64_C({not_taken:#x}))",
                val(cond)
            ),
            Next::Dynamic(v) => format!("(uint64_t){}", val(v)),
        };
        s.push_str(&time::sync(&next));
    }
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
                        let _ = writeln!(s, "    case UINT64_C({t:#x}): {}", cx.jump(*t));
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
