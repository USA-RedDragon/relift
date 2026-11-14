//! Tokenizer and recursive-descent parser for the Sail subset the sail-arm
//! model uses. Anything outside that subset is a parse error.

use crate::ast::{Arm, Dir, Expr, Lit, Pat, Stmt, Typ, TypArg};

/// The result of a parse step.
pub(crate) type PResult<T> = Result<T, String>;

/// The kind of a token.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum K {
    /// `0b...`.
    Bin,
    /// `0x...`.
    Hex,
    /// A decimal integer.
    Num,
    /// A quoted string, quotes included.
    Str,
    /// A decimal real.
    Real,
    /// A name.
    Id,
    /// A keyword.
    Kw,
    /// An operator or punctuation.
    Op,
    /// The end of input.
    Eof,
}

/// A token: its kind and text.
#[derive(Clone, Debug)]
pub(crate) struct Tok {
    /// The kind.
    pub k: K,
    /// The text as written (empty for the end marker).
    pub v: String,
}

const KEYWORDS: &[&str] = &[
    "if",
    "then",
    "else",
    "match",
    "let",
    "in",
    "foreach",
    "from",
    "to",
    "downto",
    "by",
    "while",
    "do",
    "repeat",
    "until",
    "return",
    "throw",
    "assert",
    "try",
    "catch",
    "struct",
    "with",
    "undefined",
    "true",
    "false",
    "var",
    "sizeof",
    "constraint",
    "forall",
    "as",
    "function",
    "val",
    "register",
    "inc",
    "dec",
    "ref",
    "exit",
    "bitzero",
    "bitone",
];

const OPS2: &[&str] = &["==", "!=", "<=", ">=", "<<", ">>", "->", "=>", "..", "++"];
const OPS1: &str = "-+*/^&|<>=:,;(){}[].@~%!";

fn is_id_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_' || c == '?'
}

fn is_id_rest(c: char) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, '_' | '\'' | '?' | '#')
}

/// Splits text into tokens, dropping whitespace and comments.
pub(crate) fn tokenize(s: &str) -> PResult<Vec<Tok>> {
    let ch: Vec<char> = s.chars().collect();
    let n = ch.len();
    let mut out = Vec::new();
    let mut pos = 0;
    let tok = |k: K, a: usize, b: usize| Tok {
        k,
        v: ch[a..b].iter().collect(),
    };
    while pos < n {
        let c = ch[pos];
        if c.is_whitespace() {
            while pos < n && ch[pos].is_whitespace() {
                pos += 1;
            }
            continue;
        }
        if c == '/' && pos + 1 < n && ch[pos + 1] == '/' {
            while pos < n && ch[pos] != '\n' {
                pos += 1;
            }
            continue;
        }
        if c == '/' && pos + 1 < n && ch[pos + 1] == '*' {
            let mut j = pos + 2;
            let mut end = None;
            while j + 1 < n {
                if ch[j] == '*' && ch[j + 1] == '/' {
                    end = Some(j + 2);
                    break;
                }
                j += 1;
            }
            if let Some(e) = end {
                pos = e;
                continue;
            }
        }
        if c.is_ascii_digit() {
            let mut j = pos;
            while j < n && ch[j].is_ascii_digit() {
                j += 1;
            }
            if j + 1 < n && ch[j] == '.' && ch[j + 1].is_ascii_digit() {
                let mut e = j + 1;
                while e < n && ch[e].is_ascii_digit() {
                    e += 1;
                }
                out.push(tok(K::Real, pos, e));
                pos = e;
                continue;
            }
            if c == '0'
                && pos + 2 < n
                && ch[pos + 1] == 'b'
                && matches!(ch[pos + 2], '0' | '1' | '_')
            {
                let mut e = pos + 2;
                while e < n && matches!(ch[e], '0' | '1' | '_') {
                    e += 1;
                }
                out.push(tok(K::Bin, pos, e));
                pos = e;
                continue;
            }
            if c == '0'
                && pos + 2 < n
                && ch[pos + 1] == 'x'
                && (ch[pos + 2].is_ascii_hexdigit() || ch[pos + 2] == '_')
            {
                let mut e = pos + 2;
                while e < n && (ch[e].is_ascii_hexdigit() || ch[e] == '_') {
                    e += 1;
                }
                out.push(tok(K::Hex, pos, e));
                pos = e;
                continue;
            }
            out.push(tok(K::Num, pos, j));
            pos = j;
            continue;
        }
        if c == '"' {
            let mut j = pos + 1;
            let mut end = None;
            while j < n {
                if ch[j] == '\\' {
                    if j + 1 < n {
                        j += 2;
                    } else {
                        break;
                    }
                } else if ch[j] == '"' {
                    end = Some(j + 1);
                    break;
                } else {
                    j += 1;
                }
            }
            if let Some(e) = end {
                out.push(tok(K::Str, pos, e));
                pos = e;
                continue;
            }
        }
        let id_at = if c == '\'' { pos + 1 } else { pos };
        if id_at < n && is_id_start(ch[id_at]) {
            let mut e = id_at + 1;
            while e < n && is_id_rest(ch[e]) {
                e += 1;
            }
            let t = tok(K::Id, pos, e);
            if KEYWORDS.contains(&t.v.as_str()) {
                out.push(Tok { k: K::Kw, v: t.v });
            } else {
                out.push(t);
            }
            pos = e;
            continue;
        }
        let rest = |p: &str| {
            p.chars()
                .enumerate()
                .all(|(i, x)| pos + i < n && ch[pos + i] == x)
        };
        if rest("<-->") {
            out.push(tok(K::Op, pos, pos + 4));
            pos += 4;
            continue;
        }
        if rest("-->") {
            out.push(tok(K::Op, pos, pos + 3));
            pos += 3;
            continue;
        }
        if let Some(op) = OPS2.iter().find(|op| rest(op)) {
            out.push(tok(K::Op, pos, pos + op.len()));
            pos += op.len();
            continue;
        }
        if OPS1.contains(c) {
            out.push(tok(K::Op, pos, pos + 1));
            pos += 1;
            continue;
        }
        let near: String = ch[pos..n.min(pos + 40)].iter().collect();
        return Err(format!("bad char {c:?} at {pos}: {near:?}"));
    }
    out.push(Tok {
        k: K::Eof,
        v: String::new(),
    });
    Ok(out)
}

