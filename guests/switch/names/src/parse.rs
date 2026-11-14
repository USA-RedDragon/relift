//! A recognizer for Itanium C++ mangled names.
//!
//! It walks the grammar of the Itanium C++ ABI and records what a remangler
//! needs, without building a tree to print: every substitution candidate in
//! the order the demangler adds it, as a byte span of the input; every
//! numbered substitution reference; and the shape of the names that decide
//! where a prefix and a tag go.
//!
//! The points where candidates are added follow LLVM's
//! `llvm/Demangle/ItaniumDemangle.h` (Apache-2.0 WITH LLVM-exception), whose
//! structure this module ports: nested-name prefixes, unscoped template
//! names, every type that is not a builtin, template parameters, decltypes,
//! vendor types and ABI-tagged special substitutions.
//!
//! References are opaque terminals: what one points to never changes how the
//! rest parses, so a text whose indices are stale still parses to the same
//! candidate list. Strict mode also checks that each index names a candidate
//! that already exists.

/// The deepest recursion the recognizer allows, so a hostile name cannot
/// overflow the stack.
const MAX_DEPTH: usize = 192;

/// The text does not parse.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct Fail;

type R<T> = Result<T, Fail>;

/// A byte range of the parsed text.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct Span {
    pub start: usize,
    pub end: usize,
}

/// One `S_` or `S<seq>_` reference.
#[derive(Debug, Clone, Copy)]
pub(crate) struct SubRef {
    /// Where the reference text sits.
    pub span: Span,
    /// The candidate it names: 0 for `S_`, `seq + 1` for `S<seq>_`.
    pub index: usize,
}

/// What an unqualified component of a name is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum CompKind {
    /// A source name; `internal` when an `L` precedes it.
    Source { internal: bool },
    /// `St` at the start of a nested name.
    Std,
    /// One of `Sa`, `Sb`, `Ss`, `Si`, `So`, `Sd`, by its second letter.
    Special(u8),
    /// A numbered substitution.
    Subst,
    /// A template parameter.
    TemplateParam,
    /// A `Dt` or `DT` decltype.
    Decltype,
    /// An operator name other than a conversion.
    Operator,
    /// A conversion operator, `cv <type>`.
    Conversion,
    /// A constructor or destructor.
    CtorDtor,
    /// `Ut`, `Ul` or `Ub`.
    Unnamed,
    /// A structured binding, `DC ... E`.
    Binding,
}

/// One component of a name.
#[derive(Debug, Clone)]
pub(crate) struct Comp {
    pub start: usize,
    /// The end of the component and its ABI tags, before template arguments.
    pub end: usize,
    pub kind: CompKind,
    /// Each ABI tag, from its `B` to the end of its source name.
    pub tags: Vec<Span>,
    /// The template arguments that follow, from `I` to `E`.
    pub targs: Option<Span>,
}

/// The shape of a `<name>`.
#[derive(Debug, Clone)]
pub(crate) enum Form {
    /// `N [qualifiers] <components> E`.
    Nested {
        /// Any of `H`, `r`, `V`, `K`, `R`, `O` follows the `N`.
        quals: bool,
        /// Where the first component starts.
        p0: usize,
        comps: Vec<Comp>,
        /// The position of the closing `E`.
        e_pos: usize,
    },
    /// `[St] <unqualified-name> [<template-args>]`, or a substitution with
    /// template arguments.
    Unscoped { std: bool, comp: Comp },
    /// `Z <encoding> E <entity>`; no entity for a string literal.
    Local {
        encoding: Box<Top>,
        entity: Option<Box<Name>>,
    },
}

/// A `<name>` and where it sits.
#[derive(Debug, Clone)]
pub(crate) struct Name {
    pub start: usize,
    pub end: usize,
    pub form: Form,
}

/// What a special name holds that could carry the prefix.
#[derive(Debug, Clone)]
pub(crate) enum Inner {
    /// Thunks: an encoding.
    Encoding(Box<Top>),
    /// Guard variables, reference temporaries, thread-local routines.
    Name(Name),
    /// Virtual tables, VTTs, typeinfo: a class type.
    Type(Name),
}

/// An `<encoding>`.
#[derive(Debug, Clone)]
pub(crate) enum Top {
    /// A function (`has_type`) or data name.
    Name { name: Name, has_type: bool },
    /// A `T` or `G` special name; `inner` is `None` when it holds no name.
    Special { inner: Option<Inner> },
}

/// What a parse found.
#[derive(Debug, Clone)]
pub(crate) struct Parsed {
    pub subs: Vec<Span>,
    pub refs: Vec<SubRef>,
    pub top: Top,
    /// Where the encoding ends: the text length, or the `.` of a clone
    /// suffix.
    pub end: usize,
}

/// Parses `_Z <encoding>`, optionally followed by a `.` suffix that the
/// recognizer does not read.
pub(crate) fn parse(text: &[u8], strict: bool) -> R<Parsed> {
    let mut p = Parser {
        s: text,
        pos: 0,
        strict,
        depth: 0,
        try_targs: true,
        subs: Vec::new(),
        refs: Vec::new(),
    };
    if !p.eat_str(b"_Z") {
        return Err(Fail);
    }
    let top = p.encoding()?;
    if p.pos != text.len() && p.peek() != Some(b'.') {
        return Err(Fail);
    }
    Ok(Parsed {
        subs: p.subs,
        refs: p.refs,
        top,
        end: p.pos,
    })
}

