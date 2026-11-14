//! The staged evaluator: walks the Sail semantics of one encoding with the
//! fixed bits concrete, the open bits static and the guest state dynamic,
//! and writes the Rust body of the encoding's template.
//!
//! Control flow on concrete values is folded. Control flow on static or
//! dynamic values runs both arms under a guard and merges their locals with
//! selects, so the emitted code is straight-line; side effects carry the
//! guard (a static guard as a Rust `if`, a dynamic one as an argument to the
//! state helper). A static value needed as a concrete one (a width, a loop
//! bound) stops the run with [`E::Split`]; the caller then fixes those bits
//! and runs once per value.

use std::collections::{BTreeSet, HashMap};
use std::rc::Rc;

use crate::ast::{Arm, Dir, Expr, Lit, Pat, Stmt, Typ, TypArg};
use crate::model::Model;
use crate::ops::{BOp, Cmp, IOp};
use crate::value::{K, Kind, V, mask};

/// Why a run stopped.
#[derive(Clone, Debug)]
pub enum E {
    /// These instruction bits must be concrete.
    Split(u32),
    /// The semantics use something the generator does not handle.
    Fail(String),
    /// An unconditional exception; the payload is the `LiftError` variant.
    Throw(&'static str),
    /// An unconditional `Error_See`: the word belongs to a later decode
    /// clause, which the Sail decoder retries.
    See,
    /// A call's arguments do not fit this overload.
    NoMatch,
}

/// A run's result.
pub type R<T> = Result<T, E>;

/// A failure with a message.
pub fn err(m: impl Into<String>) -> E {
    E::Fail(m.into())
}

/// Fails with a message.
pub fn fail<T>(m: impl Into<String>) -> R<T> {
    Err(err(m))
}

/// The condition under which the current path runs.
#[derive(Clone, Debug, PartialEq)]
pub struct Guard {
    /// The path never runs.
    pub dead: bool,
    /// A static condition, as a Rust `bool` expression.
    pub s: Option<(Rc<str>, u32)>,
    /// A dynamic condition, as a one-bit IR value.
    pub d: Option<Rc<str>>,
}

impl Guard {
    fn always() -> Guard {
        Guard {
            dead: false,
            s: None,
            d: None,
        }
    }

    fn never() -> Guard {
        Guard {
            dead: true,
            s: None,
            d: None,
        }
    }
}

/// The evaluator for one encoding and one assignment of its split bits.
pub struct Gen<'m> {
    /// The model.
    pub m: &'m Model,
    out: Vec<String>,
    n: usize,
    locals: HashMap<String, V>,
    scopes: Vec<Vec<(String, Option<V>)>>,
    /// The current path condition.
    pub guard: Guard,
    frames: Vec<Vec<(Guard, V)>>,
    regs: HashMap<String, V>,
    depth: usize,
    steps: usize,
    /// Instruction bits known while generating.
    pub known: u32,
    /// Their values.
    pub value: u32,
    /// The cut functions this run used.
    pub used_cut: BTreeSet<&'static str>,
    /// The next load arms the exclusive monitor for this many bytes.
    pub excl_load: Option<u32>,
    /// The bytes of an exclusive load not yet read: the IR value, its
    /// width in bits and the next bit to read.
    pub excl_rest: Option<(Rc<str>, u32, u32)>,
    /// The pending exclusive-store result, named before the store defines it.
    pub excl_pass: Option<Rc<str>>,
    /// Run the cipher functions from Sail instead of cutting them (to
    /// check the cut).
    pub no_cut: bool,
}

/// Features every configuration has: Armv8.0-A with floating point and
/// Advanced SIMD, AArch64 at every exception level.
const BASE_FEATURES: &[&str] = &[
    "FEAT_AdvSIMD",
    "FEAT_FP",
    "FEAT_AA64EL0",
    "FEAT_AA64EL1",
    "FEAT_AA64EL2",
    "FEAT_AA64EL3",
    "FEAT_EL0",
    "FEAT_EL1",
    "FEAT_EL2",
    "FEAT_EL3",
    "FEAT_AA32EL0",
];

/// Optional features a template may test at lift time (`St::has`): the
/// user-visible instruction-set extensions relift lifts. Every other
/// feature is absent in every configuration.
const LIFT_TIME_FEATURES: &[&str] = &[
    "FEAT_AES",
    "FEAT_PMULL",
    "FEAT_SHA1",
    "FEAT_SHA256",
    "FEAT_SHA512",
    "FEAT_SHA3",
    "FEAT_SM3",
    "FEAT_SM4",
    "FEAT_CRC32",
    "FEAT_LSE",
    "FEAT_LSE128",
    "FEAT_RDM",
    "FEAT_FP16",
    "FEAT_FHM",
    "FEAT_DotProd",
    "FEAT_I8MM",
    "FEAT_BF16",
    "FEAT_FCMA",
    "FEAT_JSCVT",
    "FEAT_FRINTTS",
    "FEAT_FlagM",
    "FEAT_FlagM2",
    "FEAT_LRCPC",
    "FEAT_LRCPC2",
    "FEAT_LRCPC3",
    "FEAT_CSSC",
    "FEAT_HBC",
    "FEAT_SB",
];

/// How a template sees a feature: `Some(true)` for a base feature,
/// `Some(false)` for one no configuration has, `None` for one tested at
/// lift time.
pub fn feature(name: &str) -> Option<bool> {
    if BASE_FEATURES.contains(&name) {
        return Some(true);
    }
    if LIFT_TIME_FEATURES.contains(&name) {
        return None;
    }
    Some(false)
}

/// Stubs of `stubs.sail` whose empty body is right for a user-mode guest:
/// hints with no architectural effect.
const STUBS_OK: &[&str] = &[
    "Hint_Yield",
    "Hint_PreloadData",
    "Hint_PreloadDataForWrite",
    "Hint_PreloadInstr",
];

