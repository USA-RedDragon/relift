//! The sail-arm model as tables: top-level items are split cheaply from the
//! `.sail` files, and function bodies and `val` signatures are parsed lazily.

use std::cell::RefCell;
use std::collections::HashMap;
use std::path::Path;
use std::rc::Rc;

use crate::ast::{Clause, Typ};
use crate::parse::Parser;

/// A `val` signature: parameter and return types, and the extern name.
#[derive(Clone, Debug, PartialEq)]
pub struct Sig {
    /// The parameter types and return type; `None` when the type did not parse.
    /// A value that is not a function has no parameters.
    pub fun: Option<(Vec<Typ>, Typ)>,
    /// The quoted string or braced block of `val f = pure "x" : ...`.
    pub extern_: Option<String>,
}

/// The fields of a bitfield: name to high and low bit.
pub type BitFields = HashMap<String, (u32, u32)>;

/// The parsed clauses of a function, or the parse error.
type Parsed = Result<Rc<Vec<Clause>>, String>;

/// The tables built from the Sail sources.
#[derive(Default)]
pub struct Model {
    /// Function name to the raw text of each clause.
    pub funcs: HashMap<String, Vec<String>>,
    /// Function name to the raw text of its `val`.
    pub vals: HashMap<String, String>,
    /// Register name to its type and initial value text.
    pub registers: HashMap<String, (String, Option<String>)>,
    /// Top-level `let` name to its value text.
    pub lets: HashMap<String, String>,
    /// Enum name to its members.
    pub enums: HashMap<String, Vec<String>>,
    /// Enum member to its enum and position.
    pub enum_of: HashMap<String, (String, usize)>,
    /// Struct name to its fields (name, type text).
    pub structs: HashMap<String, Vec<(String, String)>>,
    /// Bitfield name to its width and fields (name to high and low bit).
    pub bitfields: HashMap<String, (u32, BitFields)>,
    /// Overload name to its candidates; operators are named `op` plus the symbol.
    pub overloads: HashMap<String, Vec<String>>,
    /// Type synonym name to its right-hand side text.
    pub types: HashMap<String, String>,
    /// Union constructor to its union.
    pub unions: HashMap<String, String>,
    /// Functions defined in `stubs.sail`: placeholders for implementation
    /// choices, which the generator must not inline silently.
    pub stubs: std::collections::HashSet<String>,
    parsed: RefCell<HashMap<String, Parsed>>,
    sigs: RefCell<HashMap<String, Option<Rc<Sig>>>>,
}

const START_KWS: &[&str] = &[
    "function",
    "val",
    "register",
    "let",
    "enum",
    "struct",
    "bitfield",
    "union",
    "type",
    "overload",
    "mapping",
    "scattered",
    "end",
    "default",
    "infix",
    "infixl",
    "infixr",
    "termination_measure",
    "outcome",
    "instantiation",
    "newtype",
];

const LIB_FILES: &[&str] = &[
    "flow.sail",
    "arith.sail",
    "vector.sail",
    "real.sail",
    "string.sail",
    "option.sail",
    "reverse_endianness.sail",
    "mono_rewrites.sail",
    "concurrency_interface.sail",
];

/// The model files the A64 generator reads, in load order.
pub const A64_SRC_FILES: &[&str] = &[
    "prelude.sail",
    "decode_start.sail",
    "builtins.sail",
    "v8_base.sail",
    "stubs.sail",
    "interrupts.sail",
    "interface.sail",
    "no_devices.sail",
    "impdefs.sail",
    "mem.sail",
    "sysregs_autogen.sail",
    "sysregs.sail",
    "reset.sail",
    "instrs64.sail",
    "decode_end.sail",
    "fetch.sail",
];

/// Drops `$` directives, and the lines of inactive `$ifdef` and `$ifndef`
/// branches (nothing is defined), keeping line numbering.
fn preprocess(text: &str) -> String {
    let mut out: Vec<&str> = Vec::new();
    let mut stack: Vec<bool> = Vec::new();
    for line in text.split('\n') {
        let s = line.trim();
        if s.starts_with("$ifdef") || s.starts_with("$ifndef") {
            stack.push(s.starts_with("$ifndef"));
            out.push("");
        } else if s.starts_with("$else") {
            if let Some(top) = stack.last_mut() {
                *top = !*top;
            }
            out.push("");
        } else if s.starts_with("$endif") {
            stack.pop();
            out.push("");
        } else if s.starts_with('$') || !stack.iter().all(|a| *a) {
            out.push("");
        } else {
            out.push(line);
        }
    }
    out.join("\n")
}