/// What the name of a function encoding says about its signature.
#[derive(Default)]
struct State {
    ctor_dtor_conv: bool,
    ends_with_targs: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum OpKind {
    Binary,
    Prefix,
    Postfix,
    Array,
    Member { named: bool },
    New,
    Del,
    Call,
    CCast,
    Conditional,
    NameOnly,
    NamedCast,
    OfId { ty: bool },
}

/// The two-letter operator codes, sorted, as LLVM's table lists them.
const OPS: &[(&[u8; 2], OpKind)] = &[
    (b"aN", OpKind::Binary),
    (b"aS", OpKind::Binary),
    (b"aa", OpKind::Binary),
    (b"ad", OpKind::Prefix),
    (b"an", OpKind::Binary),
    (b"at", OpKind::OfId { ty: true }),
    (b"aw", OpKind::NameOnly),
    (b"az", OpKind::OfId { ty: false }),
    (b"cc", OpKind::NamedCast),
    (b"cl", OpKind::Call),
    (b"cm", OpKind::Binary),
    (b"co", OpKind::Prefix),
    (b"cp", OpKind::Call),
    (b"cv", OpKind::CCast),
    (b"dV", OpKind::Binary),
    (b"da", OpKind::Del),
    (b"dc", OpKind::NamedCast),
    (b"de", OpKind::Prefix),
    (b"dl", OpKind::Del),
    (b"ds", OpKind::Member { named: false }),
    (b"dt", OpKind::Member { named: false }),
    (b"dv", OpKind::Binary),
    (b"eO", OpKind::Binary),
    (b"eo", OpKind::Binary),
    (b"eq", OpKind::Binary),
    (b"ge", OpKind::Binary),
    (b"gt", OpKind::Binary),
    (b"ix", OpKind::Array),
    (b"lS", OpKind::Binary),
    (b"le", OpKind::Binary),
    (b"ls", OpKind::Binary),
    (b"lt", OpKind::Binary),
    (b"mI", OpKind::Binary),
    (b"mL", OpKind::Binary),
    (b"mi", OpKind::Binary),
    (b"ml", OpKind::Binary),
    (b"mm", OpKind::Postfix),
    (b"na", OpKind::New),
    (b"ne", OpKind::Binary),
    (b"ng", OpKind::Prefix),
    (b"nt", OpKind::Prefix),
    (b"nw", OpKind::New),
    (b"oR", OpKind::Binary),
    (b"oo", OpKind::Binary),
    (b"or", OpKind::Binary),
    (b"pL", OpKind::Binary),
    (b"pl", OpKind::Binary),
    (b"pm", OpKind::Member { named: true }),
    (b"pp", OpKind::Postfix),
    (b"ps", OpKind::Prefix),
    (b"pt", OpKind::Member { named: true }),
    (b"qu", OpKind::Conditional),
    (b"rM", OpKind::Binary),
    (b"rS", OpKind::Binary),
    (b"rc", OpKind::NamedCast),
    (b"rm", OpKind::Binary),
    (b"rs", OpKind::Binary),
    (b"sc", OpKind::NamedCast),
    (b"ss", OpKind::Binary),
    (b"st", OpKind::OfId { ty: true }),
    (b"sz", OpKind::OfId { ty: false }),
    (b"te", OpKind::OfId { ty: false }),
    (b"ti", OpKind::OfId { ty: true }),
];

struct Parser<'a> {
    s: &'a [u8],
    pos: usize,
    strict: bool,
    depth: usize,
    try_targs: bool,
    subs: Vec<Span>,
    refs: Vec<SubRef>,
}