/// The precedence and associativity (`l`, `r` or `n`) of a binary operator.
fn binop(op: &str) -> Option<(u8, char)> {
    Some(match op {
        "-->" => (1, 'r'),
        "<-->" => (1, 'n'),
        "|" => (2, 'r'),
        "&" => (3, 'r'),
        "==" | "!=" | "<" | ">" | "<=" | ">=" => (4, 'n'),
        "++" => (4, 'l'),
        "@" => (5, 'r'),
        "+" | "-" => (6, 'l'),
        "*" | "/" | "%" | "<<" | ">>" => (7, if matches!(op, "<<" | ">>") { 'n' } else { 'l' }),
        "^" => (8, 'r'),
        _ => return None,
    })
}

/// Turns the target of a `var` binding into an assignable expression.
fn expr_of_pat(p: Pat) -> Expr {
    match p {
        Pat::Wild => Expr::Id("_".into()),
        Pat::Lit(l) => Expr::Lit(l),
        Pat::Id(n) => Expr::Id(n),
        Pat::App(n, ps) => Expr::Call(n, ps.into_iter().map(expr_of_pat).collect()),
        Pat::Tuple(ps) => Expr::Tuple(ps.into_iter().map(expr_of_pat).collect()),
        Pat::Vec(ps) => Expr::VecLit(ps.into_iter().map(expr_of_pat).collect()),
        Pat::Concat(ps) => {
            let mut it = ps.into_iter().map(expr_of_pat).rev();
            let mut acc = it.next().unwrap_or(Expr::Lit(Lit::Unit));
            for e in it {
                acc = Expr::Binop("@".into(), Box::new(e), Box::new(acc));
            }
            acc
        }
        Pat::Typed(p, _) | Pat::As(p, _) => expr_of_pat(*p),
    }
}

/// A recursive-descent parser over a token list.
pub(crate) struct Parser {
    t: Vec<Tok>,
    /// The index of the next token.
    pub i: usize,
}

impl Parser {
    /// A parser over the tokens of `text`.
    pub(crate) fn new(text: &str) -> PResult<Parser> {
        Ok(Parser {
            t: tokenize(text)?,
            i: 0,
        })
    }