impl<'m> Gen<'m> {
    /// An evaluator with `known` bits of the word fixed to `value`.
    pub fn new(m: &'m Model, known: u32, value: u32) -> Gen<'m> {
        Gen {
            m,
            out: Vec::new(),
            n: 0,
            locals: HashMap::new(),
            scopes: vec![Vec::new()],
            guard: Guard::always(),
            frames: Vec::new(),
            regs: HashMap::new(),
            depth: 0,
            steps: 0,
            known,
            value,
            used_cut: BTreeSet::new(),
            excl_load: None,
            excl_rest: None,
            excl_pass: None,
            no_cut: false,
        }
    }

    /// The emitted lines.
    pub fn take(self) -> Vec<String> {
        self.out
    }

    /// A fresh local number.
    pub fn fresh(&mut self) -> usize {
        self.n += 1;
        self.n
    }

    /// Emits one line.
    pub fn line(&mut self, l: String) {
        self.out.push(l);
    }

    /// Whether `e` names the instruction word.
    pub fn is_word(&self, e: &str) -> bool {
        e == "w"
    }

    /// The word's bits `lo .. lo + w`: concrete where known, else static.
    pub fn word_bits(&mut self, lo: u32, w: u32) -> V {
        let m = (mask(w) as u32) << lo;
        if self.known & m == m {
            return V::Bits(w, u128::from((self.value & m) >> lo));
        }
        let e = format!("(w >> {lo}) & {:#x}", mask(w));
        self.st(K::Bits(w), e, m & !self.known)
    }

    /// The guard's dynamic part as a helper argument.
    pub fn gd(&self) -> String {
        match &self.guard.d {
            Some(d) => format!("Some({d})"),
            None => "None".into(),
        }
    }

    /// Emits a side effect under the current guard.
    pub fn effect(&mut self, stmt: String) {
        if self.guard.dead {
            return;
        }
        match &self.guard.s {
            Some((s, _)) => {
                let l = format!("if {s} {{ {stmt} }}");
                self.line(l);
            }
            None => self.line(stmt),
        }
    }

    /// An `Error_See` on this path: the template goes on with the next
    /// decode clause that takes the word, so a static condition splits the
    /// template on the bits it reads.
    fn see(&mut self) -> R<()> {
        if self.guard.dead {
            return Ok(());
        }
        if self.guard.d.is_some() {
            return fail("Error_See under a dynamic condition");
        }
        match &self.guard.s {
            None => Err(E::See),
            Some((_, deps)) => Err(E::Split(*deps)),
        }
    }

    /// Emits a lift-time exception under the current guard.
    pub fn throw(&mut self, kind: &'static str) -> R<()> {
        if self.guard.dead {
            return Ok(());
        }
        if self.guard.d.is_some() {
            return fail(format!("{kind} under a dynamic condition"));
        }
        if self.guard.s.is_none() {
            return Err(E::Throw(kind));
        }
        self.effect(format!("return Err(LiftError::{kind}(ENC));"));
        self.guard = Guard::never();
        Ok(())
    }

    fn guard_and(&mut self, g: &Guard, c: &V) -> R<Guard> {
        if g.dead {
            return Ok(g.clone());
        }
        let c = self.truth(c)?;
        let mut out = g.clone();
        match &c {
            V::Bool(true) => {}
            V::Bool(false) => return Ok(Guard::never()),
            V::S(_, e, d) => {
                out.s = Some(match &g.s {
                    None => (e.clone(), *d),
                    Some((s, sd)) => {
                        let v = self.st(K::Bool, format!("{s} && {e}"), sd | d);
                        match v {
                            V::S(_, e, d) => (e, d),
                            _ => unreachable!(),
                        }
                    }
                });
            }
            V::D(_, n) => {
                out.d = Some(match &g.d {
                    None => n.clone(),
                    Some(d) => match self.dy(K::Bool, format!("b.and({d}, {n})")) {
                        V::D(_, n) => n,
                        _ => unreachable!(),
                    },
                });
            }
            _ => return fail("guard"),
        }
        Ok(out)
    }

    fn guard_or(&mut self, a: &Guard, b: &Guard) -> R<Guard> {
        if a.dead {
            return Ok(b.clone());
        }
        if b.dead || a == b {
            return Ok(a.clone());
        }
        if a.s == b.s {
            let d = match (&a.d, &b.d) {
                (Some(x), Some(y)) => match self.dy(K::Bool, format!("b.or({x}, {y})")) {
                    V::D(_, n) => Some(n),
                    _ => unreachable!(),
                },
                _ => None,
            };
            return Ok(Guard {
                dead: false,
                s: a.s.clone(),
                d,
            });
        }
        if a.d == b.d {
            let s = match (&a.s, &b.s) {
                (Some((x, dx)), Some((y, dy))) => {
                    match self.st(K::Bool, format!("{x} || {y}"), dx | dy) {
                        V::S(_, e, d) => Some((e, d)),
                        _ => unreachable!(),
                    }
                }
                _ => None,
            };
            return Ok(Guard {
                dead: false,
                s,
                d: a.d.clone(),
            });
        }
        fail("guard union of mixed conditions")
    }

    /// `g ? a : b` for a guard.
    fn gsel(&mut self, g: &Guard, a: &V, b: &V) -> R<V> {
        if g.dead {
            return Ok(b.clone());
        }
        let mut v = a.clone();
        if let Some(d) = &g.d {
            v = self.select(&V::D(K::Bool, d.clone()), &v, b)?;
        }
        if let Some((s, sd)) = &g.s {
            v = self.select(&V::S(K::Bool, s.clone(), *sd), &v, b)?;
        }
        Ok(v)
    }

    fn bind(&mut self, name: &str, v: V) {
        let old = self.locals.insert(name.to_string(), v.clone());
        self.scopes
            .last_mut()
            .expect("scope")
            .push((name.to_string(), old));
        if let Some(n) = name.strip_prefix('\'') {
            let old = self.locals.insert(n.to_string(), v);
            self.scopes
                .last_mut()
                .expect("scope")
                .push((n.to_string(), old));
        }
    }

    fn push(&mut self) {
        self.scopes.push(Vec::new());
    }

    fn pop(&mut self) {
        for (name, old) in self.scopes.pop().expect("scope").into_iter().rev() {
            match old {
                None => {
                    self.locals.remove(&name);
                }
                Some(v) => {
                    self.locals.insert(name, v);
                }
            }
        }
    }

    fn lookup(&mut self, name: &str) -> R<V> {
        if let Some(v) = self.locals.get(name) {
            return Ok(v.clone());
        }
        if let Some(v) = self.locals.get(&format!("'{name}")) {
            return Ok(v.clone());
        }
        if let Some((ty, i)) = self.m.enum_of.get(name) {
            return Ok(V::Enum(ty.as_str().into(), *i));
        }
        if self.m.registers.contains_key(name)
            || self.m.lets.contains_key(name)
            || name == "SEE"
            || name == "__BranchTaken"
        {
            return self.reg_get(name);
        }
        fail(format!("unbound {name}"))
    }

    fn reg_get(&mut self, name: &str) -> R<V> {
        if let Some(v) = self.regs.get(name) {
            return Ok(v.clone());
        }
        let v = self.init_reg(name)?;
        self.regs.insert(name.to_string(), v.clone());
        Ok(v)
    }

    fn init_reg(&mut self, name: &str) -> R<V> {
        match name {
            "PSTATE" => {
                let fields = self
                    .m
                    .structs
                    .get("ProcState")
                    .ok_or_else(|| err("no ProcState"))?;
                let mut f = Vec::new();
                for (n, _) in fields {
                    let w = match n.as_str() {
                        "EL" | "BTYPE" => 2,
                        "GE" => 4,
                        "IT" => 8,
                        "M" => 5,
                        _ => 1,
                    };
                    f.push((n.clone(), V::Bits(w, 0)));
                }
                return Ok(V::Struct("ProcState".into(), Rc::new(f)));
            }
            "SEE" => return Ok(V::Int(-1)),
            "__ThisInstrEnc" => {
                let (t, i) = self.m.enum_of.get("__A64").ok_or_else(|| err("no __A64"))?;
                return Ok(V::Enum(t.as_str().into(), *i));
            }
            "__BranchTaken" | "SPESampleInFlight" | "InGuardedPage" => return Ok(V::Bool(false)),
            "BTypeNext" => return Ok(V::Bits(2, 0)),
            _ => {}
        }
        if let Some((_, Some(init))) = self.m.registers.get(name) {
            let init = init.clone();
            return self.eval_text(&init);
        }
        if let Some(t) = self.m.lets.get(name) {
            let t = t.clone();
            return self.eval_text(&t);
        }
        if let Some((ty, _)) = self.m.registers.get(name) {
            let ty = ty.trim().to_string();
            let cfg: u128 = match name {
                "SCR_EL3" => 1 | (1 << 10),
                "HCR_EL2" => 1 << 31,
                _ => 0,
            };
            if let Some((w, _)) = self.m.bitfields.get(&ty) {
                return Ok(V::Bf(ty.as_str().into(), Box::new(V::Bits(*w, cfg))));
            }
            if let Some(w) = ty
                .strip_prefix("bits(")
                .and_then(|s| s.strip_suffix(')'))
                .and_then(|s| s.parse::<u32>().ok())
            {
                return Ok(V::Bits(w, cfg));
            }
        }
        fail(format!("unconfigured register {name}"))
    }

    fn eval_text(&mut self, t: &str) -> R<V> {
        let e = crate::parse::parse_expr(t).map_err(err)?;
        self.eval(&e, None)
    }

    fn assign_name(&mut self, name: &str, v: V) -> R<()> {
        if self.locals.contains_key(name) {
            self.locals.insert(name.to_string(), v);
            return Ok(());
        }
        if self.m.registers.contains_key(name)
            || matches!(name, "SEE" | "__BranchTaken" | "BTypeNext")
        {
            self.regs.insert(name.to_string(), v);
            return Ok(());
        }
        self.bind(name, v);
        Ok(())
    }

    fn run_branch(
        &mut self,
        cond: &V,
        then: &mut dyn FnMut(&mut Self) -> R<V>,
        other: &mut dyn FnMut(&mut Self) -> R<V>,
    ) -> R<V> {
        let g0 = self.guard.clone();
        let (l0, r0) = (self.locals.clone(), self.regs.clone());
        self.guard = self.guard_and(&g0, cond)?;
        let gt0 = self.guard.clone();
        let vt = if self.guard.dead {
            V::Undef(None)
        } else {
            then(self)?
        };
        let gt = self.guard.clone();
        let (lt, rt) = (
            std::mem::replace(&mut self.locals, l0.clone()),
            std::mem::replace(&mut self.regs, r0.clone()),
        );
        self.guard = g0.clone();
        let nc = self.not(cond)?;
        self.guard = self.guard_and(&g0, &nc)?;
        let ge0 = self.guard.clone();
        let ve = if self.guard.dead {
            V::Undef(None)
        } else {
            other(self)?
        };
        let ge = self.guard.clone();
        let (le, re) = (
            std::mem::take(&mut self.locals),
            std::mem::take(&mut self.regs),
        );
        if gt.dead && ge.dead {
            self.locals = le;
            self.regs = re;
            self.guard = Guard::never();
            return Ok(V::Undef(None));
        }
        if gt.dead {
            self.locals = le;
            self.regs = re;
            self.guard = ge;
            return Ok(ve);
        }
        if ge.dead {
            self.locals = lt;
            self.regs = rt;
            self.guard = gt;
            return Ok(vt);
        }
        let mut ml = HashMap::new();
        let mut names: Vec<&String> = l0.keys().collect();
        names.sort();
        for k in names {
            let v0 = &l0[k];
            let a = lt.get(k).unwrap_or(v0);
            let d = le.get(k).unwrap_or(v0);
            let v = if a == d {
                a.clone()
            } else {
                self.select(cond, a, d)?
            };
            ml.insert(k.clone(), v);
        }
        let mut keys: Vec<String> = rt.keys().chain(re.keys()).cloned().collect();
        keys.sort();
        keys.dedup();
        let mut mr = HashMap::new();
        for k in keys {
            let a = match rt.get(&k).or(r0.get(&k)) {
                Some(v) => v.clone(),
                None => self.init_reg(&k)?,
            };
            let d = match re.get(&k).or(r0.get(&k)) {
                Some(v) => v.clone(),
                None => self.init_reg(&k)?,
            };
            let v = if a == d {
                a
            } else {
                self.select(cond, &a, &d)?
            };
            mr.insert(k, v);
        }
        self.locals = ml;
        self.regs = mr;
        self.guard = if gt == gt0 && ge == ge0 {
            g0
        } else {
            self.guard_or(&gt, &ge)?
        };
        match self.select(cond, &vt, &ve) {
            Ok(v) => Ok(v),
            Err(E::Fail(_)) => Ok(V::Undef(None)),
            Err(e) => Err(e),
        }
    }

    /// Evaluates an expression; `hint` is the expected width, when known.
    pub fn eval(&mut self, e: &Expr, hint: Option<u32>) -> R<V> {
        if self.guard.dead {
            return Ok(V::Undef(None));
        }
        self.steps += 1;
        if self.steps > 2_000_000 {
            return fail("step budget");
        }
        match e {
            Expr::Lit(l) => Ok(lit(l)),
            Expr::Id(n) => self.lookup(n),
            Expr::Undefined => Ok(V::Undef(hint)),
            Expr::Typed(x, t) => {
                let w = self.type_width(t);
                self.eval(x, w.or(hint))
            }
            Expr::Tuple(xs) => {
                let mut out = Vec::new();
                for x in xs {
                    out.push(self.eval(x, None)?);
                }
                Ok(V::tuple(out))
            }
            Expr::Block(ss) => {
                self.push();
                let mut v = V::Unit;
                let mut r = Ok(());
                for s in ss {
                    if self.guard.dead {
                        break;
                    }
                    match self.stmt(s) {
                        Ok(x) => v = x,
                        Err(e) => {
                            r = Err(e);
                            break;
                        }
                    }
                }
                self.pop();
                r.map(|_| v)
            }
            Expr::LetIn(p, x, body) => {
                let hint = self.pat_hint(p);
                let v = self.eval(x, hint)?;
                self.push();
                let r = self.bind_pat(p, v).and_then(|_| self.eval(body, None));
                self.pop();
                r
            }
            Expr::If(c, t, el) => {
                let c = self.eval(c, None)?;
                let c = self.truth(&c)?;
                match c {
                    V::Bool(true) => self.eval(t, hint),
                    V::Bool(false) => match el {
                        Some(x) => self.eval(x, hint),
                        None => Ok(V::Unit),
                    },
                    _ => self.run_branch(&c, &mut |g| g.eval(t, hint), &mut |g| match el {
                        Some(x) => g.eval(x, hint),
                        None => Ok(V::Unit),
                    }),
                }
            }
            Expr::Match(x, arms) => {
                let v = self.eval(x, None)?;
                self.match_arms(&v, arms, hint)
            }
            Expr::Foreach {
                var,
                from,
                dir,
                to,
                step,
                body,
            } => {
                let a = self.eval(from, None)?;
                let b = self.eval(to, None)?;
                let s = self.eval(step, None)?;
                let (a, b, s) = (self.cint(&a)?, self.cint(&b)?, self.cint(&s)?);
                let mut i = a;
                loop {
                    let go = match dir {
                        Dir::Up => i <= b,
                        Dir::Down => i >= b,
                    };
                    if !go || self.guard.dead {
                        break;
                    }
                    self.push();
                    self.bind(var, V::Int(i));
                    let r = self.eval(body, None);
                    self.pop();
                    r?;
                    i = match dir {
                        Dir::Up => i + s,
                        Dir::Down => i - s,
                    };
                }
                Ok(V::Unit)
            }
            Expr::While(c, body) => {
                for _ in 0..100_000 {
                    if self.guard.dead {
                        break;
                    }
                    let c = self.eval(c, None)?;
                    if !self.cbool(&c)? {
                        return Ok(V::Unit);
                    }
                    self.eval(body, None)?;
                }
                Ok(V::Unit)
            }
            Expr::Repeat(body, c) => {
                for _ in 0..100_000 {
                    if self.guard.dead {
                        break;
                    }
                    self.eval(body, None)?;
                    let c = self.eval(c, None)?;
                    if self.cbool(&c)? {
                        return Ok(V::Unit);
                    }
                }
                Ok(V::Unit)
            }
            Expr::Return(x) => {
                let v = self.eval(x, hint)?;
                if !self.guard.dead {
                    let g = self.guard.clone();
                    self.frames
                        .last_mut()
                        .ok_or_else(|| err("return outside a function"))?
                        .push((g, v));
                }
                self.guard = Guard::never();
                Ok(V::Undef(None))
            }
            Expr::Throw(x) => {
                let v = self.eval(x, None)?;
                let kind = match &v {
                    V::Union(c, _) => match c.as_ref() {
                        "Error_Undefined" | "Error_ReservedEncoding" => "Undefined",
                        "Error_Unpredictable" | "Error_ConstrainedUnpredictable" => "Unpredictable",
                        "Error_See" => {
                            self.see()?;
                            return Ok(V::Undef(None));
                        }
                        c => return fail(format!("throw {c}")),
                    },
                    _ => return fail(format!("throw {v:?}")),
                };
                self.throw(kind)?;
                Ok(V::Undef(None))
            }
            Expr::Exit => fail("exit"),
            Expr::Try(x, _) => self.eval(x, hint),
            Expr::Struct(fs) => {
                let mut f = Vec::new();
                for (k, x) in fs {
                    f.push((k.clone(), self.eval(x, None)?));
                }
                let names: BTreeSet<&str> = f.iter().map(|(k, _)| k.as_str()).collect();
                let mut ty = "?".to_string();
                let mut snames: Vec<&String> = self.m.structs.keys().collect();
                snames.sort();
                for sn in snames {
                    let fields = &self.m.structs[sn];
                    let fnames: BTreeSet<&str> = fields.iter().map(|(k, _)| k.as_str()).collect();
                    if fnames == names {
                        ty = sn.clone();
                        break;
                    }
                }
                Ok(V::Struct(ty.as_str().into(), Rc::new(f)))
            }
            Expr::SUpdate(base, fs) => {
                let base = self.eval(base, None)?;
                let V::Struct(t, f) = base else {
                    return fail("struct update of a non-struct");
                };
                let mut f = (*f).clone();
                for (k, x) in fs {
                    let v = self.eval(x, None)?;
                    match f.iter_mut().find(|(n, _)| n == k) {
                        Some(slot) => slot.1 = v,
                        None => f.push((k.clone(), v)),
                    }
                }
                Ok(V::Struct(t, Rc::new(f)))
            }
            Expr::VecLit(xs) => {
                let mut out: Option<V> = None;
                for x in xs {
                    let v = self.eval(x, None)?;
                    let v = self.bitify(&v)?;
                    out = Some(match out {
                        None => v,
                        Some(o) => self.concat(&o, &v)?,
                    });
                }
                out.ok_or_else(|| err("empty vector literal"))
            }
            Expr::VUpdate(base, ups) => {
                let mut b = self.eval(base, None)?;
                for (a, lo, val) in ups {
                    let v = self.eval(val, None)?;
                    if let V::Bf(ty, bits) = &b
                        && let Expr::Id(f) = a
                    {
                        let (hi, lo) = self.bf_field(ty, f)?;
                        let nb = self.set_slice(bits, hi, lo, &v)?;
                        b = V::Bf(ty.clone(), Box::new(nb));
                        continue;
                    }
                    let hi = self.eval(a, None)?;
                    let lo = match lo {
                        Some(l) => self.eval(l, None)?,
                        None => hi.clone(),
                    };
                    let (hi, lo) = (self.cint(&hi)? as u32, self.cint(&lo)? as u32);
                    b = self.set_slice(&b, hi, lo, &v)?;
                }
                Ok(b)
            }
            Expr::Field(base, f) => {
                if let Expr::Id(n) = base.as_ref()
                    && n == "PSTATE"
                    && matches!(f.as_str(), "N" | "Z" | "C" | "V")
                {
                    self.used_cut.insert("PSTATE.NZCV");
                    let r = f.to_string();
                    return Ok(self.dy(K::Bits(1), format!("st.get(b, regs::{r})")));
                }
                let base = self.eval(base, None)?;
                self.field(&base, f)
            }
            Expr::Index(base, i) => {
                let bv = self.eval(base, None)?;
                if let V::Bf(ty, bits) = &bv
                    && let Expr::Id(f) = i.as_ref()
                    && let Some((_, fs)) = self.m.bitfields.get(ty.as_ref())
                    && fs.contains_key(f)
                {
                    let (hi, lo) = fs[f];
                    let bits = bits.clone();
                    return self.extract(&bits, &V::Int(i128::from(lo)), hi - lo + 1);
                }
                let iv = self.eval(i, None)?;
                if let V::Tuple(_) = bv {
                    return fail("vector index");
                }
                self.bit(&bv, &iv)
            }
            Expr::Slice(base, hi, lo) => {
                let bv = self.eval(base, None)?;
                let h = self.eval(hi, None)?;
                let l = self.eval(lo, None)?;
                if let (V::Int(h), V::Int(l)) = (&h, &l) {
                    if h < l {
                        return fail("empty slice");
                    }
                    return self.extract(&bv, &V::Int(*l), (h - l + 1) as u32);
                }
                let w = match self.slice_width(hi, lo) {
                    Ok(w) => w,
                    Err(E::Fail(m)) => {
                        let d = h.deps() | l.deps();
                        if d != 0 {
                            return Err(E::Split(d));
                        }
                        return fail(m);
                    }
                    Err(e) => return Err(e),
                };
                self.extract(&bv, &l, w)
            }
            Expr::Binop(op, a, b) => self.binop(op, a, b),
            Expr::Call(f, args) => {
                let mut vs = Vec::new();
                for a in args {
                    vs.push(self.eval(a, None)?);
                }
                self.call(f, vs, hint)
            }
        }
    }

    fn field(&mut self, base: &V, f: &str) -> R<V> {
        match base {
            V::Struct(_, fs) => fs
                .iter()
                .find(|(n, _)| n == f)
                .map(|(_, v)| v.clone())
                .ok_or_else(|| err(format!("no field {f}"))),
            V::Bf(ty, bits) => {
                if f == "bits" {
                    return Ok((**bits).clone());
                }
                let (hi, lo) = self.bf_field(ty, f)?;
                let bits = (**bits).clone();
                self.extract(&bits, &V::Int(i128::from(lo)), hi - lo + 1)
            }
            _ => fail(format!("field {f} of {base:?}")),
        }
    }

    fn bf_field(&self, ty: &str, f: &str) -> R<(u32, u32)> {
        self.m
            .bitfields
            .get(ty)
            .and_then(|(_, fs)| fs.get(f).copied())
            .ok_or_else(|| err(format!("no bitfield {ty}.{f}")))
    }

    fn slice_width(&mut self, hi: &Expr, lo: &Expr) -> R<u32> {
        let d = Expr::Binop("-".into(), Box::new(hi.clone()), Box::new(lo.clone()));
        let save = self.out.len();
        let v = self.eval(&d, None)?;
        if let V::Int(x) = v {
            self.out.truncate(save);
            return Ok((x + 1) as u32);
        }
        if let (Expr::Binop(o, a, b), _) = (hi, lo)
            && o == "-"
            && let Expr::Binop(p, x, y) = a.as_ref()
            && p == "+"
            && x.as_ref() == lo
        {
            let y = self.eval(y, None)?;
            let b = self.eval(b, None)?;
            if let (V::Int(y), V::Int(b)) = (y, b) {
                return Ok((y - b + 1) as u32);
            }
        }
        fail("symbolic slice width")
    }

    /// The value of a concrete integer, or a split request.
    pub fn cint(&self, v: &V) -> R<i128> {
        match v {
            V::Int(i) => Ok(*i),
            V::S(K::Int, _, d) if *d != 0 => Err(E::Split(*d)),
            _ => fail(format!("not a concrete int: {v:?}")),
        }
    }

    fn cbool(&mut self, v: &V) -> R<bool> {
        match self.truth(v)? {
            V::Bool(b) => Ok(b),
            V::S(_, _, d) if d != 0 => Err(E::Split(d)),
            v => fail(format!("not a concrete bool: {v:?}")),
        }
    }

    fn stmt(&mut self, s: &Stmt) -> R<V> {
        match s {
            Stmt::Expr(e) => self.eval(e, None),
            Stmt::Let(p, e) => {
                let hint = self.pat_hint(p);
                let v = self.eval(e, hint)?;
                self.bind_pat(p, v)?;
                Ok(V::Unit)
            }
            Stmt::Decl(lhs, t, e) => {
                let w = self.type_width(t);
                let mut v = self.eval(e, w)?;
                if let V::Undef(_) = v {
                    v = self.undef_of(t, w);
                }
                match lhs {
                    Expr::Id(n) => self.bind(n, v),
                    _ => self.assign(lhs, v)?,
                }
                Ok(V::Unit)
            }
            Stmt::Assign(lhs, e) => {
                let hint = self.lexp_hint(lhs);
                let v = self.eval(e, hint)?;
                self.assign(lhs, v)?;
                Ok(V::Unit)
            }
        }
    }

    fn undef_of(&mut self, t: &Typ, w: Option<u32>) -> V {
        if w.is_some() {
            return V::Undef(w);
        }
        if let Typ::Id(n) = t {
            if let Some(fields) = self.m.structs.get(n) {
                let mut f = Vec::new();
                for (k, ft) in fields.clone() {
                    let t = crate::parse::parse_typ(&ft).unwrap_or(Typ::Id("?".into()));
                    let w = self.type_width(&t);
                    f.push((k, self.undef_of(&t, w)));
                }
                return V::Struct(n.as_str().into(), Rc::new(f));
            }
            if let Some((bw, _)) = self.m.bitfields.get(n) {
                return V::Bf(n.as_str().into(), Box::new(V::Bits(*bw, 0)));
            }
            if n == "bool" {
                return V::Bool(false);
            }
            if n == "int" || n == "integer" || n == "nat" {
                return V::Int(0);
            }
        }
        V::Undef(None)
    }

    /// The width of a bits type, when concrete.
    pub fn type_width(&mut self, t: &Typ) -> Option<u32> {
        match t {
            Typ::App(n, args) if n == "bits" || n == "bitvector" => {
                if let Some(TypArg::Expr(e)) = args.first() {
                    let save = self.out.len();
                    let r = self.eval(e, None);
                    match r {
                        Ok(V::Int(w)) => {
                            self.out.truncate(save);
                            Some(w as u32)
                        }
                        _ => {
                            self.out.truncate(save);
                            None
                        }
                    }
                } else {
                    None
                }
            }
            Typ::Id(n) => {
                let txt = self.m.types.get(n)?;
                txt.strip_prefix("bits(")
                    .and_then(|s| s.strip_suffix(')'))
                    .and_then(|s| s.trim().parse().ok())
            }
            _ => None,
        }
    }

    fn pat_hint(&mut self, p: &Pat) -> Option<u32> {
        match p {
            Pat::Typed(_, t) => self.type_width(t),
            _ => None,
        }
    }

    fn lexp_hint(&mut self, l: &Expr) -> Option<u32> {
        if let Expr::Id(n) = l
            && let Some(v) = self.locals.get(n)
        {
            return v.width();
        }
        None
    }

    fn bind_pat(&mut self, p: &Pat, v: V) -> R<()> {
        match p {
            Pat::Id(n) => {
                self.bind(n, v);
                Ok(())
            }
            Pat::Wild => Ok(()),
            Pat::Typed(q, t) => {
                let w = self.type_width(t);
                let v = if let V::Undef(_) = v {
                    self.undef_of(t, w)
                } else {
                    v
                };
                self.bind_pat(q, v)
            }
            Pat::Tuple(ps) => {
                let V::Tuple(vs) = v else {
                    return fail(format!("tuple pattern on {v:?}"));
                };
                for (q, x) in ps.iter().zip(vs.iter()) {
                    self.bind_pat(q, x.clone())?;
                }
                Ok(())
            }
            Pat::As(q, n) => {
                self.bind_pat(q, v.clone())?;
                self.bind(n, v);
                Ok(())
            }
            _ => fail(format!("bind pattern {p:?}")),
        }
    }

    fn assign(&mut self, l: &Expr, v: V) -> R<()> {
        match l {
            Expr::Id(n) => self.assign_name(n, v),
            Expr::Typed(x, _) => self.assign(x, v),
            Expr::Tuple(xs) => {
                let V::Tuple(vs) = v else {
                    return fail("tuple assign");
                };
                for (q, x) in xs.iter().zip(vs.iter()) {
                    self.assign(q, x.clone())?;
                }
                Ok(())
            }
            Expr::Binop(op, _, _) if op == "@" => {
                let mut parts = Vec::new();
                flatten_concat(l, &mut parts);
                let mut ws = Vec::new();
                for p in &parts {
                    let w = if let Expr::Field(b, f) = p
                        && matches!(b.as_ref(), Expr::Id(n) if n == "PSTATE")
                        && matches!(f.as_str(), "N" | "Z" | "C" | "V")
                    {
                        1
                    } else {
                        let save = self.out.len();
                        let cur = self.eval(p, None)?;
                        self.out.truncate(save);
                        cur.width().ok_or_else(|| err("concat assign width"))?
                    };
                    ws.push(w);
                }
                let mut pos: u32 = ws.iter().sum();
                for (p, w) in parts.iter().zip(ws) {
                    pos -= w;
                    let x = self.extract(&v, &V::Int(i128::from(pos)), w)?;
                    self.assign(p, x)?;
                }
                Ok(())
            }
            Expr::Field(b, f) => {
                if let Expr::Id(n) = b.as_ref()
                    && n == "PSTATE"
                    && matches!(f.as_str(), "N" | "Z" | "C" | "V")
                {
                    self.used_cut.insert("PSTATE.NZCV");
                    let v = self.bitify(&v)?;
                    let x = self.dx(&v)?;
                    let g = self.gd();
                    self.effect(format!("st.set(b, regs::{f}, {x}, {g});"));
                    return Ok(());
                }
                let base = self.eval(b, None)?;
                let nb = match base {
                    V::Struct(t, fs) => {
                        let mut fs = (*fs).clone();
                        match fs.iter_mut().find(|(n, _)| n == f) {
                            Some(s) => s.1 = v,
                            None => fs.push((f.clone(), v)),
                        }
                        V::Struct(t, Rc::new(fs))
                    }
                    V::Bf(t, bits) => {
                        if f == "bits" {
                            V::Bf(t, Box::new(v))
                        } else {
                            let (hi, lo) = self.bf_field(&t, f)?;
                            let nb = self.set_slice(&bits, hi, lo, &v)?;
                            V::Bf(t, Box::new(nb))
                        }
                    }
                    _ => return fail(format!("field assign on {base:?}")),
                };
                self.assign(b, nb)
            }
            Expr::Index(b, i)
                if matches!(b.as_ref(), Expr::Id(r) if r == "FPSR")
                    && matches!(i.as_ref(), Expr::Id(f) if f == "QC") =>
            {
                // The saturation flag is the one FPSR write outside the FP
                // cut: it is sticky, so a write ORs it in.
                if !matches!(self.defd(&v), V::Bits(1, 1)) {
                    return fail("FPSR.QC written with a value other than 1");
                }
                self.used_cut.insert("FPSR.QC");
                let g = self.gd();
                self.effect(format!("st.set_qc(b, {g});"));
                Ok(())
            }
            Expr::Index(b, i) => {
                let base = self.eval(b, None)?;
                if let V::Bf(t, bits) = &base
                    && let Expr::Id(f) = i.as_ref()
                    && let Ok((hi, lo)) = self.bf_field(t, f)
                {
                    let nb = self.set_slice(bits, hi, lo, &v)?;
                    return self.assign(b, V::Bf(t.clone(), Box::new(nb)));
                }
                let iv = self.eval(i, None)?;
                let i = self.cint(&iv)? as u32;
                let nb = self.set_slice(&base, i, i, &v)?;
                self.assign(b, nb)
            }
            Expr::Slice(b, hi, lo) => {
                let base = self.eval(b, None)?;
                let h = self.eval(hi, None)?;
                let lw = self.eval(lo, None)?;
                let (h, lw) = (self.cint(&h)? as u32, self.cint(&lw)? as u32);
                let nb = self.set_slice(&base, h, lw, &v)?;
                self.assign(b, nb)
            }
            Expr::Call(f, args) => {
                let mut vs = Vec::new();
                for a in args {
                    vs.push(self.eval(a, None)?);
                }
                vs.push(v);
                self.call(f, vs, None)?;
                Ok(())
            }
            _ => fail(format!("assign to {l:?}")),
        }
    }

    /// `base` with bits `hi .. lo` replaced by `v`.
    pub fn set_slice(&mut self, base: &V, hi: u32, lo: u32, v: &V) -> R<V> {
        let base = self.defd(base);
        let v = self.bitify(v)?;
        let bw = base.width().ok_or_else(|| err("set_slice base"))?;
        if hi < lo || hi >= bw {
            return fail("set_slice range");
        }
        let w = hi - lo + 1;
        let v = if v.width() == Some(w) {
            v
        } else {
            self.extract(&v, &V::Int(0), w)?
        };
        let mut out: Option<V> = None;
        if hi + 1 < bw {
            out = Some(self.extract(&base, &V::Int(i128::from(hi + 1)), bw - hi - 1)?);
        }
        out = Some(match out {
            None => v,
            Some(o) => self.concat(&o, &v)?,
        });
        if lo > 0 {
            let low = self.extract(&base, &V::Int(0), lo)?;
            out = Some(self.concat(out.as_ref().expect("set"), &low)?);
        }
        Ok(out.expect("set"))
    }

    fn match_arms(&mut self, v: &V, arms: &[Arm], hint: Option<u32>) -> R<V> {
        let Some((arm, rest)) = arms.split_first() else {
            if v.is_concrete() && v.deps() == 0 {
                return fail(format!("match failure on {v:?}"));
            }
            self.guard = Guard::never();
            return Ok(V::Undef(None));
        };
        self.push();
        let r = (|| {
            let mut c = self.pat_cond(&arm.pat, v)?;
            if c != V::Bool(false)
                && let Some(g) = &arm.guard
            {
                let gv = self.eval(g, None)?;
                c = self.and(&c, &gv)?;
            }
            match c {
                V::Bool(true) => self.eval(&arm.body, hint),
                V::Bool(false) => {
                    self.pop();
                    self.push();
                    self.match_arms(v, rest, hint)
                }
                _ => self.run_branch(&c, &mut |g| g.eval(&arm.body, hint), &mut |g| {
                    g.match_arms(v, rest, hint)
                }),
            }
        })();
        self.pop();
        r
    }

    fn pat_cond(&mut self, p: &Pat, v: &V) -> R<V> {
        match p {
            Pat::Wild => Ok(V::Bool(true)),
            Pat::Id(n) => {
                if let Some((ty, i)) = self.m.enum_of.get(n) {
                    let e = V::Enum(ty.as_str().into(), *i);
                    return self.cmp(Cmp::Eq, v, &e);
                }
                self.bind(n, v.clone());
                Ok(V::Bool(true))
            }
            Pat::Lit(l) => {
                let l = lit(l);
                if l == V::Unit {
                    return Ok(V::Bool(true));
                }
                if let V::Undef(_) = v {
                    return Ok(V::Bool(false));
                }
                if let (V::Bits(w, x), V::S(_, _, deps)) = (&l, v)
                    && *w > 1
                    && deps.count_ones() < *w
                    && v.width() == Some(*w)
                {
                    // A value with fewer unknown word bits than its width
                    // (a decode clause written as one binary literal):
                    // bit by bit, so the bits known while generating fold.
                    // Kept only when the compare folds to a constant; the
                    // lines of a partial fold are dropped again.
                    let (mark, n) = (self.out.len(), self.n);
                    let mut c = V::Bool(true);
                    for i in 0..*w {
                        let bit = self.extract(v, &V::Int(i128::from(i)), 1)?;
                        let d = self.cmp(Cmp::Eq, &bit, &V::Bits(1, x >> i & 1))?;
                        c = self.and(&c, &d)?;
                        if c == V::Bool(false) {
                            break;
                        }
                    }
                    self.out.truncate(mark);
                    self.n = n;
                    if let V::Bool(_) = c {
                        return Ok(c);
                    }
                }
                let (a, b) = match (&l, v.kind()) {
                    (V::Bits(1, x), Kind::Bool) => (V::Bool(*x == 1), v.clone()),
                    _ => (l.clone(), v.clone()),
                };
                self.cmp(Cmp::Eq, &b, &a)
            }
            Pat::Typed(q, _) => self.pat_cond(q, v),
            Pat::As(q, n) => {
                let c = self.pat_cond(q, v)?;
                self.bind(n, v.clone());
                Ok(c)
            }
            Pat::Tuple(ps) => {
                let V::Tuple(vs) = v else {
                    return fail("tuple pattern");
                };
                let mut c = V::Bool(true);
                for (q, x) in ps.iter().zip(vs.iter()) {
                    let d = self.pat_cond(q, x)?;
                    c = self.and(&c, &d)?;
                }
                Ok(c)
            }
            Pat::Vec(ps) => {
                let w = v.width().ok_or_else(|| err("vector pattern width"))?;
                if ps.len() as u32 != w {
                    return fail("vector pattern width");
                }
                let mut c = V::Bool(true);
                for (i, q) in ps.iter().enumerate() {
                    if *q == Pat::Wild {
                        continue;
                    }
                    let bit = self.extract(v, &V::Int(i128::from(w - 1 - i as u32)), 1)?;
                    let d = self.pat_cond(q, &bit)?;
                    c = self.and(&c, &d)?;
                    if c == V::Bool(false) {
                        return Ok(c);
                    }
                }
                Ok(c)
            }
            Pat::Concat(ps) => {
                let w = v.width().ok_or_else(|| err("concat pattern width"))?;
                let mut ws: Vec<Option<u32>> = Vec::new();
                for q in ps {
                    ws.push(self.pat_width(q));
                }
                let unk: Vec<usize> = (0..ws.len()).filter(|i| ws[*i].is_none()).collect();
                if unk.len() > 1 {
                    return fail("concat pattern widths");
                }
                if let Some(i) = unk.first() {
                    let known: u32 = ws.iter().flatten().sum();
                    ws[*i] = Some(w - known);
                }
                let mut pos = w;
                let mut c = V::Bool(true);
                for (q, qw) in ps.iter().zip(ws) {
                    let qw = qw.expect("width");
                    pos -= qw;
                    let x = self.extract(v, &V::Int(i128::from(pos)), qw)?;
                    let d = self.pat_cond(q, &x)?;
                    c = self.and(&c, &d)?;
                    if c == V::Bool(false) {
                        return Ok(c);
                    }
                }
                Ok(c)
            }
            Pat::App(n, args) => {
                if let V::Union(c, arg) = v {
                    if c.as_ref() != n {
                        return Ok(V::Bool(false));
                    }
                    return match args.len() {
                        0 => Ok(V::Bool(true)),
                        1 => self.pat_cond(&args[0], arg),
                        _ => self.pat_cond(&Pat::Tuple(args.clone()), arg),
                    };
                }
                fail(format!("constructor pattern on {v:?}"))
            }
        }
    }

    fn pat_width(&mut self, q: &Pat) -> Option<u32> {
        match q {
            Pat::Vec(ps) => Some(ps.len() as u32),
            Pat::Lit(Lit::Bits { width, .. }) => Some(*width),
            Pat::Typed(_, t) => self.type_width(t),
            Pat::As(q, _) => self.pat_width(q),
            _ => None,
        }
    }

    fn binop(&mut self, op: &str, a: &Expr, b: &Expr) -> R<V> {
        match op {
            "&" | "|" => {
                let x = self.eval(a, None)?;
                if x.kind() == Kind::Bits && x.width() != Some(1) || matches!(x, V::Bits(..)) {
                    let y = self.eval(b, None)?;
                    if y.kind() == Kind::Bits || x.width() != Some(1) {
                        let o = if op == "&" { BOp::And } else { BOp::Or };
                        return self.bits_op(o, &x, &y);
                    }
                }
                let xb = self.truth(&x)?;
                if op == "&" && xb == V::Bool(false) {
                    return Ok(V::Bool(false));
                }
                if op == "|" && xb == V::Bool(true) {
                    return Ok(V::Bool(true));
                }
                let y = self.eval(b, None)?;
                if op == "&" {
                    self.and(&xb, &y)
                } else {
                    self.or(&xb, &y)
                }
            }
            "-->" => {
                let x = self.eval(a, None)?;
                let x = self.truth(&x)?;
                if x == V::Bool(false) {
                    return Ok(V::Bool(true));
                }
                let nx = self.not(&x)?;
                let y = self.eval(b, None)?;
                self.or(&nx, &y)
            }
            _ => {
                let x = self.eval(a, None)?;
                let y = self.eval(b, None)?;
                self.call(&format!("op{op}"), vec![x, y], None)
            }
        }
    }

    /// Calls a function, an overload, a builtin or a cut intrinsic.
    pub fn call(&mut self, name: &str, args: Vec<V>, hint: Option<u32>) -> R<V> {
        if self.guard.dead {
            return Ok(V::Undef(None));
        }
        if let Some(r) = self.cut(name, &args, hint)? {
            return Ok(r);
        }
        if let Some(t) = name.strip_prefix("Mk_")
            && self.m.bitfields.contains_key(t)
            && !self.m.funcs.contains_key(name)
        {
            return Ok(V::Bf(t.into(), Box::new(args[0].clone())));
        }
        if name == "op^" && args.len() == 2 {
            if let (V::Int(a), V::Int(b)) = (&args[0], &args[1]) {
                return Ok(V::Int(a.pow(*b as u32)));
            }
            if args[0] == V::Int(2) {
                return self.builtin("pow2", &args[1..], hint)?.ok_or(E::NoMatch);
            }
        }
        if self.m.unions.contains_key(name) && !self.m.funcs.contains_key(name) {
            let a = if args.len() == 1 {
                args[0].clone()
            } else {
                V::tuple(args)
            };
            return Ok(V::Union(name.into(), Box::new(a)));
        }
        if let Some(cands) = self.m.overloads.get(name) {
            for c in cands.clone() {
                match self.try_call(&c, &args, hint, false) {
                    Err(E::NoMatch) => continue,
                    r => return r,
                }
            }
            let ks: Vec<Kind> = args.iter().map(V::kind).collect();
            return fail(format!("no overload of {name} for {ks:?}"));
        }
        self.try_call(name, &args, hint, true)
    }

    fn try_call(&mut self, name: &str, args: &[V], hint: Option<u32>, force: bool) -> R<V> {
        if self.m.overloads.contains_key(name)
            && !self.m.funcs.contains_key(name)
            && !self.m.vals.contains_key(name)
        {
            return match self.call(name, args.to_vec(), hint) {
                Err(E::Fail(m)) if m.starts_with("no overload") => Err(E::NoMatch),
                r => r,
            };
        }
        let sig = self.m.sig(name);
        let mut args = args.to_vec();
        let mut tyvars: HashMap<String, V> = HashMap::new();
        let mut ext: Option<String> = None;
        if let Some(s) = &sig {
            ext = s.extern_.clone();
            if let Some((ptys, _)) = &s.fun {
                match self.match_sig(ptys, &args, hint) {
                    Some((a, tv)) => {
                        args = a;
                        tyvars = tv;
                    }
                    None => {
                        if force {
                            let ks: Vec<Kind> = args.iter().map(V::kind).collect();
                            return fail(format!("signature mismatch {name} {ks:?}"));
                        }
                        return Err(E::NoMatch);
                    }
                }
            }
        }
        if let Some(r) = self.builtin(name, &args, hint)? {
            return Ok(r);
        }
        if let Some(x) = &ext
            && !self.m.funcs.contains_key(name)
        {
            for c in extern_names(x) {
                if let Some(r) = self.builtin(&c, &args, hint)? {
                    return Ok(r);
                }
            }
            return fail(format!("missing builtin {name} ({x})"));
        }
        if !self.m.funcs.contains_key(name) {
            if !force {
                return Err(E::NoMatch);
            }
            return fail(format!("no definition for {name}"));
        }
        self.invoke(name, args, tyvars)
    }

    fn match_sig(
        &mut self,
        ptys: &[Typ],
        args: &[V],
        hint: Option<u32>,
    ) -> Option<(Vec<V>, HashMap<String, V>)> {
        let mut tv = HashMap::new();
        let nimpl = ptys.iter().filter(|t| is_implicit(t)).count();
        if ptys.len() == 1 && ptys[0] == Typ::Id("unit".into()) && args.is_empty() {
            return Some((Vec::new(), tv));
        }
        let mut args = args.to_vec();
        if args.len() != ptys.len() {
            if args.len() + nimpl == ptys.len() && nimpl == 1 {
                let h = hint?;
                let i = ptys.iter().position(is_implicit)?;
                args.insert(i, V::Int(i128::from(h)));
            } else {
                return None;
            }
        }
        for (t, a) in ptys.iter().zip(args.iter()) {
            if !self.unify(t, a, &mut tv) {
                return None;
            }
        }
        Some((args, tv))
    }

    fn unify(&self, t: &Typ, a: &V, tv: &mut HashMap<String, V>) -> bool {
        let k = a.kind();
        if k == Kind::Undef {
            return true;
        }
        match t {
            Typ::Tuple(_) => k == Kind::Tuple,
            Typ::Set => k == Kind::Int,
            Typ::Id(n) => {
                if n.starts_with('\'') {
                    return true;
                }
                match n.as_str() {
                    "int" | "nat" => k == Kind::Int,
                    "bool" => k == Kind::Bool,
                    "real" => false,
                    "string" => k == Kind::Str,
                    "unit" => k == Kind::Unit,
                    "bit" => k == Kind::Bits && a.width() == Some(1),
                    _ => {
                        if self.m.enums.contains_key(n) {
                            return k == Kind::Enum(n.as_str().into());
                        }
                        if self.m.structs.contains_key(n) {
                            return k == Kind::Struct(n.as_str().into());
                        }
                        if self.m.bitfields.contains_key(n) {
                            return k == Kind::Bf(n.as_str().into());
                        }
                        if let Some(txt) = self.m.types.get(n) {
                            if txt.starts_with("bits") || txt.starts_with("bitvector") {
                                return k == Kind::Bits;
                            }
                            if txt.starts_with("int")
                                || txt.starts_with("range")
                                || txt.starts_with('{')
                            {
                                return k == Kind::Int;
                            }
                            return true;
                        }
                        if self.m.unions.values().any(|u| u == n) {
                            return k == Kind::Union;
                        }
                        true
                    }
                }
            }
            Typ::App(n, args) => match n.as_str() {
                "bits" | "bitvector" => {
                    if k != Kind::Bits {
                        return false;
                    }
                    if let Some(TypArg::Expr(Expr::Id(v))) = args.first()
                        && v.starts_with('\'')
                    {
                        let w = V::Int(i128::from(a.width().unwrap_or(0)));
                        if let Some(old) = tv.get(v)
                            && *old != w
                        {
                            return false;
                        }
                        tv.insert(v.clone(), w);
                    }
                    true
                }
                "int" | "atom" | "range" | "implicit" | "nat" => k == Kind::Int,
                "bool" => k == Kind::Bool,
                "vector" => k == Kind::Bits,
                "option" => k == Kind::Union,
                _ => true,
            },
            Typ::Fun(..) => false,
        }
    }

    fn invoke(&mut self, name: &str, args: Vec<V>, _tyvars: HashMap<String, V>) -> R<V> {
        if self.m.stubs.contains(name) && !STUBS_OK.contains(&name) {
            return fail(format!("calls the stub {name}"));
        }
        let clauses = match self.m.func(name) {
            Some(Ok(c)) => c,
            Some(Err(e)) => return fail(format!("parse {name}: {e}")),
            None => return fail(format!("no function {name}")),
        };
        self.depth += 1;
        if self.depth > 300 {
            return fail("recursion depth");
        }
        let saved_l = std::mem::take(&mut self.locals);
        let saved_s = std::mem::replace(&mut self.scopes, vec![Vec::new()]);
        let g0 = self.guard.clone();
        self.frames.push(Vec::new());
        let r = self.invoke_body(name, &clauses, &args);
        self.frames.pop();
        self.guard = g0;
        self.locals = saved_l;
        self.scopes = saved_s;
        self.depth -= 1;
        r
    }

    fn invoke_body(&mut self, name: &str, clauses: &[crate::ast::Clause], args: &[V]) -> R<V> {
        if let Some(sig) = self.m.sig(name)
            && let Some((ptys, _)) = &sig.fun
        {
            for (t, a) in ptys.iter().zip(args.iter()) {
                if let Typ::App(n, targs) = t
                    && let Some(TypArg::Expr(Expr::Id(v))) = targs.first()
                    && v.starts_with('\'')
                {
                    match n.as_str() {
                        "bits" | "bitvector" => {
                            let w = a.width().unwrap_or(0);
                            self.locals.insert(v.clone(), V::Int(i128::from(w)));
                        }
                        "int" | "atom" | "implicit" | "bool" => {
                            self.locals.insert(v.clone(), a.clone());
                        }
                        _ => {}
                    }
                }
            }
        }
        let argv = match args.len() {
            0 => V::Unit,
            1 => args[0].clone(),
            _ => V::tuple(args.to_vec()),
        };
        for c in clauses {
            let mut cond = if c.params == Pat::Lit(Lit::Unit) {
                V::Bool(true)
            } else {
                self.pat_cond(&c.params, &argv)?
            };
            if cond != V::Bool(false)
                && let Some(g) = &c.guard
            {
                let gv = self.eval(g, None)?;
                cond = self.and(&cond, &gv)?;
            }
            match cond {
                V::Bool(false) => {
                    self.locals.retain(|k, _| k.starts_with('\''));
                    continue;
                }
                V::Bool(true) => {}
                _ => return fail(format!("symbolic clause selection in {name}")),
            }
            let v = self.eval(&c.body, None)?;
            let rets = self.frames.last().expect("frame").clone();
            if rets.is_empty() {
                return Ok(v);
            }
            let live = !self.guard.dead;
            let (mut acc, rest) = if live {
                (v, &rets[..])
            } else {
                (rets[rets.len() - 1].1.clone(), &rets[..rets.len() - 1])
            };
            for (g, rv) in rest.iter().rev() {
                acc = self.gsel(g, rv, &acc)?;
            }
            return Ok(acc);
        }
        fail(format!("no clause matched for {name}"))
    }
}

impl Gen<'_> {
    /// Runs one `__DecodeA64_*` clause on the template's word.
    pub fn run_decode(&mut self, c: &crate::ast::Clause) -> R<()> {
        self.frames.push(Vec::new());
        let argv = V::tuple(vec![
            V::S(K::Bits(64), "pc".into(), 0),
            V::S(K::Bits(32), "w".into(), !self.known),
        ]);
        let cond = self.pat_cond(&c.params, &argv)?;
        if cond != V::Bool(true) {
            return fail(format!("decode clause pattern is {cond:?}"));
        }
        if let Some(g) = &c.guard {
            let gv = self.eval(g, None)?;
            if !self.cbool(&gv)? {
                return fail("decode clause guard is false");
            }
        }
        self.eval(&c.body, None)?;
        Ok(())
    }
}

