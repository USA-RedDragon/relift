//! A generated corpus of mangled names, from a small mangler that compresses
//! with substitutions the way a compiler does, checked by round trip and by
//! the independent demangler.

use std::collections::BTreeMap;

use super::oracle;
use crate::{GuestName, forward, guest_name, host_name};

/// How many generated entities the corpus test mangles.
const COUNT: usize = 20000;

/// A deterministic xorshift generator, so every run checks the same names.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }

    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }

    fn chance(&mut self, percent: usize) -> bool {
        self.below(100) < percent
    }

    fn pick<'a>(&mut self, items: &[&'a str]) -> &'a str {
        items[self.below(items.len())]
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
enum Ty {
    Builtin(&'static str),
    Class(Path),
    Ptr(Box<Ty>),
    LRef(Box<Ty>),
    RRef(Box<Ty>),
    Const(Box<Ty>),
    Vendor(&'static str, Box<Ty>),
    Func(Box<Ty>, Vec<Ty>),
    Param(usize),
    Pack(Box<Ty>),
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
enum Arg {
    Ty(Ty),
    Int(i64),
    Pack(Vec<Arg>),
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
enum SegName {
    Src(String),
    Internal(String),
    Ctor(&'static str),
    Op(&'static str),
    Conv(Box<Ty>),
    Lambda(Vec<Ty>, u32),
    Unnamed(u32),
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct Seg {
    name: SegName,
    tags: Vec<&'static str>,
    targs: Option<Vec<Arg>>,
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct Path {
    /// Nonzero for the entities of one local scope.
    scope: u32,
    std: bool,
    segs: Vec<Seg>,
}

/// What a path is mangled as.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Use {
    Type,
    Entity,
}

/// The mangler's substitution table.
#[derive(Default)]
struct Mangler {
    subs: Vec<String>,
}

fn src(s: &str) -> Seg {
    Seg {
        name: SegName::Src(s.to_string()),
        tags: Vec::new(),
        targs: None,
    }
}

fn targ(s: &str, args: Vec<Arg>) -> Seg {
    Seg {
        targs: Some(args),
        ..src(s)
    }
}

fn std_path(segs: Vec<Seg>) -> Path {
    Path {
        scope: 0,
        std: true,
        segs,
    }
}

fn char_traits() -> Ty {
    Ty::Class(std_path(vec![targ(
        "char_traits",
        vec![Arg::Ty(Ty::Builtin("c"))],
    )]))
}

fn allocator(t: Ty) -> Ty {
    Ty::Class(std_path(vec![targ("allocator", vec![Arg::Ty(t)])]))
}

fn std_string() -> Ty {
    Ty::Class(std_path(vec![targ(
        "basic_string",
        vec![
            Arg::Ty(Ty::Builtin("c")),
            Arg::Ty(char_traits()),
            Arg::Ty(allocator(Ty::Builtin("c"))),
        ],
    )]))
}

fn stream(name: &str) -> Ty {
    Ty::Class(std_path(vec![targ(
        name,
        vec![Arg::Ty(Ty::Builtin("c")), Arg::Ty(char_traits())],
    )]))
}

fn len_name(s: &str) -> String {
    format!("{}{s}", s.len())
}

impl Mangler {
    fn find(&self, key: &str) -> Option<String> {
        let i = self.subs.iter().position(|k| k == key)?;
        Some(crate::edit::ref_text(i))
    }

    fn add(&mut self, key: String) {
        self.subs.push(key);
    }

    fn key_path(p: &Path, k: usize, args: bool) -> String {
        let mut segs = p.segs[..k].to_vec();
        if !args && let Some(last) = segs.last_mut() {
            last.targs = None;
        }
        format!("P{}{}{segs:?}", p.scope, p.std)
    }

    fn ty(&mut self, t: &Ty) -> String {
        if let Ty::Builtin(b) = t {
            return (*b).to_string();
        }
        if let Ty::Class(p) = t {
            return self.path(p, Use::Type, "");
        }
        let key = format!("T{t:?}");
        if let Some(r) = self.find(&key) {
            return r;
        }
        let s = match t {
            Ty::Ptr(i) => format!("P{}", self.ty(i)),
            Ty::LRef(i) => format!("R{}", self.ty(i)),
            Ty::RRef(i) => format!("O{}", self.ty(i)),
            Ty::Const(i) => format!("K{}", self.ty(i)),
            Ty::Vendor(q, i) => format!("U{}{}", len_name(q), self.ty(i)),
            Ty::Func(r, ps) => format!("F{}{}E", self.ty(r), self.params(ps)),
            Ty::Param(0) => "T_".to_string(),
            Ty::Param(n) => format!("T{}_", n - 1),
            Ty::Pack(i) => format!("Dp{}", self.ty(i)),
            Ty::Builtin(_) | Ty::Class(_) => String::new(),
        };
        self.add(key);
        s
    }

    fn params(&mut self, ps: &[Ty]) -> String {
        if ps.is_empty() {
            return "v".to_string();
        }
        ps.iter().map(|p| self.ty(p)).collect()
    }

    fn arg(&mut self, a: &Arg) -> String {
        match a {
            Arg::Ty(t) => self.ty(t),
            Arg::Int(v) if *v < 0 => format!("Lin{}E", -v),
            Arg::Int(v) => format!("Li{v}E"),
            Arg::Pack(v) => format!("J{}E", v.iter().map(|a| self.arg(a)).collect::<String>()),
        }
    }

    fn args(&mut self, v: &[Arg]) -> String {
        format!("I{}E", v.iter().map(|a| self.arg(a)).collect::<String>())
    }

    fn seg_name(&mut self, s: &Seg) -> String {
        let mut out = match &s.name {
            SegName::Src(n) => len_name(n),
            SegName::Internal(n) => format!("L{}", len_name(n)),
            SegName::Ctor(c) | SegName::Op(c) => (*c).to_string(),
            SegName::Conv(t) => format!("cv{}", self.ty(t)),
            SegName::Lambda(ps, n) => {
                let d = if *n == 0 {
                    String::new()
                } else {
                    (n - 1).to_string()
                };
                format!("Ul{}E{d}_", self.params(ps))
            }
            SegName::Unnamed(0) => "Ut_".to_string(),
            SegName::Unnamed(n) => format!("Ut{}_", n - 1),
        };
        for t in &s.tags {
            out.push_str(&format!("B{}", len_name(t)));
        }
        out
    }

    /// The special abbreviation for the head of a std path, and whether the
    /// first segment's arguments still follow.
    fn special(p: &Path) -> Option<(&'static str, bool)> {
        let s0 = p.segs.first()?;
        if !p.std || p.scope != 0 || !s0.tags.is_empty() {
            return None;
        }
        let SegName::Src(n) = &s0.name else {
            return None;
        };
        let args = s0.targs.clone();
        let is = |t: Ty| match t {
            Ty::Class(q) => q.segs[0].targs == args,
            _ => false,
        };
        match n.as_str() {
            "basic_string" if is(std_string()) => Some(("Ss", false)),
            "basic_istream" if is(stream("basic_istream")) => Some(("Si", false)),
            "basic_ostream" if is(stream("basic_ostream")) => Some(("So", false)),
            "basic_iostream" if is(stream("basic_iostream")) => Some(("Sd", false)),
            "basic_string" => Some(("Sb", s0.targs.is_some())),
            "allocator" => Some(("Sa", s0.targs.is_some())),
            _ => None,
        }
    }

    fn path(&mut self, p: &Path, u: Use, quals: &str) -> String {
        let n = p.segs.len();
        if u == Use::Type
            && let Some(r) = self.find(&Self::key_path(p, n, true))
        {
            return r;
        }
        let mut head = String::new();
        let mut k = 0;
        let mut pending = false;
        let mut comps = 0;
        for j in (1..=n).rev() {
            if j == n && u == Use::Entity {
                continue;
            }
            if let Some(r) = self.find(&Self::key_path(p, j, true)) {
                head = r;
                k = j;
                break;
            }
            if p.segs[j - 1].targs.is_some()
                && let Some(r) = self.find(&Self::key_path(p, j, false))
            {
                head = r;
                k = j;
                pending = true;
                break;
            }
        }
        if k > 0 {
            comps = 1;
        } else if let Some((s, args)) = Self::special(p) {
            head = s.to_string();
            k = 1;
            pending = args;
            comps = 1;
        } else if p.std {
            head = "St".to_string();
        }
        let mut body = String::new();
        let mut pushed = 0;
        if pending {
            let a = self.args(p.segs[k - 1].targs.as_deref().unwrap_or_default());
            body.push_str(&a);
            self.add(Self::key_path(p, k, true));
            pushed += 1;
        }
        for i in k..n {
            let s = &p.segs[i];
            body.push_str(&self.seg_name(s));
            self.add(Self::key_path(p, i + 1, false));
            pushed += 1;
            if let Some(v) = &s.targs {
                let a = self.args(v);
                body.push_str(&a);
                self.add(Self::key_path(p, i + 1, true));
                pushed += 1;
            }
            comps += 1;
        }
        if u == Use::Entity && pushed > 0 {
            self.subs.pop();
        }
        if comps == 1 && quals.is_empty() {
            format!("{head}{body}")
        } else {
            format!("N{quals}{head}{body}E")
        }
    }
}

/// One generated name: the text and whether the scheme must name it.
struct Entry {
    text: String,
    named: bool,
    kind: &'static str,
}

const NS: &[&str] = &["ns", "app", "gfx", "util", "detail", "io"];
const CLS: &[&str] = &["Vec", "Node", "Buffer", "Map", "Handle", "List"];
const FNS: &[&str] = &["get", "set", "run", "size", "push", "find", "reset"];
const TAGS: &[&str] = &["cxx11", "v2"];
const BUILTINS: &[&str] = &["i", "c", "b", "m", "f", "d", "j", "l", "h", "x"];
const OPS: &[&str] = &["pl", "eq", "aS", "ix", "cl", "lt", "ne", "mi"];
const STD_CLS: &[&str] = &["vector", "pair", "unique_ptr", "function", "array"];
const SUFFIXES: &[&str] = &[
    ".cold",
    ".llvm.123456",
    ".isra.0",
    ".part.1",
    ".constprop.0",
    ".llvm.9$x_y",
];
const PLAIN: &[&str] = &["memcpy", "nnMain", "_start", "__cxa_throw", "fn_123", "x9"];

struct Gen {
    rng: Rng,
    scope: u32,
}

impl Gen {
    fn ty(&mut self, depth: usize) -> Ty {
        let r = self.rng.below(if depth > 2 { 3 } else { 14 });
        match r {
            0..=2 => Ty::Builtin(self.rng.pick(BUILTINS)),
            3 => Ty::Ptr(Box::new(self.ty(depth + 1))),
            4 => Ty::LRef(Box::new(Ty::Const(Box::new(self.ty(depth + 1))))),
            5 => Ty::RRef(Box::new(self.ty(depth + 1))),
            6 | 7 => Ty::Class(self.class_path(depth + 1)),
            8 => Ty::Class(self.std_class(depth + 1)),
            9 => std_string(),
            10 => Ty::Ptr(Box::new(Ty::Func(
                Box::new(self.ty(depth + 1)),
                (0..self.rng.below(3)).map(|_| self.ty(depth + 2)).collect(),
            ))),
            11 => Ty::LRef(Box::new(self.ty(depth + 1))),
            12 => Ty::Ptr(Box::new(Ty::Vendor("AS1", Box::new(self.ty(depth + 1))))),
            _ => Ty::Ptr(Box::new(Ty::Const(Box::new(self.ty(depth + 1))))),
        }
    }

    fn targs(&mut self, depth: usize) -> Vec<Arg> {
        (0..1 + self.rng.below(2))
            .map(|_| match self.rng.below(8) {
                0 => Arg::Int(self.rng.below(9) as i64 - 2),
                1 => Arg::Pack(vec![Arg::Ty(self.ty(depth + 1))]),
                _ => Arg::Ty(self.ty(depth + 1)),
            })
            .collect()
    }

    fn class_path(&mut self, depth: usize) -> Path {
        let mut segs = Vec::new();
        for _ in 0..self.rng.below(3) {
            segs.push(src(self.rng.pick(NS)));
        }
        let mut c = src(self.rng.pick(CLS));
        if depth < 3 && self.rng.chance(35) {
            c.targs = Some(self.targs(depth));
        }
        if self.rng.chance(5) {
            c.tags.push(self.rng.pick(TAGS));
        }
        segs.push(c);
        Path {
            scope: 0,
            std: false,
            segs,
        }
    }

    fn std_class(&mut self, depth: usize) -> Path {
        let mut segs = Vec::new();
        if self.rng.chance(30) {
            segs.push(src("__1"));
        }
        match self.rng.below(6) {
            0 => {
                let t = self.ty(depth + 1);
                segs.push(targ("allocator", vec![Arg::Ty(t)]));
            }
            1 => segs.push(targ(
                "basic_string",
                vec![
                    Arg::Ty(Ty::Builtin("w")),
                    Arg::Ty(char_traits()),
                    Arg::Ty(allocator(Ty::Builtin("w"))),
                ],
            )),
            _ => {
                let n = self.rng.pick(STD_CLS);
                let a = if depth < 3 {
                    self.targs(depth)
                } else {
                    vec![Arg::Ty(Ty::Builtin("i"))]
                };
                segs.push(targ(n, a));
            }
        }
        std_path(segs)
    }

    /// A function: its path, qualifiers, template-ness and parameters.
    fn function(&mut self) -> (Path, String, Vec<Ty>, bool) {
        let shape = self.rng.below(12);
        let mut quals = String::new();
        let mut path = match shape {
            0 => Path {
                scope: 0,
                std: false,
                segs: Vec::new(),
            },
            1 => {
                let mut p = self.std_class(0);
                if self.rng.chance(30) {
                    p = std_path(Vec::new());
                }
                p
            }
            2 => match std_string() {
                Ty::Class(p) => p,
                _ => std_path(Vec::new()),
            },
            3 => match stream(
                self.rng
                    .pick(&["basic_ostream", "basic_istream", "basic_iostream"]),
            ) {
                Ty::Class(p) => p,
                _ => std_path(Vec::new()),
            },
            _ => self.class_path(0),
        };
        if !path.segs.is_empty()
            && path.segs.last().is_some_and(|s| {
                CLS.iter()
                    .any(|c| matches!(&s.name, SegName::Src(n) if n == c))
                    || path.std
            })
        {
            if self.rng.chance(25) {
                quals.push('K');
            }
            if self.rng.chance(5) {
                quals.push(if self.rng.chance(50) { 'R' } else { 'O' });
            }
        }
        let is_class = !path.segs.is_empty()
            && !(path.std && path.segs.len() == 1 && path.segs[0].targs.is_none());
        let leaf = match self.rng.below(10) {
            0 if is_class => Seg {
                name: SegName::Ctor(self.rng.pick(&["C1", "C2", "D0", "D1", "D2"])),
                tags: Vec::new(),
                targs: None,
            },
            1 if is_class => Seg {
                name: SegName::Op(self.rng.pick(OPS)),
                tags: Vec::new(),
                targs: None,
            },
            2 if is_class => Seg {
                name: SegName::Conv(Box::new(Ty::Builtin(self.rng.pick(BUILTINS)))),
                tags: Vec::new(),
                targs: None,
            },
            3 if path.segs.is_empty() && !path.std => Seg {
                name: SegName::Internal(self.rng.pick(FNS).to_string()),
                tags: Vec::new(),
                targs: None,
            },
            _ => {
                let mut s = src(self.rng.pick(FNS));
                if self.rng.chance(5) {
                    s.tags.push(self.rng.pick(TAGS));
                }
                s
            }
        };
        let is_ctor = matches!(leaf.name, SegName::Ctor(_) | SegName::Conv(_));
        let dtor = matches!(leaf.name, SegName::Ctor(c) if c.starts_with('D'));
        path.segs.push(leaf);
        let template = !is_ctor && self.rng.chance(25);
        let mut params: Vec<Ty> = if dtor {
            Vec::new()
        } else {
            (0..self.rng.below(4)).map(|_| self.ty(0)).collect()
        };
        if template {
            let mut a = self.targs(1);
            if self.rng.chance(30) {
                a.push(Arg::Pack(vec![
                    Arg::Ty(Ty::Builtin("i")),
                    Arg::Ty(Ty::Builtin("c")),
                ]));
                params.push(Ty::Pack(Box::new(Ty::Param(a.len() - 1))));
            }
            if let Some(last) = path.segs.last_mut() {
                last.targs = Some(a);
            }
            if self.rng.chance(60) {
                params.insert(0, Ty::LRef(Box::new(Ty::Param(0))));
                params.push(Ty::LRef(Box::new(Ty::Param(0))));
            }
        }
        if path.segs.len() == 1 {
            quals.clear();
        }
        if !params.is_empty() && self.rng.chance(40) {
            let again = params[self.rng.below(params.len())].clone();
            params.push(again);
        }
        if !path.segs.is_empty() && path.segs.len() > 1 && self.rng.chance(20) {
            let mut owner = path.clone();
            owner.segs.pop();
            params.push(Ty::LRef(Box::new(Ty::Const(Box::new(Ty::Class(owner))))));
        }
        (path, quals, params, template)
    }

    fn encoding(
        &mut self,
        m: &mut Mangler,
        path: &Path,
        quals: &str,
        ps: &[Ty],
        tmpl: bool,
    ) -> String {
        let name = m.path(path, Use::Entity, quals);
        let ret = if tmpl {
            m.ty(&Ty::Builtin("v"))
        } else {
            String::new()
        };
        format!("{name}{ret}{}", m.params(ps))
    }

    fn entry(&mut self) -> Entry {
        let mut m = Mangler::default();
        let kind = self.rng.below(100);
        match kind {
            0..=49 => {
                let (path, quals, ps, tmpl) = self.function();
                let enc = self.encoding(&mut m, &path, &quals, &ps, tmpl);
                let mut text = format!("_Z{enc}");
                let mut named = true;
                if self.rng.chance(12) {
                    text.push_str(self.rng.pick(SUFFIXES));
                    let leaf = &path.segs[path.segs.len() - 1].name;
                    named = !matches!(leaf, SegName::Ctor(_) | SegName::Conv(_));
                }
                Entry {
                    text,
                    named,
                    kind: "function",
                }
            }
            50..=54 => {
                let mut p = self.class_path(0);
                p.segs.push(src("instance"));
                if p.segs.len() == 1 {
                    p.segs.insert(0, src("ns"));
                }
                Entry {
                    text: format!("_Z{}", m.path(&p, Use::Entity, "")),
                    named: true,
                    kind: "data",
                }
            }
            55..=56 => Entry {
                text: format!("_ZL{}", len_name(self.rng.pick(FNS))),
                named: true,
                kind: "data",
            },
            57..=66 => self.local(&mut m),
            67..=76 => {
                let c = if self.rng.chance(20) {
                    self.std_class(0)
                } else {
                    self.class_path(0)
                };
                let t = self.rng.pick(&["TV", "TI", "TS", "TT"]);
                Entry {
                    text: format!("_Z{t}{}", m.path(&c, Use::Type, "")),
                    named: !(c.segs.len() == 1 && Mangler::special(&c).is_some_and(|s| !s.1)),
                    kind: "special",
                }
            }
            77..=81 => {
                let (path, quals, ps, tmpl) = self.function();
                let off = self.rng.pick(&["h16_", "hn16_", "v0_n24_"]);
                let t = if off.starts_with('v') { "Tv" } else { "Th" };
                let off = off.strip_prefix('v').unwrap_or(off);
                let enc = self.encoding(&mut m, &path, &quals, &ps, tmpl);
                Entry {
                    text: format!("_Z{t}{off}{enc}"),
                    named: true,
                    kind: "thunk",
                }
            }
            82..=84 => Entry {
                text: format!("_ZTI{}", self.rng.pick(&["PKc", "i", "Pv", "PFvvE"])),
                named: false,
                kind: "special",
            },
            85..=89 => {
                let name = self.rng.pick(PLAIN);
                Entry {
                    text: name.to_string(),
                    named: true,
                    kind: "plain",
                }
            }
            _ => {
                let t = self.ty(0);
                let f = self.rng.pick(FNS);
                let p = Path {
                    scope: 0,
                    std: false,
                    segs: vec![targ(f, vec![Arg::Ty(Ty::Builtin("i"))])],
                };
                let name = m.path(&p, Use::Entity, "");
                let a = m.ty(&Ty::Builtin("v"));
                let b = m.params(&[t.clone(), Ty::Param(0), t]);
                Entry {
                    text: format!("_Z{name}{a}{b}"),
                    named: true,
                    kind: "function",
                }
            }
        }
    }

    fn local(&mut self, m: &mut Mangler) -> Entry {
        let (path, quals, ps, tmpl) = self.function();
        if matches!(path.segs.last().map(|s| &s.name), Some(SegName::Ctor(c)) if c.starts_with('D'))
        {
            let enc = self.encoding(m, &path, &quals, &ps, tmpl);
            return Entry {
                text: format!("_Z{enc}"),
                named: true,
                kind: "function",
            };
        }
        let enc = self.encoding(m, &path, &quals, &ps, tmpl);
        self.scope += 1;
        let scope = self.scope;
        let guard = self.rng.chance(20);
        let (entity, named) = match self.rng.below(4) {
            0 => {
                let lam = Seg {
                    name: SegName::Lambda(
                        (0..self.rng.below(2)).map(|_| self.ty(1)).collect(),
                        self.rng.below(2) as u32,
                    ),
                    tags: Vec::new(),
                    targs: None,
                };
                let p = Path {
                    scope,
                    std: false,
                    segs: vec![
                        lam,
                        Seg {
                            name: SegName::Op("cl"),
                            tags: Vec::new(),
                            targs: None,
                        },
                    ],
                };
                (format!("{}v", m.path(&p, Use::Entity, "K")), true)
            }
            1 => {
                let un = Seg {
                    name: SegName::Unnamed(self.rng.below(2) as u32),
                    tags: Vec::new(),
                    targs: None,
                };
                let p = Path {
                    scope,
                    std: false,
                    segs: vec![un, src(self.rng.pick(FNS))],
                };
                (format!("{}v", m.path(&p, Use::Entity, "")), true)
            }
            2 => ("s".to_string(), true),
            _ => {
                let d = if self.rng.chance(30) { "_0" } else { "" };
                (
                    format!("{}{d}", len_name(self.rng.pick(&["counter", "once"]))),
                    true,
                )
            }
        };
        let body = format!("Z{enc}E{entity}");
        if guard && !entity.ends_with('v') && entity != "s" {
            return Entry {
                text: format!("_ZGV{body}"),
                named,
                kind: "guard",
            };
        }
        Entry {
            text: format!("_Z{body}"),
            named,
            kind: "local",
        }
    }
}

/// Names a mangler emits that the generator does not make.
const TRICKY: &[(&str, bool)] = &[
    ("_ZN3foo3barERKS_", true),
    ("_ZNK3foo3bar3bazIiEEvRKS1_S0_", true),
    ("_ZNSt3__16vectorIiNS_9allocatorIiEEE9push_backERKi", true),
    (
        "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm",
        true,
    ),
    ("_ZNSs6appendERKSs", true),
    ("_ZNSsC1ERKSs", true),
    ("_ZNSoD1Ev", true),
    ("_ZNSdD0Ev", true),
    ("_ZNSi4readEPcl", true),
    (
        "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc",
        true,
    ),
    ("_ZSt4swapIiEvRT_S1_", true),
    ("_ZNSt6vectorIiSaIiEE9push_backERKi", true),
    ("_ZNKSt6vectorIiSaIiEE4sizeEv", true),
    ("_ZNSaIcEC2ERKS_", true),
    ("_ZNSbIwSt11char_traitsIwESaIwEE6appendEPKwm", true),
    ("_ZZN3foo3barEvENKUlvE_clEv", true),
    ("_ZZN3foo3barEvENKUlRKiE0_clES2_", true),
    ("_ZZN3foo3barEvE3baz_0", true),
    ("_ZZ3foovE3baz", true),
    ("_ZZ3foovEs", true),
    ("_ZGVZN3foo3barEvE1x", true),
    ("_ZGR3foo_", true),
    ("_ZTW3foo", true),
    ("_ZTH3foo", true),
    ("_ZTVN3foo3barE", true),
    ("_ZTTN3foo3barE", true),
    ("_ZTSN3foo3barE", true),
    ("_ZTISt9exception", true),
    ("_ZTISt12length_error", true),
    ("_ZTVSt9bad_alloc", true),
    ("_ZTISs", true),
    ("_ZTISaIcE", true),
    ("_ZThn8_N3foo3barD1Ev", true),
    ("_ZTv0_n24_N3foo3barD0Ev", true),
    ("_ZTch0_h16_N3foo3barEv", true),
    ("_ZN3foo3barB5cxx11Ev", true),
    (
        "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEPKc",
        true,
    ),
    ("_ZN3fooUt_3barEv", true),
    ("_ZN3foocviEv", true),
    ("_ZN3fooplERKS_", true),
    ("_ZN3fooixEm", true),
    ("_ZN3foo3barIJidEEEvDpT_", true),
    ("_ZN3foo3barILi3EEEvv", true),
    ("_ZN3foo3barILin3EEEvv", true),
    ("_Z3fooILb1EEvv", true),
    ("_Z3fooPFviE", true),
    ("_Z3fooPFvS_E", true),
    ("_Z3fooPU3AS1i", true),
    ("_Z3fooIiEvT_", true),
    ("_Z3fooIiEDTcl3barfp_EET_", true),
    ("_ZN3foo3barEv.llvm.12", true),
    ("_ZN3foo3barEv.cold.1", true),
    ("_ZN3fooC2Ev.cold", false),
    ("_ZN3foo3barEPKc.part.0.isra.0", true),
    ("_ZN5guest3fooEv", true),
    ("_ZN3std3fooEv", false),
    ("_ZTIPKc", false),
    ("_ZTIi", false),
    ("_ZN3fooE", false),
    ("_ZNS_3fooE", true),
    ("_ZNDtfp_E3fooEv", false),
    ("_ZNT_3fooEv", false),
    ("_ZN3foo3barEv$", false),
    ("_ZNKR3foo3barEv", true),
    ("_ZNO3foo3barEv", true),
    ("_ZNVK3foo3barEv", true),
    ("_ZN3foo3barEU8__vectori", true),
    ("_ZN3foo3barEDv4_f", true),
    ("_ZN3foo3barEA4_i", true),
    ("_ZN3foo3barEM3bazFviE", true),
    ("_ZN3foo3barEDn", true),
    ("_ZN3foo3barIiEEvNS0_IT_E3bazE", true),
    ("_Z1fIiEvT_PS0_", true),
    ("memcpy", true),
    ("__aeabi_memcpy8", true),
    ("_Zfoo", true),
    ("fn_00001234", false),
    ("bad-name", false),
];

/// Every corpus name, with whether the scheme must name it.
fn corpus() -> Vec<(String, bool, &'static str)> {
    let mut g = Gen {
        rng: Rng(0x9e37_79b9_7f4a_7c15),
        scope: 0,
    };
    let mut out: Vec<(String, bool, &'static str)> = (0..COUNT)
        .map(|_| {
            let e = g.entry();
            (e.text, e.named, e.kind)
        })
        .collect();
    out.extend(TRICKY.iter().map(|(t, n)| (t.to_string(), *n, "tricky")));
    out.sort();
    out.dedup();
    out
}

#[test]
fn every_corpus_name_round_trips_and_demangles_inside_guest() {
    let modules = ["main", "sdk", "subsdk0"];
    let mut failures = Vec::new();
    let mut checked = 0;
    let mut oracle_checked = 0;
    let mut kinds: BTreeMap<&str, usize> = BTreeMap::new();
    for (i, (name, named, kind)) in corpus().iter().enumerate() {
        let module = modules[i % modules.len()];
        let h = host_name(module, name);
        match (&h, named) {
            (None, true) => failures.push(format!("{module}:{name}: no host name")),
            (Some(h), false) => failures.push(format!("{module}:{name}: named {h}")),
            _ => {}
        }
        let Some(h) = h else { continue };
        checked += 1;
        *kinds.entry(kind).or_default() += 1;
        if h.as_bytes()[0].is_ascii_digit()
            || !h.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_')
        {
            failures.push(format!("{h}: not a C identifier"));
        }
        if guest_name(&h) != Some((module.to_string(), GuestName::Symbol(name.clone()))) {
            failures.push(format!("{module}:{name} -> {h}: round trip"));
        }
        if !name.starts_with("_Z") || oracle::knows(name) {
            oracle_checked += 1;
            if let Err(e) = oracle::check(module, name, &h) {
                failures.push(e);
            }
        }
    }
    println!("corpus: {checked} names checked, {oracle_checked} by the oracle, {kinds:?}");
    assert!(
        checked > COUNT / 2,
        "control: only {checked} names were named"
    );
    assert!(
        oracle_checked * 10 > checked * 9,
        "the oracle knew {oracle_checked} of {checked}"
    );
    assert!(
        failures.is_empty(),
        "{} failures:\n{}",
        failures.len(),
        failures[..failures.len().min(40)].join("\n")
    );
}

/// The oracle must catch a remangler that only inserts the prefix: every
/// name whose references move must then demangle differently.
#[test]
fn the_oracle_catches_a_prefix_without_renumbering() {
    let mut moved = 0;
    let mut caught = 0;
    for (name, named, _) in corpus() {
        if !named || !name.starts_with("_Z") || !oracle::knows(&name) {
            continue;
        }
        let (Some(good), Some(naive)) = (forward("sdk", &name, true), forward("sdk", &name, false))
        else {
            continue;
        };
        if good == naive {
            continue;
        }
        moved += 1;
        if oracle::check("sdk", &name, &naive).is_err() {
            caught += 1;
        }
    }
    println!("control: {moved} names move a reference, the oracle caught {caught}");
    assert!(
        moved > 500,
        "control: only {moved} generated names had references to move"
    );
    assert!(
        caught * 100 >= moved * 95,
        "the oracle caught only {caught} of {moved}"
    );
}

/// Truncated and mutated names never panic either direction.
#[test]
fn damaged_names_do_not_panic() {
    let mut rng = Rng(0x2545_f491_4f6c_dd1d);
    let alphabet = b"_ZNESIT0123456789abcdKLPRVCDtv.$";
    let mut tried = 0;
    for (name, _, _) in corpus().iter().step_by(7) {
        let b = name.as_bytes();
        for cut in 0..b.len() {
            let s = String::from_utf8_lossy(&b[..cut]).into_owned();
            let _ = host_name("sdk", &s);
            let _ = guest_name(&s);
            tried += 1;
        }
        for _ in 0..8 {
            let mut m = b.to_vec();
            let at = rng.below(m.len());
            m[at] = alphabet[rng.below(alphabet.len())];
            let s = String::from_utf8_lossy(&m).into_owned();
            if let Some(h) = host_name("sdk", &s) {
                assert_eq!(
                    guest_name(&h),
                    Some(("sdk".to_string(), GuestName::Symbol(s.clone())))
                );
            }
            let _ = guest_name(&s);
            tried += 1;
        }
    }
    let _ = host_name("sdk", "_ZN3fo\u{e9}3barEv");
    assert!(tried > 10_000, "control: only {tried} damaged names");
}