impl Parser<'_> {
    fn peek(&self) -> Option<u8> {
        self.s.get(self.pos).copied()
    }

    fn peek_at(&self, n: usize) -> Option<u8> {
        self.s.get(self.pos + n).copied()
    }

    fn eat(&mut self, c: u8) -> bool {
        if self.peek() == Some(c) {
            self.pos += 1;
            true
        } else {
            false
        }
    }

    fn eat_str(&mut self, t: &[u8]) -> bool {
        if self.s[self.pos..].starts_with(t) {
            self.pos += t.len();
            true
        } else {
            false
        }
    }

    fn expect(&mut self, c: u8) -> R<()> {
        if self.eat(c) { Ok(()) } else { Err(Fail) }
    }

    fn push(&mut self, start: usize) {
        self.subs.push(Span {
            start,
            end: self.pos,
        });
    }

    fn enter(&mut self) -> R<()> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return Err(Fail);
        }
        Ok(())
    }

    fn leave<T>(&mut self, r: R<T>) -> R<T> {
        self.depth -= 1;
        r
    }

    fn digit(&self) -> bool {
        self.peek().is_some_and(|c| c.is_ascii_digit())
    }

    /// `[n] <digits>`; returns whether any digit was read.
    fn number(&mut self, negative: bool) -> bool {
        let save = self.pos;
        if negative {
            self.eat(b'n');
        }
        if !self.digit() {
            self.pos = save;
            return false;
        }
        while self.digit() {
            self.pos += 1;
        }
        true
    }

    fn positive(&mut self) -> R<usize> {
        let mut n: usize = 0;
        if !self.digit() {
            return Err(Fail);
        }
        while let Some(c) = self.peek().filter(u8::is_ascii_digit) {
            n = n
                .checked_mul(10)
                .and_then(|n| n.checked_add(usize::from(c - b'0')))
                .ok_or(Fail)?;
            self.pos += 1;
        }
        Ok(n)
    }

    fn source_name(&mut self) -> R<()> {
        let len = self.positive()?;
        if len == 0 || self.s.len() - self.pos < len {
            return Err(Fail);
        }
        self.pos += len;
        Ok(())
    }

    fn cv_quals(&mut self) -> bool {
        let r = self.eat(b'r');
        let v = self.eat(b'V');
        let k = self.eat(b'K');
        r || v || k
    }

    fn at_end_of_encoding(&self) -> bool {
        matches!(self.peek(), None | Some(b'E' | b'.' | b'_'))
    }

    fn encoding(&mut self) -> R<Top> {
        self.enter()?;
        let r = self.encoding_inner();
        self.leave(r)
    }

    fn encoding_inner(&mut self) -> R<Top> {
        if matches!(self.peek(), Some(b'G' | b'T')) {
            return self.special_name();
        }
        let mut st = State::default();
        let name = self.name(Some(&mut st))?;
        if self.at_end_of_encoding() {
            return Ok(Top::Name {
                name,
                has_type: false,
            });
        }
        if self.eat_str(b"Ua9enable_ifI") {
            while !self.eat(b'E') {
                self.template_arg()?;
            }
        }
        if !st.ctor_dtor_conv && st.ends_with_targs {
            self.ty()?;
        }
        if !self.eat(b'v') {
            loop {
                self.ty()?;
                if self.at_end_of_encoding() || self.peek() == Some(b'Q') {
                    break;
                }
            }
        }
        if self.eat(b'Q') {
            self.expr()?;
        }
        Ok(Top::Name {
            name,
            has_type: true,
        })
    }

    fn call_offset(&mut self) -> R<()> {
        if self.eat(b'h') {
            if !self.number(true) {
                return Err(Fail);
            }
            return self.expect(b'_');
        }
        if self.eat(b'v') {
            if !self.number(true) {
                return Err(Fail);
            }
            self.expect(b'_')?;
            if !self.number(true) {
                return Err(Fail);
            }
            return self.expect(b'_');
        }
        Err(Fail)
    }

    fn special_name(&mut self) -> R<Top> {
        let inner = match (self.peek(), self.peek_at(1)) {
            (Some(b'T'), Some(b'A')) => {
                self.pos += 2;
                self.template_arg()?;
                None
            }
            (Some(b'T'), Some(b'V' | b'T' | b'I' | b'S')) => {
                self.pos += 2;
                self.ty()?.map(Inner::Type)
            }
            (Some(b'T'), Some(b'c')) => {
                self.pos += 2;
                self.call_offset()?;
                self.call_offset()?;
                Some(Inner::Encoding(Box::new(self.encoding()?)))
            }
            (Some(b'T'), Some(b'C')) => {
                self.pos += 2;
                let first = self.ty()?;
                if !self.number(true) {
                    return Err(Fail);
                }
                self.expect(b'_')?;
                self.ty()?;
                first.map(Inner::Type)
            }
            (Some(b'T'), Some(b'W' | b'H')) => {
                self.pos += 2;
                Some(Inner::Name(self.name(None)?))
            }
            (Some(b'T'), _) => {
                self.pos += 1;
                self.call_offset()?;
                Some(Inner::Encoding(Box::new(self.encoding()?)))
            }
            (Some(b'G'), Some(b'V')) => {
                self.pos += 2;
                Some(Inner::Name(self.name(None)?))
            }
            (Some(b'G'), Some(b'R')) => {
                self.pos += 2;
                let name = self.name(None)?;
                let seq = self.seq_id();
                if !self.eat(b'_') && seq {
                    return Err(Fail);
                }
                Some(Inner::Name(name))
            }
            _ => return Err(Fail),
        };
        Ok(Top::Special { inner })
    }

    /// `[0-9A-Z]+`; returns whether any was read.
    fn seq_id(&mut self) -> bool {
        let start = self.pos;
        while self
            .peek()
            .is_some_and(|c| c.is_ascii_digit() || c.is_ascii_uppercase())
        {
            self.pos += 1;
        }
        self.pos > start
    }

    fn name(&mut self, st: Option<&mut State>) -> R<Name> {
        self.enter()?;
        let r = self.name_inner(st);
        self.leave(r)
    }

    fn name_inner(&mut self, mut st: Option<&mut State>) -> R<Name> {
        match self.peek() {
            Some(b'N') => return self.nested(st),
            Some(b'Z') => return self.local(st),
            _ => {}
        }
        let start = self.pos;
        let std = self.eat_str(b"St");
        let mut comp;
        let subst;
        if self.peek() == Some(b'S') {
            if std {
                return Err(Fail);
            }
            comp = self.substitution()?;
            subst = true;
        } else {
            comp = self.unqualified(st.as_deref_mut(), false)?;
            subst = false;
        }
        if self.peek() == Some(b'I') {
            if !subst {
                self.push(start);
            }
            let ts = self.pos;
            self.template_args()?;
            comp.targs = Some(Span {
                start: ts,
                end: self.pos,
            });
            if let Some(st) = st {
                st.ends_with_targs = true;
            }
        } else if subst {
            return Err(Fail);
        }
        Ok(Name {
            start,
            end: self.pos,
            form: Form::Unscoped { std, comp },
        })
    }

    /// `S <seq-id> _`, `S_`, or a special abbreviation with its ABI tags.
    fn substitution(&mut self) -> R<Comp> {
        let start = self.pos;
        self.expect(b'S')?;
        match self.peek() {
            Some(c @ (b'a' | b'b' | b's' | b'i' | b'o' | b'd')) => {
                self.pos += 1;
                let tags = self.abi_tags()?;
                if !tags.is_empty() {
                    self.push(start);
                }
                Ok(Comp {
                    start,
                    end: self.pos,
                    kind: CompKind::Special(c),
                    tags,
                    targs: None,
                })
            }
            Some(b'_') => {
                self.pos += 1;
                self.reference(start, 0)
            }
            _ => {
                let ds = self.pos;
                if !self.seq_id() {
                    return Err(Fail);
                }
                let mut n: usize = 0;
                for &c in &self.s[ds..self.pos] {
                    let d = if c.is_ascii_digit() {
                        c - b'0'
                    } else {
                        c - b'A' + 10
                    };
                    n = n
                        .checked_mul(36)
                        .and_then(|n| n.checked_add(usize::from(d)))
                        .ok_or(Fail)?;
                }
                self.expect(b'_')?;
                self.reference(start, n.checked_add(1).ok_or(Fail)?)
            }
        }
    }

    fn reference(&mut self, start: usize, index: usize) -> R<Comp> {
        if self.strict && index >= self.subs.len() {
            return Err(Fail);
        }
        let span = Span {
            start,
            end: self.pos,
        };
        self.refs.push(SubRef { span, index });
        Ok(Comp {
            start,
            end: self.pos,
            kind: CompKind::Subst,
            tags: Vec::new(),
            targs: None,
        })
    }

    fn nested(&mut self, mut st: Option<&mut State>) -> R<Name> {
        let start = self.pos;
        self.expect(b'N')?;
        let quals = if self.eat(b'H') {
            true
        } else {
            let cv = self.cv_quals();
            let rq = self.eat(b'O') || self.eat(b'R');
            cv || rq
        };
        let p0 = self.pos;
        let mut comps: Vec<Comp> = Vec::new();
        let mut pushed = false;
        loop {
            if self.eat(b'E') {
                break;
            }
            if let Some(st) = st.as_deref_mut() {
                st.ends_with_targs = false;
            }
            let here = self.pos;
            match (self.peek(), self.peek_at(1)) {
                (Some(b'T'), _) => {
                    if !comps.is_empty() {
                        return Err(Fail);
                    }
                    self.template_param()?;
                    comps.push(plain(here, self.pos, CompKind::TemplateParam));
                }
                (Some(b'I'), _) => {
                    let last = comps.last().ok_or(Fail)?;
                    if last.targs.is_some() {
                        return Err(Fail);
                    }
                    self.template_args()?;
                    if let Some(last) = comps.last_mut() {
                        last.targs = Some(Span {
                            start: here,
                            end: self.pos,
                        });
                    }
                    if let Some(st) = st.as_deref_mut() {
                        st.ends_with_targs = true;
                    }
                }
                (Some(b'D'), Some(b't' | b'T')) => {
                    if !comps.is_empty() {
                        return Err(Fail);
                    }
                    self.decltype()?;
                    comps.push(plain(here, self.pos, CompKind::Decltype));
                }
                (Some(b'S'), Some(b't')) => {
                    if !comps.is_empty() {
                        return Err(Fail);
                    }
                    self.pos += 2;
                    comps.push(plain(here, self.pos, CompKind::Std));
                    continue;
                }
                (Some(b'S'), _) => {
                    if !comps.is_empty() {
                        return Err(Fail);
                    }
                    let c = self.substitution()?;
                    comps.push(c);
                    continue;
                }
                _ => {
                    let scoped = !comps.is_empty();
                    let c = self.unqualified(st.as_deref_mut(), scoped)?;
                    comps.push(c);
                }
            }
            self.push(p0);
            pushed = true;
            self.eat(b'M');
        }
        if !pushed {
            return Err(Fail);
        }
        self.subs.pop();
        Ok(Name {
            start,
            end: self.pos,
            form: Form::Nested {
                quals,
                p0,
                comps,
                e_pos: self.pos - 1,
            },
        })
    }

    fn local(&mut self, st: Option<&mut State>) -> R<Name> {
        let start = self.pos;
        self.expect(b'Z')?;
        let encoding = Box::new(self.encoding()?);
        self.expect(b'E')?;
        if self.eat(b's') {
            self.discriminator();
            return Ok(Name {
                start,
                end: self.pos,
                form: Form::Local {
                    encoding,
                    entity: None,
                },
            });
        }
        let entity = if self.eat(b'd') {
            self.number(true);
            self.expect(b'_')?;
            self.name(st)?
        } else {
            let n = self.name(st)?;
            self.discriminator();
            n
        };
        Ok(Name {
            start,
            end: self.pos,
            form: Form::Local {
                encoding,
                entity: Some(Box::new(entity)),
            },
        })
    }

    fn discriminator(&mut self) {
        let s = self.s;
        let p = self.pos;
        if s.get(p) == Some(&b'_') {
            match s.get(p + 1) {
                Some(c) if c.is_ascii_digit() => self.pos = p + 2,
                Some(b'_') => {
                    let mut q = p + 2;
                    while s.get(q).is_some_and(u8::is_ascii_digit) {
                        q += 1;
                    }
                    if s.get(q) == Some(&b'_') {
                        self.pos = q + 1;
                    }
                }
                _ => {}
            }
        } else if s.get(p).is_some_and(u8::is_ascii_digit) {
            let mut q = p + 1;
            while s.get(q).is_some_and(u8::is_ascii_digit) {
                q += 1;
            }
            if q == s.len() {
                self.pos = q;
            }
        }
    }

    fn abi_tags(&mut self) -> R<Vec<Span>> {
        let mut tags = Vec::new();
        loop {
            let start = self.pos;
            if !self.eat(b'B') {
                return Ok(tags);
            }
            self.source_name()?;
            tags.push(Span {
                start,
                end: self.pos,
            });
        }
    }

    fn unqualified(&mut self, st: Option<&mut State>, scoped: bool) -> R<Comp> {
        let start = self.pos;
        if scoped {
            self.eat(b'F');
        }
        let internal = self.eat(b'L');
        let kind = match (self.peek(), self.peek_at(1)) {
            (Some(b'1'..=b'9'), _) => {
                self.source_name()?;
                CompKind::Source { internal }
            }
            (Some(b'U'), _) => {
                self.unnamed_type()?;
                CompKind::Unnamed
            }
            (Some(b'D'), Some(b'C')) => {
                self.pos += 2;
                loop {
                    self.source_name()?;
                    if self.eat(b'E') {
                        break;
                    }
                }
                CompKind::Binding
            }
            (Some(b'C' | b'D'), _) => {
                if !scoped {
                    return Err(Fail);
                }
                self.ctor_dtor(st)?;
                CompKind::CtorDtor
            }
            _ => self.operator_name(st)?,
        };
        let tags = self.abi_tags()?;
        Ok(Comp {
            start,
            end: self.pos,
            kind,
            tags,
            targs: None,
        })
    }

    fn ctor_dtor(&mut self, st: Option<&mut State>) -> R<()> {
        if self.eat(b'C') {
            let inherited = self.eat(b'I');
            if !matches!(self.peek(), Some(b'1'..=b'5')) {
                return Err(Fail);
            }
            self.pos += 1;
            if let Some(st) = st {
                st.ctor_dtor_conv = true;
            }
            if inherited {
                self.name(None)?;
            }
            return Ok(());
        }
        if self.peek() == Some(b'D')
            && matches!(self.peek_at(1), Some(b'0' | b'1' | b'2' | b'4' | b'5'))
        {
            self.pos += 2;
            if let Some(st) = st {
                st.ctor_dtor_conv = true;
            }
            return Ok(());
        }
        Err(Fail)
    }

    fn op_code(&self) -> Option<OpKind> {
        let a = self.peek()?;
        let b = self.peek_at(1)?;
        OPS.binary_search_by(|(code, _)| code[..].cmp(&[a, b][..]))
            .ok()
            .map(|i| OPS[i].1)
    }

    fn operator_name(&mut self, st: Option<&mut State>) -> R<CompKind> {
        if let Some(kind) = self.op_code() {
            match kind {
                OpKind::CCast => {
                    self.pos += 2;
                    let save = self.try_targs;
                    self.try_targs = false;
                    let r = self.ty();
                    self.try_targs = save;
                    r?;
                    if let Some(st) = st {
                        st.ctor_dtor_conv = true;
                    }
                    return Ok(CompKind::Conversion);
                }
                OpKind::NamedCast | OpKind::OfId { .. } | OpKind::Member { named: false } => {
                    return Err(Fail);
                }
                _ => {
                    self.pos += 2;
                    return Ok(CompKind::Operator);
                }
            }
        }
        if self.eat_str(b"li") {
            self.source_name()?;
            return Ok(CompKind::Operator);
        }
        if self.peek() == Some(b'v') && self.peek_at(1).is_some_and(|c| c.is_ascii_digit()) {
            self.pos += 2;
            self.source_name()?;
            return Ok(CompKind::Operator);
        }
        Err(Fail)
    }

    fn unnamed_type(&mut self) -> R<()> {
        if self.eat_str(b"Ut") {
            self.number(false);
            return self.expect(b'_');
        }
        if self.eat_str(b"Ul") {
            while self.is_template_param_decl() {
                self.template_param_decl()?;
            }
            if self.eat(b'Q') {
                self.expr()?;
            }
            if !self.eat(b'v') {
                loop {
                    self.ty()?;
                    if matches!(self.peek(), Some(b'E' | b'Q')) {
                        break;
                    }
                }
            }
            if self.eat(b'Q') {
                self.expr()?;
            }
            self.expect(b'E')?;
            self.number(false);
            return self.expect(b'_');
        }
        if self.eat_str(b"Ub") {
            self.number(false);
            return self.expect(b'_');
        }
        Err(Fail)
    }

    fn is_template_param_decl(&self) -> bool {
        self.peek() == Some(b'T')
            && matches!(self.peek_at(1), Some(b'y' | b'p' | b't' | b'n' | b'k'))
    }

    fn template_param_decl(&mut self) -> R<()> {
        self.enter()?;
        let r = self.template_param_decl_inner();
        self.leave(r)
    }

    fn template_param_decl_inner(&mut self) -> R<()> {
        self.expect(b'T')?;
        match self.peek() {
            Some(b'y') => {
                self.pos += 1;
                Ok(())
            }
            Some(b'k') => {
                self.pos += 1;
                self.name(None)?;
                if self.peek() == Some(b'I') {
                    self.template_args()?;
                }
                Ok(())
            }
            Some(b'n') => {
                self.pos += 1;
                self.ty().map(|_| ())
            }
            Some(b't') => {
                self.pos += 1;
                while self.is_template_param_decl() {
                    self.template_param_decl()?;
                }
                if self.eat(b'Q') {
                    self.expr()?;
                }
                self.expect(b'E')
            }
            Some(b'p') => {
                self.pos += 1;
                if !self.is_template_param_decl() {
                    return Err(Fail);
                }
                self.template_param_decl()
            }
            _ => Err(Fail),
        }
    }

    fn template_param(&mut self) -> R<()> {
        self.expect(b'T')?;
        if self.eat(b'L') {
            self.positive()?;
            self.expect(b'_')?;
        }
        if !self.eat(b'_') {
            self.positive()?;
            self.expect(b'_')?;
        }
        Ok(())
    }

    fn template_args(&mut self) -> R<()> {
        self.expect(b'I')?;
        loop {
            if self.eat(b'E') {
                return Ok(());
            }
            self.template_arg()?;
            if self.eat(b'Q') {
                self.expr()?;
                return self.expect(b'E');
            }
        }
    }

    fn template_arg(&mut self) -> R<()> {
        self.enter()?;
        let r = self.template_arg_inner();
        self.leave(r)
    }

    fn template_arg_inner(&mut self) -> R<()> {
        match (self.peek(), self.peek_at(1)) {
            (Some(b'X'), _) => {
                self.pos += 1;
                self.expr()?;
                self.expect(b'E')
            }
            (Some(b'J'), _) => {
                self.pos += 1;
                while !self.eat(b'E') {
                    self.template_arg()?;
                }
                Ok(())
            }
            (Some(b'L'), Some(b'Z')) => {
                self.pos += 2;
                self.encoding()?;
                self.expect(b'E')
            }
            (Some(b'L'), _) => self.expr_primary(),
            (Some(b'T'), _) if self.is_template_param_decl() => {
                self.template_param_decl()?;
                self.template_arg()
            }
            _ => self.ty().map(|_| ()),
        }
    }

    fn expr_primary(&mut self) -> R<()> {
        self.expect(b'L')?;
        match self.peek() {
            Some(b'w') => {
                self.pos += 1;
                self.int_literal()
            }
            Some(b'b') => {
                if self.eat_str(b"b0E") || self.eat_str(b"b1E") {
                    Ok(())
                } else {
                    Err(Fail)
                }
            }
            Some(
                b'c' | b'a' | b'h' | b's' | b't' | b'i' | b'j' | b'l' | b'm' | b'x' | b'y' | b'n'
                | b'o',
            ) => {
                self.pos += 1;
                self.int_literal()
            }
            Some(b'f' | b'd' | b'e') => {
                self.pos += 1;
                let start = self.pos;
                while self
                    .peek()
                    .is_some_and(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
                {
                    self.pos += 1;
                }
                if self.pos == start {
                    return Err(Fail);
                }
                self.expect(b'E')
            }
            Some(b'_') => {
                if !self.eat_str(b"_Z") {
                    return Err(Fail);
                }
                self.encoding()?;
                self.expect(b'E')
            }
            Some(b'A') => {
                self.ty()?;
                self.expect(b'E')
            }
            Some(b'D') => {
                if !self.eat_str(b"Dn") {
                    return Err(Fail);
                }
                self.eat(b'0');
                self.expect(b'E')
            }
            Some(b'T') => Err(Fail),
            Some(b'U') => {
                if self.peek_at(1) != Some(b'l') {
                    return Err(Fail);
                }
                self.unnamed_type()?;
                self.expect(b'E')
            }
            _ => {
                self.ty()?;
                if !self.number(true) {
                    return Err(Fail);
                }
                self.expect(b'E')
            }
        }
    }

    fn int_literal(&mut self) -> R<()> {
        if !self.number(true) {
            return Err(Fail);
        }
        self.expect(b'E')
    }

    fn decltype(&mut self) -> R<()> {
        if !(self.eat_str(b"Dt") || self.eat_str(b"DT")) {
            return Err(Fail);
        }
        self.expr()?;
        self.expect(b'E')
    }

    /// Parses a type and adds it as a candidate unless it is a builtin or a
    /// bare substitution. Returns the class name when the type is one.
    fn ty(&mut self) -> R<Option<Name>> {
        self.enter()?;
        let r = self.ty_inner();
        self.leave(r)
    }

    fn ty_inner(&mut self) -> R<Option<Name>> {
        let start = self.pos;
        let mut name = None;
        match (self.peek(), self.peek_at(1)) {
            (Some(b'r' | b'V' | b'K'), _) => {
                let mut k = 0;
                for q in *b"rVK" {
                    if self.peek_at(k) == Some(q) {
                        k += 1;
                    }
                }
                let after = self.peek_at(k);
                let next = self.peek_at(k + 1);
                if after == Some(b'F')
                    || (after == Some(b'D') && matches!(next, Some(b'o' | b'O' | b'w' | b'x')))
                {
                    self.function_type()?;
                } else {
                    self.qualified_type()?;
                }
            }
            (Some(b'U'), _) => self.qualified_type()?,
            (
                Some(
                    b'v' | b'w' | b'b' | b'c' | b'a' | b'h' | b's' | b't' | b'i' | b'j' | b'l'
                    | b'm' | b'x' | b'y' | b'n' | b'o' | b'f' | b'd' | b'e' | b'g' | b'z',
                ),
                _,
            ) => {
                self.pos += 1;
                return Ok(None);
            }
            (Some(b'u'), _) => {
                self.pos += 1;
                self.source_name()?;
                if self.peek() == Some(b'I') {
                    self.template_args()?;
                }
            }
            (Some(b'D'), Some(d)) => match d {
                b'd' | b'e' | b'f' | b'h' | b'i' | b's' | b'u' | b'a' | b'c' | b'n' => {
                    self.pos += 2;
                    return Ok(None);
                }
                b'F' => {
                    self.pos += 2;
                    if !self.number(false) {
                        return Err(Fail);
                    }
                    if !(self.eat(b'_') || self.eat(b'b')) {
                        return Err(Fail);
                    }
                    return Ok(None);
                }
                b'B' | b'U' => {
                    self.pos += 2;
                    if !self.number(false) {
                        self.expr()?;
                    }
                    self.expect(b'_')?;
                    return Ok(None);
                }
                b't' | b'T' => self.decltype()?,
                b'p' => {
                    self.pos += 2;
                    self.ty()?;
                }
                b'v' => self.vector_type()?,
                b'o' | b'O' | b'w' | b'x' => self.function_type()?,
                _ => return Err(Fail),
            },
            (Some(b'F'), _) => self.function_type()?,
            (Some(b'A'), _) => {
                self.pos += 1;
                if !self.number(false) && self.peek() != Some(b'_') {
                    self.expr()?;
                }
                self.expect(b'_')?;
                self.ty()?;
            }
            (Some(b'M'), _) => {
                self.pos += 1;
                self.ty()?;
                self.ty()?;
            }
            (Some(b'T'), Some(b's' | b'u' | b'e')) => {
                self.pos += 2;
                self.name(None)?;
            }
            (Some(b'T'), _) => {
                self.template_param()?;
                if self.try_targs && self.peek() == Some(b'I') {
                    self.push(start);
                    self.template_args()?;
                }
            }
            (Some(b'P' | b'R' | b'O' | b'C' | b'G'), _) => {
                self.pos += 1;
                self.ty()?;
            }
            (Some(b'S'), Some(b't')) => name = Some(self.name(None)?),
            (Some(b'S'), _) => {
                let mut comp = self.substitution()?;
                if self.peek() == Some(b'I') && self.try_targs {
                    let ts = self.pos;
                    self.template_args()?;
                    comp.targs = Some(Span {
                        start: ts,
                        end: self.pos,
                    });
                    name = Some(Name {
                        start,
                        end: self.pos,
                        form: Form::Unscoped { std: false, comp },
                    });
                } else {
                    return Ok(Some(Name {
                        start,
                        end: self.pos,
                        form: Form::Unscoped { std: false, comp },
                    }));
                }
            }
            (Some(_), _) => name = Some(self.name(None)?),
            (None, _) => return Err(Fail),
        }
        self.push(start);
        Ok(name)
    }

    fn qualified_type(&mut self) -> R<()> {
        self.enter()?;
        let r = self.qualified_type_inner();
        self.leave(r)
    }

    fn qualified_type_inner(&mut self) -> R<()> {
        if self.eat(b'U') {
            self.source_name()?;
            if self.peek() == Some(b'I') {
                self.template_args()?;
            }
            return self.qualified_type();
        }
        self.cv_quals();
        self.ty().map(|_| ())
    }

    fn function_type(&mut self) -> R<()> {
        self.cv_quals();
        if self.eat_str(b"Do") {
        } else if self.eat_str(b"DO") {
            self.expr()?;
            self.expect(b'E')?;
        } else if self.eat_str(b"Dw") {
            loop {
                self.ty()?;
                if self.eat(b'E') {
                    break;
                }
            }
        }
        self.eat_str(b"Dx");
        self.expect(b'F')?;
        self.eat(b'Y');
        self.ty()?;
        loop {
            if self.eat(b'E') || self.eat_str(b"RE") || self.eat_str(b"OE") {
                return Ok(());
            }
            if self.eat(b'v') {
                continue;
            }
            self.ty()?;
        }
    }

    fn vector_type(&mut self) -> R<()> {
        if !self.eat_str(b"Dv") {
            return Err(Fail);
        }
        if self.peek().is_some_and(|c| (b'1'..=b'9').contains(&c)) {
            self.number(false);
            self.expect(b'_')?;
            if self.eat(b'p') {
                return Ok(());
            }
            return self.ty().map(|_| ());
        }
        if !self.eat(b'_') {
            self.expr()?;
            self.expect(b'_')?;
        }
        self.ty().map(|_| ())
    }

    fn expr(&mut self) -> R<()> {
        self.enter()?;
        let r = self.expr_inner();
        self.leave(r)
    }

    fn exprs_until_e(&mut self) -> R<()> {
        while !self.eat(b'E') {
            self.expr()?;
        }
        Ok(())
    }

    fn expr_inner(&mut self) -> R<()> {
        self.eat_str(b"gs");
        if let Some(kind) = self.op_code() {
            self.pos += 2;
            return match kind {
                OpKind::Binary | OpKind::Array | OpKind::Member { .. } => {
                    self.expr()?;
                    self.expr()
                }
                OpKind::Prefix | OpKind::Del => self.expr(),
                OpKind::Postfix => {
                    self.eat(b'_');
                    self.expr()
                }
                OpKind::New => {
                    while !self.eat(b'_') {
                        self.expr()?;
                    }
                    self.ty()?;
                    let inits = self.eat_str(b"pi");
                    while !self.eat(b'E') {
                        if !inits {
                            return Err(Fail);
                        }
                        self.expr()?;
                    }
                    Ok(())
                }
                OpKind::Call => {
                    self.expr()?;
                    self.exprs_until_e()
                }
                OpKind::CCast => {
                    let save = self.try_targs;
                    self.try_targs = false;
                    let r = self.ty();
                    self.try_targs = save;
                    r?;
                    if self.eat(b'_') {
                        self.exprs_until_e()
                    } else {
                        self.expr()
                    }
                }
                OpKind::Conditional => {
                    self.expr()?;
                    self.expr()?;
                    self.expr()
                }
                OpKind::NamedCast => {
                    self.ty()?;
                    self.expr()
                }
                OpKind::OfId { ty: true } => self.ty().map(|_| ()),
                OpKind::OfId { ty: false } => self.expr(),
                OpKind::NameOnly => Err(Fail),
            };
        }
        match (self.peek(), self.peek_at(1)) {
            (Some(b'L'), _) => return self.expr_primary(),
            (Some(b'T'), _) => return self.template_param(),
            (Some(b'f'), Some(b'p')) => return self.function_param(),
            (Some(b'f'), Some(b'L')) if self.peek_at(2).is_some_and(|c| c.is_ascii_digit()) => {
                return self.function_param();
            }
            (Some(b'f'), _) => return self.fold_expr(),
            _ => {}
        }
        if self.eat_str(b"il") {
            while !self.eat(b'E') {
                self.braced_expr()?;
            }
            return Ok(());
        }
        if self.eat_str(b"mc") {
            self.ty()?;
            self.expr()?;
            self.number(true);
            return self.expect(b'E');
        }
        if self.eat_str(b"nx") || self.eat_str(b"sp") || self.eat_str(b"tw") {
            return self.expr();
        }
        if self.eat_str(b"so") {
            self.ty()?;
            self.expr()?;
            self.number(true);
            while self.eat(b'_') {
                self.number(false);
            }
            self.eat(b'p');
            return self.expect(b'E');
        }
        if self.eat_str(b"sZ") {
            if self.peek() == Some(b'T') {
                return self.template_param();
            }
            return self.function_param();
        }
        if self.eat_str(b"sP") {
            while !self.eat(b'E') {
                self.template_arg()?;
            }
            return Ok(());
        }
        if self.eat_str(b"tl") {
            self.ty()?;
            while !self.eat(b'E') {
                self.braced_expr()?;
            }
            return Ok(());
        }
        if self.eat_str(b"tr") {
            return Ok(());
        }
        if self.peek() == Some(b'u') {
            self.pos += 1;
            let ns = self.pos;
            self.source_name()?;
            let uuid = self.s[ns..self.pos].ends_with(b"__uuidof");
            if uuid && self.eat(b't') {
                return self.ty().map(|_| ());
            }
            if uuid && self.eat(b'z') {
                return self.expr();
            }
            while !self.eat(b'E') {
                self.template_arg()?;
            }
            return Ok(());
        }
        self.unresolved_name()
    }

    fn function_param(&mut self) -> R<()> {
        if self.eat_str(b"fpT") {
            return Ok(());
        }
        if self.eat_str(b"fp") {
            self.cv_quals();
            self.number(false);
            return self.expect(b'_');
        }
        if self.eat_str(b"fL") {
            if !self.number(false) {
                return Err(Fail);
            }
            self.expect(b'p')?;
            self.cv_quals();
            self.number(false);
            return self.expect(b'_');
        }
        Err(Fail)
    }

    fn fold_expr(&mut self) -> R<()> {
        self.expect(b'f')?;
        let init = match self.peek() {
            Some(b'L' | b'R') => true,
            Some(b'l' | b'r') => false,
            _ => return Err(Fail),
        };
        self.pos += 1;
        let ok = match self.op_code() {
            Some(OpKind::Binary) => true,
            Some(OpKind::Member { .. }) => {
                matches!(
                    (self.peek(), self.peek_at(1)),
                    (Some(b'd'), Some(b's')) | (Some(b'p'), Some(b'm'))
                )
            }
            _ => false,
        };
        if !ok {
            return Err(Fail);
        }
        self.pos += 2;
        self.expr()?;
        if init {
            self.expr()?;
        }
        Ok(())
    }

    fn braced_expr(&mut self) -> R<()> {
        self.enter()?;
        let r = self.braced_expr_inner();
        self.leave(r)
    }

    fn braced_expr_inner(&mut self) -> R<()> {
        if self.eat_str(b"di") {
            self.source_name()?;
            return self.braced_expr();
        }
        if self.eat_str(b"dx") {
            self.expr()?;
            return self.braced_expr();
        }
        if self.eat_str(b"dX") {
            self.expr()?;
            self.expr()?;
            return self.braced_expr();
        }
        self.expr()
    }

    fn simple_id(&mut self) -> R<()> {
        self.source_name()?;
        if self.peek() == Some(b'I') {
            self.template_args()?;
        }
        Ok(())
    }

    fn unresolved_type(&mut self) -> R<()> {
        let start = self.pos;
        match self.peek() {
            Some(b'T') => {
                self.template_param()?;
                self.push(start);
                Ok(())
            }
            Some(b'D') => {
                self.decltype()?;
                self.push(start);
                Ok(())
            }
            Some(b'S') if self.peek_at(1) != Some(b't') => self.substitution().map(|_| ()),
            _ => Err(Fail),
        }
    }

    fn base_unresolved_name(&mut self) -> R<()> {
        if self.digit() {
            return self.simple_id();
        }
        if self.eat_str(b"dn") {
            if self.digit() {
                return self.simple_id();
            }
            return self.unresolved_type();
        }
        self.eat_str(b"on");
        self.operator_name(None)?;
        if self.peek() == Some(b'I') {
            self.template_args()?;
        }
        Ok(())
    }

    fn unresolved_name(&mut self) -> R<()> {
        if self.eat_str(b"srN") {
            self.unresolved_type()?;
            if self.peek() == Some(b'I') {
                self.template_args()?;
            }
            while !self.eat(b'E') {
                self.simple_id()?;
            }
            return self.base_unresolved_name();
        }
        if !self.eat_str(b"sr") {
            return self.base_unresolved_name();
        }
        if self.digit() {
            loop {
                self.simple_id()?;
                if self.eat(b'E') {
                    break;
                }
            }
        } else {
            self.unresolved_type()?;
            if self.peek() == Some(b'I') {
                self.template_args()?;
            }
        }
        self.base_unresolved_name()
    }
}

fn plain(start: usize, end: usize, kind: CompKind) -> Comp {
    Comp {
        start,
        end,
        kind,
        tags: Vec::new(),
        targs: None,
    }
}
