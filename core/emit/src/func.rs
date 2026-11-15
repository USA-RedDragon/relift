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
use relift_ir::{BinOp, Block, Flow, IoMap, Next, Op, Reg, Value};

use crate::state::{Plan, Regs, plan};
use crate::time;
use crate::value::{Emitted, cty, emit_op, inlined, reg, val};
use crate::{Function, Insn, Options, Targets};

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
    /// Each instruction other than the entry, with the C function that
    /// enters there by `c->pc`, when every instruction is an entry.
    pub(crate) inner: Vec<(u64, String)>,
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
    targets: &'a Targets<'a>,
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
    /// Whether the registers stay in their homes ([`Options::regs_in_context`]).
    in_context: bool,
    /// The piece's stores and reloads.
    plan: &'a Plan,
    /// The registers that may be dirty where the code being written runs.
    dirty: Regs,
    /// The registers the piece writes.
    written: &'a BTreeSet<Reg>,
    /// The registers the piece has locals for.
    used: &'a BTreeSet<Reg>,
    /// The slow calls of the guards that share the slow path, with the
    /// address each continues at.
    slow: Vec<(String, u64)>,
}

/// The C local holding whether hoisted guard register `r` is nonzero.
fn flag(r: Reg) -> String {
    format!("g{}", reg(r))
}

/// Whether a transfer to `t` stays in `f`: analysis puts in a function's
/// extent only code it owns, which may include an address that is also the
/// entry of another function (an address-taken label inside it), and a jump
/// there must keep the function's state rather than leave it.
fn inside(f: &Function<'_>, _targets: &Targets<'_>, t: u64) -> bool {
    f.insts.contains_key(&t)
}

impl Ctx<'_> {
    fn inside(&self, t: u64) -> bool {
        inside(self.f, self.targets, t)
    }

    /// The statements that store the registers of `set` the piece writes
    /// (and the budget) to the context.
    fn store(&self, set: &Regs) -> String {
        let mut t = String::new();
        if let Some(clk) = self.clock {
            let _ = write!(t, "{clk}.budget = {}; ", time::BUDGET);
        }
        let mut words: BTreeMap<String, Vec<(u32, Reg)>> = BTreeMap::new();
        for &r in self
            .written
            .iter()
            .filter(|r| !self.in_context && set.contains(**r))
        {
            match self.fe.home(r) {
                Home::Field(lv) => {
                    let _ = write!(t, "{lv} = {}; ", reg(r));
                }
                Home::Flag { word, bit } => words.entry(word).or_default().push((bit, r)),
            }
        }
        for (word, bits) in words {
            let mask: u64 = bits.iter().map(|(b, _)| 1u64 << b).sum();
            let _ = write!(t, "{word} = ({word} & ~{mask:#x}u)");
            for (bit, r) in bits {
                let _ = write!(t, " | ((uint32_t){} << {bit})", reg(r));
            }
            t.push_str("; ");
        }
        t.trim_end().to_string()
    }

    /// The statements that reload the registers of `set` from the context.
    fn reload(&self, set: &Regs) -> String {
        if self.in_context {
            let mut flags = self.plan.none().clone();
            self.hoisted.iter().for_each(|&r| flags.insert(r));
            return reload(self.fe, self.clock, &self.hoisted, &BTreeSet::new(), &flags);
        }
        reload(self.fe, self.clock, &self.hoisted, self.used, set)
    }

    /// `s` as an indented line of its own, or nothing when it is empty.
    fn line(s: &str, indent: &str) -> String {
        if s.is_empty() {
            String::new()
        } else {
            format!("{indent}{s}\n")
        }
    }

    /// A transfer to the constant `t` that leaves no frame behind.
    fn jump(&mut self, t: u64) -> String {
        if self.inside(t) {
            if let Some((s, k)) = self.split {
                let to = s.piece_of[&t];
                if to != k {
                    return format!(
                        "{}c->pc = UINT64_C({t:#x}); RL_MUSTTAIL return {}(c, mem);",
                        self.lead(),
                        s.names[to]
                    );
                }
            }
            return format!("goto {};", label(t));
        }
        if let Some(target) = self.targets.get(&t) {
            self.callees.insert(t);
            return format!("{}RL_MUSTTAIL return {}(c, mem);", self.lead(), target.name);
        }
        format!(
            "{}RL_MUSTTAIL return rl_indirect(c, UINT64_C({t:#x}))(c, mem);",
            self.lead()
        )
    }

    /// The stores of the dirty registers before a jump on the same line.
    fn lead(&self) -> String {
        let s = self.store(&self.dirty);
        if s.is_empty() { s } else { s + " " }
    }
}