fn is_implicit(t: &Typ) -> bool {
    matches!(t, Typ::App(n, _) if n == "implicit")
}

fn flatten_concat(l: &Expr, out: &mut Vec<Expr>) {
    if let Expr::Binop(op, a, b) = l
        && op == "@"
    {
        flatten_concat(a, out);
        flatten_concat(b, out);
        return;
    }
    out.push(l.clone());
}

/// The value of a literal.
pub fn lit(l: &Lit) -> V {
    match l {
        Lit::Unit => V::Unit,
        Lit::Bool(b) => V::Bool(*b),
        Lit::Bit(b) => V::Bits(1, u128::from(*b)),
        Lit::Int(i) => V::Int(*i),
        Lit::Bits { width, value } => V::Bits(*width, *value),
        Lit::Wide(ps) => V::Wide(Rc::new(ps.iter().map(|p| V::Bits(128, *p)).collect())),
        Lit::Real(r) => V::Str(r.as_str().into()),
        Lit::Str(s) => V::Str(s.as_str().into()),
    }
}

/// The builtin names an extern declaration names.
pub fn extern_names(ext: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut rest = ext;
    while let Some(i) = rest.find('"') {
        let after = &rest[i + 1..];
        let Some(j) = after.find('"') else { break };
        out.push(after[..j].to_string());
        rest = &after[j + 1..];
    }
    out
}

impl Gen<'_> {
    /// Applies the integer operation `op` (used by builtins).
    pub fn iop(&mut self, op: IOp, a: &V, b: &V) -> R<V> {
        self.int_op(op, a, b)
    }
}