fn is_word(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

/// Splits text into top-level items: each starts at column 0 with a keyword.
fn split_items(text: &str) -> Vec<String> {
    let starts = |line: &str| {
        START_KWS.iter().any(|kw| {
            line.strip_prefix(kw)
                .is_some_and(|r| r.as_bytes().first().is_none_or(|c| !is_word(*c)))
        })
    };
    let mut items: Vec<String> = Vec::new();
    let mut cur: Vec<&str> = Vec::new();
    let mut started = false;
    for line in text.split('\n') {
        if starts(line) {
            if started {
                items.push(cur.join("\n"));
            }
            cur = vec![line];
            started = true;
        } else {
            cur.push(line);
        }
    }
    if started {
        items.push(cur.join("\n"));
    }
    items
}

fn is_ws(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c)
}

fn skip_ws(b: &[u8], mut p: usize) -> usize {
    while p < b.len() && is_ws(b[p]) {
        p += 1;
    }
    p
}

fn ws1(b: &[u8], p: usize) -> Option<usize> {
    let q = skip_ws(b, p);
    (q > p).then_some(q)
}

/// The end of the identifier `[A-Za-z_][A-Za-z0-9_]*` at `p`.
fn ident(b: &[u8], p: usize) -> Option<usize> {
    if p >= b.len() || !(b[p].is_ascii_alphabetic() || b[p] == b'_') {
        return None;
    }
    let mut e = p + 1;
    while e < b.len() && is_word(b[e]) {
        e += 1;
    }
    Some(e)
}

fn int_of(s: &str) -> Option<u32> {
    s.trim().parse().ok()
}

fn first_word(it: &str) -> &str {
    it.split_whitespace().next().unwrap_or("")
}

impl Model {
    /// Loads the library files in `sail_lib` (missing ones are skipped) and the
    /// model sources `files` in `src`.
    pub fn load(sail_lib: &Path, src: &Path, files: &[&str]) -> std::io::Result<Model> {
        let mut m = Model::default();
        for f in LIB_FILES {
            let p = sail_lib.join(f);
            if p.exists() {
                m.load_file(&p)?;
            }
        }
        for f in files {
            let before: std::collections::HashSet<String> = m.funcs.keys().cloned().collect();
            m.load_file(&src.join(f))?;
            if *f == "stubs.sail" {
                m.stubs = m
                    .funcs
                    .keys()
                    .filter(|k| !before.contains(*k))
                    .cloned()
                    .collect();
            }
        }
        Ok(m)
    }

    fn load_file(&mut self, path: &Path) -> std::io::Result<()> {
        let bytes = std::fs::read(path)?;
        let text = String::from_utf8_lossy(&bytes)
            .replace("\r\n", "\n")
            .replace('\r', "\n");
        for it in split_items(&preprocess(&text)) {
            let kw = first_word(&it).to_string();
            let _ = self.add(&kw, &it);
        }
        Ok(())
    }

    /// Records one top-level item; `None` when it does not have the expected
    /// shape, in which case nothing is recorded.
    fn add(&mut self, kw: &str, it: &str) -> Option<()> {
        match kw {
            "function" => self.add_function(it),
            "val" => self.add_val(it),
            "register" => self.add_register(it),
            "let" => self.add_let(it),
            "enum" => self.add_enum(it),
            "struct" => self.add_struct(it),
            "bitfield" => self.add_bitfield(it),
            "overload" => self.add_overload(it),
            "type" => self.add_type(it),
            "union" => self.add_union(it),
            _ => Some(()),
        }
    }

    fn add_function(&mut self, it: &str) -> Option<()> {
        let b = it.as_bytes();
        let p = ws1(b, "function".len())?;
        let mut name = None;
        if it[p..].starts_with("clause")
            && let Some(q) = ws1(b, p + 6)
            && let Some(e) = ident(b, q)
        {
            name = Some(&it[q..e]);
        }
        let name = match name {
            Some(n) => n,
            None => &it[p..ident(b, p)?],
        };
        self.funcs
            .entry(name.to_string())
            .or_default()
            .push(it.to_string());
        Some(())
    }

    fn add_val(&mut self, it: &str) -> Option<()> {
        let b = it.as_bytes();
        let mut p = ws1(b, 3)?;
        if b.get(p) == Some(&b'"') {
            p += 1;
        }
        let e = ident(b, p)?;
        self.vals.insert(it[p..e].to_string(), it.to_string());
        Some(())
    }