    fn peek(&self, k: usize) -> &Tok {
        let j = (self.i + k).min(self.t.len() - 1);
        &self.t[j]
    }

    fn next(&mut self) -> PResult<Tok> {
        match self.t.get(self.i) {
            Some(t) => {
                let t = t.clone();
                self.i += 1;
                Ok(t)
            }
            None => Err("unexpected end of input".into()),
        }
    }

    /// Whether the token `k` ahead is the operator or keyword `v`.
    pub(crate) fn at_k(&self, v: &str, k: usize) -> bool {
        let t = self.peek(k);
        t.v == v && matches!(t.k, K::Op | K::Kw)
    }

    /// Whether the next token is the operator or keyword `v`.
    pub(crate) fn at(&self, v: &str) -> bool {
        self.at_k(v, 0)
    }

    /// The text of the next token.
    pub(crate) fn peek_text(&self) -> &str {
        &self.peek(0).v
    }

    /// Consumes a token that must have text `v`.
    pub(crate) fn expect(&mut self, v: &str) -> PResult<Tok> {
        let tok = self.next()?;
        if tok.v != v {
            return Err(format!(
                "expected {v:?} got {:?} near {}",
                tok.v,
                self.ctx()
            ));
        }
        Ok(tok)
    }

    /// Skips the next token.
    pub(crate) fn skip(&mut self) -> PResult<()> {
        self.next().map(|_| ())
    }

    fn ctx(&self) -> String {
        let a = self.i.saturating_sub(8);
        let b = (self.i + 8).min(self.t.len());
        self.t[a.min(b)..b]
            .iter()
            .map(|t| t.v.as_str())
            .collect::<Vec<_>>()
            .join(" ")
    }

    /// Consumes a name.
    pub(crate) fn ident(&mut self) -> PResult<String> {
        let tok = self.next()?;
        if tok.k != K::Id {
            return Err(format!(
                "expected identifier got {:?} near {}",
                tok.v,
                self.ctx()
            ));
        }
        Ok(tok.v)
    }

    /// A type.
    pub(crate) fn typ(&mut self) -> PResult<Typ> {
        if self.peek(0).v == "{" {
            let mut depth = 0;
            loop {
                let tk = self.next()?;
                if tk.v == "{" {
                    depth += 1;
                } else if tk.v == "}" {
                    depth -= 1;
                    if depth == 0 {
                        break;
                    }
                }
            }
            return Ok(Typ::Set);
        }
        if self.peek(0).v == "(" {
            self.skip()?;
            let mut ts = vec![self.typ()?];
            while self.at(",") {
                self.skip()?;
                ts.push(self.typ()?);
            }
            self.expect(")")?;
            if self.at("->") {
                self.skip()?;
                let r = self.typ()?;
                return Ok(Typ::Fun(ts, Box::new(r)));
            }
            return Ok(if ts.len() == 1 {
                ts.remove(0)
            } else {
                Typ::Tuple(ts)
            });
        }
        let name = self.next()?;
        if !matches!(name.k, K::Id | K::Kw | K::Num) {
            return Err(format!("bad type {:?} near {}", name.v, self.ctx()));
        }
        let t = if self.at("(") {
            self.skip()?;
            let mut args = Vec::new();
            if !self.at(")") {
                args.push(self.typarg()?);
                while self.at(",") {
                    self.skip()?;
                    args.push(self.typarg()?);
                }
            }
            self.expect(")")?;
            Typ::App(name.v, args)
        } else {
            Typ::Id(name.v)
        };
        if self.at("->") {
            self.skip()?;
            let r = self.typ()?;
            return Ok(Typ::Fun(vec![t], Box::new(r)));
        }
        Ok(t)
    }

    fn typarg(&mut self) -> PResult<TypArg> {
        let save = self.i;
        match self.expr(0) {
            Ok(e) => Ok(TypArg::Expr(e)),
            Err(_) => {
                self.i = save;
                Ok(TypArg::Typ(self.typ()?))
            }
        }
    }