/// The block as the emitter writes it: constants folded, operations that
/// give an operand unchanged dropped, and without the values nothing
/// observable uses.
/// The test hook of `Options::test_fault`: the first add, in address order,
/// becomes a subtract.
fn miscompile_first_add(blocks: &mut BTreeMap<u64, Block>) {
    let hit = blocks.values_mut().find_map(|b| {
        b.insts.iter_mut().find_map(|i| match &mut i.op {
            Op::Bin(op @ BinOp::Add, ..) => Some(op),
            _ => None,
        })
    });
    if let Some(op) = hit {
        *op = BinOp::Sub;
    }
}

fn prepare(b: &Block) -> Block {
    relift_ir::dce(&relift_ir::simplify(b))
}

/// The registers live after each instruction of `f`, by address, as a bit
/// set over the register file (bit `r % 64` of word `r / 64`), by backward
/// dataflow over the function's instructions.
///
/// Every register counts as live wherever the C stores the context: at
/// calls, traps, returns, tail calls, indirect jumps, guards, sync points
/// and instructions that do not lift. So a register is dead after an
/// instruction only when every path redefines it before it is read or
/// stored, and dropping its write there changes nothing observable.
fn live_out(
    f: &Function<'_>,
    targets: &Targets<'_>,
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
    let mut synced = vec![false; n];
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
                Op::Sync => synced[k] = true,
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
            _ if synced[k] => o.copy_from_slice(&all),
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
    targets: &Targets<'_>,
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
fn resume_points(f: &Function<'_>, targets: &Targets<'_>) -> BTreeSet<u64> {
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
/// about that size. With `anywhere` (the mask of instruction addresses),
/// every instruction is an entry: the function is written as a stub and
/// pieces, and each piece switches to any of its instructions. An address
/// with bits outside the mask is a continuation of the instruction at its
/// masked address, kept in that instruction's piece and not an entry.
pub(crate) fn emit_function(
    fe: &dyn Frontend,
    f: &Function<'_>,
    targets: &Targets<'_>,
    opts: &Options,
) -> Emitted1 {
    let piece_bytes = opts.piece_bytes.max(1);
    let io = &opts.io_map;
    let mask = opts
        .enter_anywhere
        .filter(|_| f.insts.contains_key(&f.entry));
    let anywhere = mask.is_some();
    let resumes = resume_points(f, targets);
    let mut blocks: BTreeMap<u64, Block> = f
        .insts
        .iter()
        .filter_map(|(&pc, i)| match i {
            Insn::Lifted { block, .. } => Some((pc, prepare(block))),
            Insn::Failed(_) => None,
        })
        .collect();
    if opts.test_fault == Some(f.entry) {
        miscompile_first_add(&mut blocks);
    }
    let blocks = drop_dead_sets(fe, f, targets, blocks);
    let all: Vec<u64> = f.insts.keys().copied().collect();
    let env = Env {
        fe,
        f,
        targets,
        blocks: &blocks,
        resumes: &resumes,
        io,
        anywhere,
        in_context: opts.regs_in_context,
    };
    let whole = emit_piece(&env, &all, None);
    if !anywhere && (whole.text.len() <= piece_bytes || all.len() < 2) {
        return Emitted1 {
            pieces: vec![Piece {
                name: f.name.clone(),
                text: whole.text,
                first_pc: None,
            }],
            callees: whole.callees,
            undefined: whole.undefined,
            flags: whole.flags,
            inner: Vec::new(),
        };
    }
    // Divide in address order at instruction boundaries, by the size each
    // instruction took in the whole function.
    let mut piece_of = BTreeMap::new();
    let (mut k, mut size) = (0, 0);
    let base = |pc: u64| mask.map_or(pc, |m| pc & m);
    for pc in all.iter().filter(|&&pc| base(pc) == pc) {
        let n = whole.sizes.get(pc).copied().unwrap_or(0);
        if size > 0 && size + n > piece_bytes {
            k += 1;
            size = 0;
        }
        size += n;
        piece_of.insert(*pc, k);
    }
    for pc in all.iter().filter(|&&pc| base(pc) != pc) {
        let to = piece_of.get(&base(*pc)).copied().unwrap_or(k);
        piece_of.insert(*pc, to);
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
    let inner = if anywhere {
        all.iter()
            .filter(|&&pc| pc != f.entry && base(pc) == pc)
            .map(|pc| (*pc, split.names[split.piece_of[pc]].clone()))
            .collect()
    } else {
        Vec::new()
    };
    Emitted1 {
        pieces,
        callees,
        undefined,
        flags,
        inner,
    }
}

/// What every piece of a function is emitted from.
#[derive(Clone, Copy)]
pub(crate) struct Env<'a> {
    pub(crate) fe: &'a dyn Frontend,
    pub(crate) f: &'a Function<'a>,
    targets: &'a Targets<'a>,
    pub(crate) blocks: &'a BTreeMap<u64, Block>,
    resumes: &'a BTreeSet<u64>,
    io: &'a IoMap,
    anywhere: bool,
    in_context: bool,
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
        anywhere,
        in_context,
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
    // The instructions the piece is entered at with the context up to date.
    let entries: BTreeSet<u64> = match split {
        None => [f.entry].into(),
        Some((s, k)) => {
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
            if anywhere {
                entries.extend(own.iter().copied());
            }
            entries
        }
    };
    let plan = plan(env, pcs, clock.is_some(), lp_reg);
    let mut load = Regs::new(regs.len());
    for &e in &entries {
        load.union(plan.live_at(e));
    }
    let mut t = String::new();
    let _ = writeln!(t, "\nRL_TEXT RL_LIFTED({name})\n{{");
    for &r in &used {
        let ty = cty(regs.width(r));
        let _ = match (fe.home(r), in_context) {
            (Home::Field(lv), true) => writeln!(t, "#define {} ({lv})", reg(r)),
            (Home::Flag { word, bit }, true) => {
                writeln!(t, "#define {} (({ty})(({word} >> {bit}) & 1u))", reg(r))
            }
            _ if !load.contains(r) => writeln!(t, "    {ty} {};", reg(r)),
            (Home::Field(lv), false) => writeln!(t, "    {ty} {} = {lv};", reg(r)),
            (Home::Flag { word, bit }, false) => {
                writeln!(t, "    {ty} {} = ({ty})(({word} >> {bit}) & 1u);", reg(r))
            }
        };
    }
    for &r in &hoisted {
        if in_context || load.contains(r) {
            let _ = writeln!(t, "    int {} = {} != 0;", flag(r), reg(r));
        } else {
            let _ = writeln!(t, "    int {};", flag(r));
        }
    }
    if let Some(clk) = clock {
        let _ = writeln!(t, "    int64_t {} = {clk}.budget;", time::BUDGET);
    }
    t.push_str("    int rc;\n    (void)rc;\n");
    if plan.shared().is_some() {
        t.push_str("    unsigned rl_k = 0;\n");
    }
    match split {
        None => {
            let _ = writeln!(t, "    goto {};", label(f.entry));
        }
        Some(_) => {
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
        in_context,
        dirty: Regs::new(regs.len()),
        plan: &plan,
        written: &written,
        used: &used,
        slow: Vec::new(),
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
                let st = cx.store(plan.dirty_in(pc));
                let _ = writeln!(
                    t,
                    "{}    return {};",
                    Ctx::line(&st, "    "),
                    fe.undefined(pc, insn)
                );
            }
        }
        t.push_str("}\n");
        sizes.insert(pc, t.len() - start);
    }
    if let Some((store, reload)) = plan.shared() {
        let slow = std::mem::take(&mut cx.slow);
        let st = cx.store(store);
        let rl = cx.reload(reload);
        let _ = writeln!(
            t,
            "rl_slow:\n{}    switch (rl_k) {{",
            Ctx::line(&st, "    ")
        );
        for (k, (call, _)) in slow.iter().enumerate() {
            let _ = writeln!(t, "    case {k}: rc = {call}; break;");
        }
        let _ = writeln!(
            t,
            "    default: __builtin_unreachable();\n    }}\n    if (RL_UNLIKELY(rc))\n        return RL_PENDING;\n{}    switch (rl_k) {{",
            Ctx::line(&rl, "    ")
        );
        cx.dirty = Regs::new(regs.len());
        for (k, (_, to)) in slow.iter().enumerate() {
            let _ = writeln!(t, "    case {k}: {}", cx.jump(*to));
        }
        t.push_str("    default: __builtin_unreachable();\n    }\n");
    }
    if lp_reg.is_some() {
        t.push_str("lp_dispatch:\n    switch (c->pend.pc) {\n");
        for r in resumes {
            cx.dirty = Regs::new(regs.len());
            let next = cx.jump(*r);
            let rl = cx.reload(plan.live_at(*r));
            let _ = writeln!(
                t,
                "    case UINT64_C({r:#x}):\n        c->pend.kind = RL_PEND_NONE;\n{}        {next}",
                Ctx::line(&rl, "        ")
            );
        }
        let _ = writeln!(
            t,
            "    default:\n        break;\n    }}\n    rl_bad_resume(c, mem, UINT64_C({:#x}));",
            f.entry
        );
    }
    for &r in used.iter().filter(|_| in_context) {
        let _ = writeln!(t, "#undef {}", reg(r));
    }
    t.push_str("}\n");
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

/// Whether the exit of `b` may jump back to its own or an earlier address
/// in the function: every loop has such an edge, and its fence makes the C
/// reload guest memory on each turn (docs/abi.md section 9).
fn loops_back(cx: &Ctx<'_>, b: &Block) -> bool {
    let back = |t: u64| t <= b.pc && cx.inside(t);
    match (b.exit.flow, b.exit.next) {
        (Flow::Jump, Next::Const(t)) => back(t),
        (
            Flow::Jump,
            Next::Select {
                taken, not_taken, ..
            },
        ) => back(taken) || back(not_taken),
        _ => false,
    }
}

/// Whether the exit of `b` is a preemption point: a jump back inside the
/// function that closes a loop holding a call, a trap, a computed jump or
/// another loop, or a loop that waits on memory ([`waits`]); a computed
/// jump; or a jump or call to its own or a lower address outside the
/// function (recursion, a tail call back). Other loops (a copy, a hash, a
/// counted search) have no check of their own, so clang keeps unrolling
/// and vectorizing them: such a loop delays a preemption by one run of it,
/// until the loop or function around it reaches a check.
fn preempts(cx: &Ctx<'_>, b: &Block) -> bool {
    let back = |t: u64| t <= b.pc;
    let jump = |t: u64| {
        if !back(t) {
            false
        } else if cx.inside(t) {
            loop_has_work(cx, t, b.pc) || waits(cx, t, b)
        } else {
            true
        }
    };
    match (b.exit.flow, b.exit.next) {
        (Flow::Return | Flow::Trap(_), _) => false,
        (Flow::Call { .. }, Next::Const(t)) => back(t),
        (Flow::Call { .. }, _) => false,
        (Flow::Jump, Next::Const(t)) => jump(t),
        (
            Flow::Jump,
            Next::Select {
                taken, not_taken, ..
            },
        ) => jump(taken) || jump(not_taken),
        (Flow::Jump, Next::Dynamic(_)) => true,
    }
}

/// Whether the loop from `from` to its jump back `b` waits on memory: the
/// jump back or a branch out of the loop decides on a value that comes,
/// through the loop's instructions before it in address order, from a load
/// at an address the loop does not change (it reads one word again and
/// again), an exclusive store's status or an intrinsic (a clock read).
/// Such a loop may wait for a thread that needs its core (a spin-wait). A
/// loop that leaves on a value it loads from moving addresses (a compare,
/// a search) does not wait.
fn waits(cx: &Ctx<'_>, from: u64, b: &Block) -> bool {
    let regs = |blk: &Block, set: bool| -> BTreeSet<Reg> {
        blk.insts
            .iter()
            .filter_map(|i| match i.op {
                Op::Get(r) if !set => Some(r),
                Op::Set(r, _) if set => Some(r),
                _ => None,
            })
            .collect()
    };
    let mut written = BTreeSet::new();
    for (&pc, i) in cx.f.insts.range(from..=b.pc) {
        match i {
            Insn::Lifted { block: p, .. } => {
                written.extend(regs(if pc == b.pc { b } else { p }, true))
            }
            Insn::Failed(_) => return true,
        }
    }
    let mut needed = BTreeSet::new();
    for (&pc, i) in cx.f.insts.range(from..=b.pc).rev() {
        let Insn::Lifted { block: p, .. } = i else {
            return true;
        };
        let p = if pc == b.pc { b } else { p };
        let sets = regs(p, true);
        if !sets.is_disjoint(&needed) {
            let fixed = || regs(p, false).is_disjoint(&written);
            let source = p.insts.iter().any(|i| match i.op {
                Op::StoreExclusive { .. } | Op::Intrinsic(..) => true,
                Op::Load { .. } | Op::LoadExclusive { .. } => fixed(),
                _ => false,
            });
            if source {
                return true;
            }
            needed.retain(|r| !sets.contains(r));
            needed.extend(regs(p, false));
        }
        let leaves = |t: u64| t < from || t > b.pc;
        if let Next::Select {
            taken, not_taken, ..
        } = p.exit.next
            && (pc == b.pc || leaves(taken) || leaves(not_taken))
        {
            needed.extend(regs(p, false));
        }
    }
    false
}

/// Whether the instructions from `from` to the jump back at `to` hold a
/// call, a trap, a computed jump or another jump back.
fn loop_has_work(cx: &Ctx<'_>, from: u64, to: u64) -> bool {
    cx.f.insts.range(from..=to).any(|(&pc, i)| {
        let Insn::Lifted { block: b, .. } = i else {
            return true;
        };
        let back = |t: u64| t <= pc && cx.inside(t);
        match (b.exit.flow, b.exit.next) {
            (Flow::Call { .. } | Flow::Trap(_), _) | (_, Next::Dynamic(_)) => true,
            (Flow::Return, _) => false,
            _ if pc == to => false,
            (Flow::Jump, Next::Const(t)) => back(t),
            (
                Flow::Jump,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => back(taken) || back(not_taken),
        }
    })
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
                if let Some(to) = cx.fe.guard_exit(code, b.pc, raw) {
                    let _ = writeln!(
                        s,
                        "    if (RL_UNLIKELY({cond})) {{\n        RL_STORE();\n        RL_MUSTTAIL return {to}(c, mem);\n    }}"
                    );
                    continue;
                }
                let slow = cx
                    .fe
                    .guard_slow(code, b.pc, raw)
                    .unwrap_or_else(|| cx.fe.undefined(b.pc, raw));
                let to = b.pc + u64::from(b.len);
                if cx.plan.shared().is_some() {
                    let k = cx.slow.len();
                    cx.slow.push((slow, to));
                    let _ = writeln!(
                        s,
                        "    if (RL_UNLIKELY({cond})) {{ rl_k = {k}; goto rl_slow; }}"
                    );
                    continue;
                }
                let st = cx.store(cx.plan.dirty_in(b.pc));
                let rl = cx.reload(cx.plan.live_at(to));
                let saved = std::mem::replace(&mut cx.dirty, cx.plan.none().clone());
                let next = cx.jump(to);
                cx.dirty = saved;
                let _ = writeln!(
                    s,
                    "    if (RL_UNLIKELY({cond})) {{\n{}        rc = {slow};\n        if (RL_UNLIKELY(rc))\n            return RL_PENDING;\n{}        {next}\n    }}",
                    Ctx::line(&st, "        "),
                    Ctx::line(&rl, "        ")
                );
            }
        }
        if let Some(clk) = call {
            let _ = writeln!(s, "    {} = {clk}.budget;", time::BUDGET);
        }
    }
    for (r, e) in sets {
        let _ = match cx.fe.home(r) {
            Home::Flag { word, bit } if cx.in_context => writeln!(
                s,
                "    {word} = ({word} & ~(1u << {bit})) | ((uint32_t)({e}) << {bit});"
            ),
            _ => writeln!(s, "    {} = {e};", reg(r)),
        };
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
    cx.dirty = cx.plan.dirty_out(b.pc);
    let out_store = cx.store(&cx.dirty);
    let after = |cx: &mut Ctx<'_>, s: &mut String, call: String, to: u64| {
        let rl = cx.reload(cx.plan.live_at(to));
        let _ = writeln!(
            s,
            "{}    rc = {call};\n{check}\n{}",
            Ctx::line(&out_store, "    "),
            Ctx::line(&rl, "    ").trim_end_matches('\n')
        );
        if s.ends_with("\n\n") {
            s.pop();
        }
        cx.dirty = cx.plan.none().clone();
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
        let rl = cx.reload(cx.plan.exit_live(b.pc));
        s.push_str(&time::sync(cx.fe, b, &next, &out_store, &rl));
    }
    if let Some(clk) = cx.fe.preempt_clock().filter(|_| preempts(cx, b)) {
        s.push_str(&time::preempt(clk));
    }
    if loops_back(cx, b) {
        s.push_str("    RL_LOOP_FENCE();\n");
    }
    match b.exit.flow {
        Flow::Return => {
            s.push_str(&Ctx::line(&out_store, "    "));
            s.push_str("    return RL_OK;\n");
        }
        Flow::Call { ret } => {
            let call = match b.exit.next {
                Next::Const(t) if let Some(target) = cx.targets.get(&t) => {
                    cx.callees.insert(t);
                    format!("{}(c, mem)", target.name)
                }
                Next::Const(t) => format!("rl_indirect(c, UINT64_C({t:#x}))(c, mem)"),
                Next::Dynamic(v) => format!("rl_indirect(c, {})(c, mem)", val(v)),
                Next::Select { .. } => return Err((raw, "a conditional call".into())),
            };
            after(cx, &mut s, call, ret);
            let _ = writeln!(s, "    {}", cx.jump(ret));
        }
        Flow::Trap(code) => {
            let Next::Const(t) = b.exit.next else {
                return Err((raw, "a trap with a computed resume".into()));
            };
            let trap = cx.fe.trap(code);
            after(cx, &mut s, trap, t);
            let _ = writeln!(s, "    {}", cx.jump(t));
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
                    "{}    RL_MUSTTAIL return rl_indirect(c, {})(c, mem);",
                    Ctx::line(&out_store, "    "),
                    val(v)
                );
            }
        },
    }
    Ok(s)
}

/// The statements that reload the registers of `set` (of the piece's
/// locals `used`) from the context, with the budget and the hoisted guard
/// flags of the registers reloaded.
fn reload(
    fe: &dyn Frontend,
    clock: Option<&'static str>,
    hoisted: &BTreeSet<Reg>,
    used: &BTreeSet<Reg>,
    set: &Regs,
) -> String {
    let regs = fe.regs();
    let mut t = String::new();
    if let Some(clk) = clock {
        let _ = write!(t, "{} = {clk}.budget; ", time::BUDGET);
    }
    for &r in used.iter().filter(|r| set.contains(**r)) {
        let ty = cty(regs.width(r));
        let _ = match fe.home(r) {
            Home::Field(lv) => write!(t, "{} = {lv}; ", reg(r)),
            Home::Flag { word, bit } => write!(t, "{} = ({ty})(({word} >> {bit}) & 1u); ", reg(r)),
        };
    }
    for &r in hoisted.iter().filter(|r| set.contains(**r)) {
        let _ = write!(t, "{} = {} != 0; ", flag(r), reg(r));
    }
    t.trim_end().to_string()
}