    fn add_register(&mut self, it: &str) -> Option<()> {
        let s = it.trim();
        let b = s.as_bytes();
        let p = ws1(b, "register".len())?;
        let e = ident(b, p)?;
        let name = &s[p..e];
        let p = skip_ws(b, e);
        if b.get(p) != Some(&b':') {
            return None;
        }
        let start = skip_ws(b, p + 1);
        let mut end = start + 1;
        loop {
            let c = *b.get(end - 1)?;
            if c == b'=' || c == b'\n' {
                return None;
            }
            let q = skip_ws(b, end);
            if q == b.len() {
                self.registers
                    .insert(name.to_string(), (s[start..end].trim().to_string(), None));
                return Some(());
            }
            if b[q] == b'=' {
                let v = skip_ws(b, q + 1);
                self.registers.insert(
                    name.to_string(),
                    (s[start..end].trim().to_string(), Some(s[v..].to_string())),
                );
                return Some(());
            }
            end += 1;
        }
    }

    fn add_let(&mut self, it: &str) -> Option<()> {
        let s = it.trim();
        let b = s.as_bytes();
        let p = ws1(b, 3)?;
        let e = ident(b, p)?;
        let name = &s[p..e];
        let mut p = skip_ws(b, e);
        if b.get(p) == Some(&b':') {
            let eq = s[p + 1..].find('=')? + p + 1;
            if eq <= p + 1 {
                return None;
            }
            p = eq;
        }
        if b.get(p) != Some(&b'=') {
            return None;
        }
        let v = skip_ws(b, p + 1);
        self.lets
            .insert(name.to_string(), s[v..].trim().to_string());
        Some(())
    }

    fn add_enum(&mut self, it: &str) -> Option<()> {
        let s = it.trim();
        let b = s.as_bytes();
        let p = ws1(b, 4)?;
        let e = ident(b, p)?;
        let name = &s[p..e];
        let p = skip_ws(b, e);
        if b.get(p) != Some(&b'=') {
            return None;
        }
        let body = &s[skip_ws(b, p + 1)..];
        let inner = match body.find('{') {
            Some(ob) => {
                let cb = body.find('}')?;
                if cb > ob + 1 { &body[ob + 1..cb] } else { "" }
            }
            None => body.split('\n').next().unwrap_or(""),
        };
        let members: Vec<String> = inner
            .split(['|', ','])
            .map(str::trim)
            .filter(|x| !x.is_empty())
            .map(str::to_string)
            .collect();
        for (i, x) in members.iter().enumerate() {
            self.enum_of.insert(x.clone(), (name.to_string(), i));
        }
        self.enums.insert(name.to_string(), members);
        Some(())
    }

    fn add_struct(&mut self, it: &str) -> Option<()> {
        let b = it.as_bytes();
        let p = ws1(b, 6)?;
        let e = ident(b, p)?;
        let body = braced(it)?;
        let fields = body
            .split(',')
            .filter_map(|f| f.split_once(':'))
            .map(|(a, t)| (a.trim().to_string(), t.trim().to_string()))
            .collect();
        self.structs.insert(it[p..e].to_string(), fields);
        Some(())
    }

    fn add_bitfield(&mut self, it: &str) -> Option<()> {
        let b = it.as_bytes();
        let p = ws1(b, "bitfield".len())?;
        let e = ident(b, p)?;
        let name = &it[p..e];
        let p = skip_ws(b, e);
        if b.get(p) != Some(&b':') {
            return None;
        }
        let p = skip_ws(b, p + 1);
        if !it[p..].starts_with("bits(") {
            return None;
        }
        let ds = p + 5;
        let mut de = ds;
        while de < b.len() && b[de].is_ascii_digit() {
            de += 1;
        }
        if de == ds || b.get(de) != Some(&b')') {
            return None;
        }
        let width: u32 = it[ds..de].parse().ok()?;
        let mut fields = HashMap::new();
        for f in braced(it)?.split(',') {
            if let Some((a, t)) = f.split_once(':') {
                let t = t.trim();
                let range = match t.split_once("..") {
                    Some((hi, lo)) => {
                        if lo.contains("..") {
                            return None;
                        }
                        (int_of(hi)?, int_of(lo)?)
                    }
                    None => (int_of(t)?, int_of(t)?),
                };
                fields.insert(a.trim().to_string(), range);
            }
        }
        self.bitfields.insert(name.to_string(), (width, fields));
        Some(())
    }