    /// A pattern.
    pub(crate) fn pat(&mut self) -> PResult<Pat> {
        let mut p = self.pat_atom()?;
        loop {
            if self.at("@") {
                self.skip()?;
                let q = self.pat_atom()?;
                p = match p {
                    Pat::Concat(mut ps) => {
                        ps.push(q);
                        Pat::Concat(ps)
                    }
                    p => Pat::Concat(vec![p, q]),
                };
            } else if self.at(":") {
                self.skip()?;
                let t = self.typ()?;
                p = Pat::Typed(Box::new(p), t);
            } else if self.at("as") {
                self.skip()?;
                let nm = self.ident()?;
                p = Pat::As(Box::new(p), nm);
            } else {
                break;
            }
        }
        Ok(p)
    }

    fn pat_list(&mut self, close: &str) -> PResult<Vec<Pat>> {
        let mut ps = Vec::new();
        if !self.at(close) {
            ps.push(self.pat()?);
            while self.at(",") {
                self.skip()?;
                ps.push(self.pat()?);
            }
        }
        self.expect(close)?;
        Ok(ps)
    }

    fn pat_atom(&mut self) -> PResult<Pat> {
        let tok = self.peek(0).clone();
        if tok.v == "(" {
            self.skip()?;
            if self.at(")") {
                self.skip()?;
                return Ok(Pat::Lit(Lit::Unit));
            }
            let mut ps = vec![self.pat()?];
            while self.at(",") {
                self.skip()?;
                ps.push(self.pat()?);
            }
            self.expect(")")?;
            return Ok(if ps.len() == 1 {
                ps.remove(0)
            } else {
                Pat::Tuple(ps)
            });
        }
        if tok.v == "[" {
            self.skip()?;
            return Ok(Pat::Vec(self.pat_list("]")?));
        }
        if matches!(tok.k, K::Bin | K::Hex | K::Num | K::Str | K::Real)
            || matches!(tok.v.as_str(), "true" | "false" | "bitzero" | "bitone")
        {
            return Ok(Pat::Lit(self.lit()?));
        }
        if tok.k == K::Id {
            self.skip()?;
            if tok.v == "_" {
                return Ok(Pat::Wild);
            }
            if self.at("(") {
                self.skip()?;
                return Ok(Pat::App(tok.v, self.pat_list(")")?));
            }
            return Ok(Pat::Id(tok.v));
        }
        if tok.v == "-" && self.peek(1).k == K::Num {
            self.skip()?;
            let v = self.next()?.v;
            let n: i128 = v.parse().map_err(|_| format!("bad integer {v}"))?;
            return Ok(Pat::Lit(Lit::Int(-n)));
        }
        Err(format!("bad pattern {:?} near {}", tok.v, self.ctx()))
    }

    /// A literal.
    pub(crate) fn lit(&mut self) -> PResult<Lit> {
        let tok = self.next()?;
        let v = tok.v;
        match tok.k {
            K::Bin => {
                let b: String = v[2..].chars().filter(|c| *c != '_').collect();
                let value =
                    u128::from_str_radix(&b, 2).map_err(|e| format!("bad literal {v}: {e}"))?;
                Ok(Lit::Bits {
                    width: b.len() as u32,
                    value,
                })
            }
            K::Hex => {
                let h: String = v[2..].chars().filter(|c| *c != '_').collect();
                if h.len() > 32 {
                    if !h.len().is_multiple_of(32) {
                        return Err(format!("wide literal {v} is not whole 128-bit parts"));
                    }
                    let mut parts = Vec::new();
                    for i in (0..h.len() / 32).rev() {
                        let p = &h[32 * i..32 * i + 32];
                        parts.push(
                            u128::from_str_radix(p, 16)
                                .map_err(|e| format!("bad literal {v}: {e}"))?,
                        );
                    }
                    return Ok(Lit::Wide(parts));
                }
                let value =
                    u128::from_str_radix(&h, 16).map_err(|e| format!("bad literal {v}: {e}"))?;
                Ok(Lit::Bits {
                    width: 4 * h.len() as u32,
                    value,
                })
            }
            K::Num => v
                .parse()
                .map(Lit::Int)
                .map_err(|_| format!("bad integer {v}")),
            K::Real => Ok(Lit::Real(v)),
            K::Str => Ok(Lit::Str(v[1..v.len() - 1].to_string())),
            _ => match v.as_str() {
                "true" => Ok(Lit::Bool(true)),
                "false" => Ok(Lit::Bool(false)),
                "bitzero" => Ok(Lit::Bit(0)),
                "bitone" => Ok(Lit::Bit(1)),
                _ => Err(format!("bad literal {v:?}")),
            },
        }
    }