    fn add_overload(&mut self, it: &str) -> Option<()> {
        let b = it.as_bytes();
        let p = ws1(b, "overload".len())?;
        let after_name = |e: usize| -> Option<usize> {
            let q = skip_ws(b, e);
            if b.get(q) != Some(&b'=') {
                return None;
            }
            let q = skip_ws(b, q + 1);
            (b.get(q) == Some(&b'{')).then_some(q)
        };
        let mut found: Option<(&str, usize)> = None;
        if it[p..].starts_with("operator")
            && let Some(q) = ws1(b, p + 8)
        {
            let mut e = q;
            while e < b.len() && !is_ws(b[e]) {
                e += 1;
            }
            while e > q {
                if let Some(ob) = after_name(e) {
                    found = Some((&it[p..e], ob));
                    break;
                }
                e -= 1;
            }
        }
        if found.is_none() {
            let e = ident(b, p)?;
            found = Some((&it[p..e], after_name(e)?));
        }
        let (raw, ob) = found?;
        let cb = it[ob + 1..].find('}')? + ob + 1;
        let mut name = raw.replace("operator", "").trim().to_string();
        if raw.starts_with("operator") {
            name = format!("op{name}");
        }
        let list = self.overloads.entry(name).or_default();
        list.extend(
            it[ob + 1..cb]
                .split(',')
                .map(str::trim)
                .filter(|x| !x.is_empty())
                .map(str::to_string),
        );
        Some(())
    }

    fn add_type(&mut self, it: &str) -> Option<()> {
        let s = it.trim();
        let b = s.as_bytes();
        let p = ws1(b, 4)?;
        let e = ident(b, p)?;
        let name = &s[p..e];
        let mut p = skip_ws(b, e);
        if b.get(p) == Some(&b'(') {
            p = s[p..].find(')')? + p + 1;
            p = skip_ws(b, p);
        }
        if b.get(p) == Some(&b':') {
            p = skip_ws(b, p + 1);
            let st = p;
            while p < b.len() && is_word(b[p]) {
                p += 1;
            }
            if p == st {
                return None;
            }
            p = skip_ws(b, p);
        }
        if b.get(p) != Some(&b'=') {
            return None;
        }
        let v = skip_ws(b, p + 1);
        self.types
            .insert(name.to_string(), s[v..].trim().to_string());
        Some(())
    }

    fn add_union(&mut self, it: &str) -> Option<()> {
        let b = it.as_bytes();
        let p = ws1(b, 5)?;
        let e = ident(b, p)?;
        let name = &it[p..e];
        for f in braced(it)?.split(',') {
            if let Some((c, _)) = f.split_once(':') {
                self.unions.insert(c.trim().to_string(), name.to_string());
            }
        }
        Some(())
    }

    /// The clauses of function `name`, parsed on first use: parameter pattern,
    /// guard and body. `None` when there is no such function.
    pub fn func(&self, name: &str) -> Option<Result<Rc<Vec<Clause>>, String>> {
        if let Some(r) = self.parsed.borrow().get(name) {
            return Some(r.clone());
        }
        let raws = self.funcs.get(name)?;
        let res = raws
            .iter()
            .map(|raw| parse_clause(raw))
            .collect::<Result<Vec<_>, _>>()
            .map(Rc::new);
        self.parsed
            .borrow_mut()
            .insert(name.to_string(), res.clone());
        Some(res)
    }

    /// The signature of the `val` declaration of `name`.
    pub fn sig(&self, name: &str) -> Option<Rc<Sig>> {
        if let Some(r) = self.sigs.borrow().get(name) {
            return r.clone();
        }
        let raw = self.vals.get(name)?;
        let out = Some(Rc::new(Sig {
            fun: parse_sig_type(raw, name),
            extern_: extern_of(raw),
        }));
        self.sigs.borrow_mut().insert(name.to_string(), out.clone());
        out
    }

    /// Parses every function, in name order: the number that parsed, and the
    /// name and error of each that did not.
    pub fn parse_all(&self) -> (usize, Vec<(String, String)>) {
        let mut names: Vec<&String> = self.funcs.keys().collect();
        names.sort();
        let mut ok = 0;
        let mut bad = Vec::new();
        for name in names {
            match self.func(name) {
                Some(Ok(_)) => ok += 1,
                Some(Err(e)) => bad.push((name.clone(), e)),
                None => {}
            }
        }
        (ok, bad)
    }
}

/// The text between the first `{` and the last `}` of `it`.
fn braced(it: &str) -> Option<&str> {
    let ob = it.find('{')?;
    let cb = it.rfind('}')?;
    Some(if cb > ob + 1 { &it[ob + 1..cb] } else { "" })
}

fn parse_clause(raw: &str) -> Result<Clause, String> {
    let mut p = Parser::new(raw)?;
    p.expect("function")?;
    if p.peek_text() == "clause" {
        p.skip()?;
    }
    p.ident()?;
    let mut guard = None;
    let params;
    if p.at("(") && !p.at_k(")", 1) {
        let save = p.i;
        p.skip()?;
        let first = p.pat()?;
        if p.at("if") {
            p.skip()?;
            guard = Some(p.expr(0)?);
            p.expect(")")?;
            params = first;
        } else {
            p.i = save;
            params = p.pat()?;
        }
    } else {
        params = p.pat()?;
    }
    if p.at("if") {
        p.skip()?;
        guard = Some(p.expr(0)?);
    }
    if p.at("->") || p.at(":") {
        while !p.at("=") {
            p.skip()?;
        }
    }
    p.expect("=")?;
    let body = p.expr(0)?;
    Ok(Clause {
        params,
        guard,
        body,
    })
}

/// Removes every `effect { ... }` from `s`.
fn strip_effects(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = String::new();
    let mut i = 0;
    while i < s.len() {
        if s[i..].starts_with("effect") {
            let q = skip_ws(b, i + 6);
            if b.get(q) == Some(&b'{')
                && let Some(c) = s[q..].find('}')
            {
                i = q + c + 1;
                continue;
            }
        }
        let ch = s[i..].chars().next().unwrap_or(' ');
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

fn parse_sig_type(raw: &str, name: &str) -> Option<(Vec<Typ>, Typ)> {
    let i = raw.find(name)? + name.len();
    let rest = &raw[i..];
    let mut depth = 0i32;
    let mut colon = None;
    for (j, ch) in rest.char_indices() {
        match ch {
            '{' | '(' => depth += 1,
            '}' | ')' => depth -= 1,
            ':' if depth == 0 => {
                colon = Some(j);
                break;
            }
            _ => {}
        }
    }
    let mut body = colon.map_or(String::new(), |c| rest[c + 1..].to_string());
    body = strip_effects(&body);
    let t = body.trim_start();
    if t.starts_with("forall") && t.as_bytes().get(6).is_none_or(|c| !is_word(*c)) {
        let mut depth = 0i32;
        let cs: Vec<(usize, char)> = body.char_indices().collect();
        for (k, &(j, ch)) in cs.iter().enumerate() {
            match ch {
                '(' | '{' => depth += 1,
                ')' | '}' => depth -= 1,
                '.' if depth == 0 && matches!(cs.get(k + 1), Some((_, ' ' | '\n'))) => {
                    body = body[j + 1..].to_string();
                    break;
                }
                _ => {}
            }
        }
    }
    let mut p = Parser::new(&body).ok()?;
    match p.typ().ok()? {
        Typ::Fun(ps, r) => Some((ps, *r)),
        t => Some((Vec::new(), t)),
    }
}

/// The quoted string or braced block of `val f = [pure|impure|monadic] ...`.
fn extern_of(raw: &str) -> Option<String> {
    let b = raw.as_bytes();
    if !raw.starts_with("val") {
        return None;
    }
    let p = ws1(b, 3)?;
    let mut e = p;
    while e < b.len() && is_word(b[e]) {
        e += 1;
    }
    if e == p {
        return None;
    }
    let p = skip_ws(b, e);
    if b.get(p) != Some(&b'=') {
        return None;
    }
    let p = skip_ws(b, p + 1);
    let block_at = |q: usize| -> Option<String> {
        let q = skip_ws(b, q);
        match b.get(q)? {
            b'"' => {
                let c = raw[q + 1..].find('"')?;
                Some(raw[q..q + c + 2].to_string())
            }
            b'{' => {
                let c = raw[q + 1..].find('}')?;
                Some(raw[q..q + c + 2].to_string())
            }
            _ => None,
        }
    };
    for w in ["pure", "impure", "monadic"] {
        if raw[p..].starts_with(w)
            && let Some(r) = block_at(p + w.len())
        {
            return Some(r);
        }
    }
    block_at(p)
}