    fn block(&mut self) -> PResult<Expr> {
        self.expect("{")?;
        let mut stmts = Vec::new();
        while !self.at("}") {
            stmts.push(self.stmt()?);
            if self.at(";") {
                self.skip()?;
            } else if !self.at("}") {
                return Err(format!("expected ; or }} near {}", self.ctx()));
            }
        }
        self.expect("}")?;
        Ok(Expr::Block(stmts))
    }

    fn stmt(&mut self) -> PResult<Stmt> {
        if self.at("let") {
            self.skip()?;
            let p = self.pat()?;
            self.expect("=")?;
            let e = self.expr(0)?;
            if self.at("in") {
                self.skip()?;
                let body = self.expr(0)?;
                return Ok(Stmt::Expr(Expr::LetIn(p, Box::new(e), Box::new(body))));
            }
            return Ok(Stmt::Let(p, e));
        }
        if self.at("var") {
            self.skip()?;
            let p = self.pat()?;
            self.expect("=")?;
            let e = self.expr(0)?;
            return Ok(match p {
                Pat::Typed(p, t) => Stmt::Decl(expr_of_pat(*p), t, e),
                p => Stmt::Assign(expr_of_pat(p), e),
            });
        }
        let e = self.expr(0)?;
        if self.at("=") {
            self.skip()?;
            let rhs = self.expr(0)?;
            return Ok(Stmt::Assign(e, rhs));
        }
        if self.at(":") {
            self.skip()?;
            let t = self.typ()?;
            self.expect("=")?;
            let rhs = self.expr(0)?;
            return Ok(Stmt::Decl(e, t, rhs));
        }
        Ok(Stmt::Expr(e))
    }

    /// An expression whose binary operators bind at least as tightly as `minprec`.
    pub(crate) fn expr(&mut self, minprec: u8) -> PResult<Expr> {
        let mut lhs = self.unary()?;
        loop {
            let tok = self.peek(0);
            if tok.k != K::Op {
                break;
            }
            let Some((prec, assoc)) = binop(&tok.v) else {
                break;
            };
            if prec < minprec {
                break;
            }
            let op = tok.v.clone();
            self.skip()?;
            let nxt = if assoc == 'l' || assoc == 'n' {
                prec + 1
            } else {
                prec
            };
            let rhs = self.expr(nxt)?;
            lhs = Expr::Binop(op, Box::new(lhs), Box::new(rhs));
        }
        Ok(lhs)
    }

    fn unary(&mut self) -> PResult<Expr> {
        if self.at("-") {
            self.skip()?;
            let e = self.unary()?;
            return Ok(Expr::Call("negate".into(), vec![e]));
        }
        if self.at("~") {
            self.skip()?;
            let e = self.unary()?;
            return Ok(Expr::Call("not_bool".into(), vec![e]));
        }
        let p = self.primary()?;
        self.postfix(p)
    }

    fn postfix(&mut self, mut e: Expr) -> PResult<Expr> {
        loop {
            if self.at(".") {
                self.skip()?;
                let f = self.ident()?;
                e = Expr::Field(Box::new(e), f);
            } else if self.at("[") {
                self.skip()?;
                let a = self.expr(0)?;
                if self.at("..") {
                    self.skip()?;
                    let b = self.expr(0)?;
                    self.expect("]")?;
                    e = Expr::Slice(Box::new(e), Box::new(a), Box::new(b));
                } else {
                    self.expect("]")?;
                    e = Expr::Index(Box::new(e), Box::new(a));
                }
            } else if self.at("(") && matches!(e, Expr::Id(_)) {
                let args = self.args()?;
                let Expr::Id(name) = e else { unreachable!() };
                e = Expr::Call(name, args);
            } else {
                break;
            }
        }
        Ok(e)
    }

    fn args(&mut self) -> PResult<Vec<Expr>> {
        self.expect("(")?;
        let mut args = Vec::new();
        if !self.at(")") {
            args.push(self.expr(0)?);
            while self.at(",") {
                self.skip()?;
                args.push(self.expr(0)?);
            }
        }
        self.expect(")")?;
        Ok(args)
    }

    fn skip_parens(&mut self) -> PResult<()> {
        self.expect("(")?;
        let mut depth = 1;
        while depth > 0 {
            let t = self.next()?;
            if t.v == "(" {
                depth += 1;
            } else if t.v == ")" {
                depth -= 1;
            }
        }
        Ok(())
    }

    fn typed_suffix(&self) -> bool {
        self.at(":") && self.peek(1).k == K::Id && !self.at_k("=", 2)
    }

    fn arms(&mut self) -> PResult<Vec<Arm>> {
        self.expect("{")?;
        let mut arms = Vec::new();
        while !self.at("}") {
            let pat = self.pat()?;
            let mut guard = None;
            if self.at("if") {
                self.skip()?;
                guard = Some(self.expr(0)?);
            }
            self.expect("=>")?;
            let body = self.expr(0)?;
            arms.push(Arm { pat, guard, body });
            if self.at(",") {
                self.skip()?;
            }
        }
        self.expect("}")?;
        Ok(arms)
    }

    fn primary(&mut self) -> PResult<Expr> {
        let tok = self.peek(0).clone();
        let (k, v) = (tok.k, tok.v.as_str());
        if matches!(k, K::Bin | K::Hex | K::Num | K::Str | K::Real)
            || matches!(v, "true" | "false" | "bitzero" | "bitone")
        {
            return Ok(Expr::Lit(self.lit()?));
        }
        if v == "(" && k == K::Op {
            self.skip()?;
            if self.at(")") {
                self.skip()?;
                return Ok(Expr::Lit(Lit::Unit));
            }
            let e = self.expr(0)?;
            if self.at(":") {
                self.skip()?;
                let t = self.typ()?;
                self.expect(")")?;
                return Ok(Expr::Typed(Box::new(e), t));
            }
            if self.at(",") {
                let mut es = vec![e];
                while self.at(",") {
                    self.skip()?;
                    es.push(self.expr(0)?);
                }
                self.expect(")")?;
                return Ok(Expr::Tuple(es));
            }
            self.expect(")")?;
            if self.typed_suffix() {
                self.skip()?;
                let t = self.typ()?;
                return Ok(Expr::Typed(Box::new(e), t));
            }
            return Ok(e);
        }
        if v == "{" && k == K::Op {
            let save = self.i;
            self.skip()?;
            if self.peek(0).k == K::Id && self.at_k("with", 1) {
                let base = Expr::Id(self.next()?.v);
                self.skip()?;
                let fs = self.fieldassigns()?;
                return Ok(Expr::SUpdate(Box::new(base), fs));
            }
            self.i = save;
            return self.block();
        }
        if v == "[" && k == K::Op {
            self.skip()?;
            let first = self.expr(0)?;
            if self.at("with") {
                self.skip()?;
                let mut ups = Vec::new();
                loop {
                    let a = self.expr(0)?;
                    let mut b = None;
                    if self.at("..") {
                        self.skip()?;
                        b = Some(self.expr(0)?);
                    }
                    self.expect("=")?;
                    let val = self.expr(0)?;
                    ups.push((a, b, val));
                    if self.at(",") {
                        self.skip()?;
                        continue;
                    }
                    break;
                }
                self.expect("]")?;
                return Ok(Expr::VUpdate(Box::new(first), ups));
            }
            let mut es = vec![first];
            while self.at(",") {
                self.skip()?;
                es.push(self.expr(0)?);
            }
            self.expect("]")?;
            if self.typed_suffix() {
                self.skip()?;
                let t = self.typ()?;
                return Ok(Expr::Typed(Box::new(Expr::VecLit(es)), t));
            }
            return Ok(Expr::VecLit(es));
        }
        if k == K::Kw {
            match v {
                "if" => {
                    self.skip()?;
                    let c = self.expr(0)?;
                    self.expect("then")?;
                    let t = self.expr(0)?;
                    let mut el = None;
                    if self.at("else") {
                        self.skip()?;
                        el = Some(Box::new(self.expr(0)?));
                    }
                    return Ok(Expr::If(Box::new(c), Box::new(t), el));
                }
                "match" => {
                    self.skip()?;
                    let e = self.expr(0)?;
                    let arms = self.arms()?;
                    return Ok(Expr::Match(Box::new(e), arms));
                }
                "foreach" => {
                    self.skip()?;
                    self.expect("(")?;
                    let var = self.ident()?;
                    self.expect("from")?;
                    let from = self.expr(0)?;
                    let mut dir = if self.next()?.v == "downto" {
                        Dir::Down
                    } else {
                        Dir::Up
                    };
                    let to = self.expr(0)?;
                    let mut step = Expr::Lit(Lit::Int(1));
                    if self.at("by") {
                        self.skip()?;
                        step = self.expr(0)?;
                    }
                    if self.at("in") {
                        self.skip()?;
                        if self.next()?.v == "dec" {
                            dir = Dir::Down;
                        }
                    }
                    self.expect(")")?;
                    let body = self.expr(0)?;
                    return Ok(Expr::Foreach {
                        var,
                        from: Box::new(from),
                        dir,
                        to: Box::new(to),
                        step: Box::new(step),
                        body: Box::new(body),
                    });
                }
                "while" => {
                    self.skip()?;
                    let c = self.expr(0)?;
                    self.expect("do")?;
                    let body = self.expr(0)?;
                    return Ok(Expr::While(Box::new(c), Box::new(body)));
                }
                "repeat" => {
                    self.skip()?;
                    let body = self.expr(0)?;
                    self.expect("until")?;
                    let c = self.expr(0)?;
                    return Ok(Expr::Repeat(Box::new(body), Box::new(c)));
                }
                "return" => {
                    self.skip()?;
                    return Ok(Expr::Return(Box::new(self.expr(0)?)));
                }
                "throw" => {
                    self.skip()?;
                    return Ok(Expr::Throw(Box::new(self.expr(0)?)));
                }
                "assert" => {
                    self.skip()?;
                    self.skip_parens()?;
                    return Ok(Expr::Lit(Lit::Unit));
                }
                "exit" => {
                    self.skip()?;
                    self.args()?;
                    return Ok(Expr::Exit);
                }
                "try" => {
                    self.skip()?;
                    let e = self.expr(0)?;
                    self.expect("catch")?;
                    let arms = self.arms()?;
                    return Ok(Expr::Try(Box::new(e), arms));
                }
                "undefined" => {
                    self.skip()?;
                    return Ok(Expr::Undefined);
                }
                "struct" => {
                    self.skip()?;
                    self.expect("{")?;
                    return Ok(Expr::Struct(self.fieldassigns()?));
                }
                "sizeof" => {
                    self.skip()?;
                    self.expect("(")?;
                    let e = self.expr(0)?;
                    self.expect(")")?;
                    return Ok(e);
                }
                "constraint" => {
                    self.skip()?;
                    self.skip_parens()?;
                    return Ok(Expr::Lit(Lit::Bool(true)));
                }
                "let" => {
                    self.skip()?;
                    let p = self.pat()?;
                    self.expect("=")?;
                    let e = self.expr(0)?;
                    self.expect("in")?;
                    let body = self.expr(0)?;
                    return Ok(Expr::LetIn(p, Box::new(e), Box::new(body)));
                }
                "ref" => {
                    self.skip()?;
                    return Ok(Expr::Id(self.ident()?));
                }
                _ => {}
            }
        }
        if k == K::Id {
            self.skip()?;
            return Ok(Expr::Id(tok.v));
        }
        Err(format!("unexpected {:?} near {}", tok.v, self.ctx()))
    }

    fn fieldassigns(&mut self) -> PResult<Vec<(String, Expr)>> {
        let mut fs = Vec::new();
        while !self.at("}") {
            let f = self.ident()?;
            self.expect("=")?;
            let e = self.expr(0)?;
            fs.push((f, e));
            if self.at(",") {
                self.skip()?;
            }
        }
        self.expect("}")?;
        Ok(fs)
    }
}

/// Parses `text` as an expression.
pub fn parse_expr(text: &str) -> Result<Expr, String> {
    Parser::new(text)?.expr(0)
}

/// Parses `text` as a type.
pub fn parse_typ(text: &str) -> Result<Typ, String> {
    Parser::new(text)?.typ()
}
