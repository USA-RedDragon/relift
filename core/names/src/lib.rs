//! Host names for guest symbols: Itanium names remangled into a
//! `guest::<module>` namespace.
//!
//! A guest symbol in module `M` becomes a host C identifier that is itself a
//! valid Itanium mangled name, so lldb, perf, `addr2line -C`, `nm -C` and
//! `llvm-cxxfilt` show it as `guest::M::...`:
//!
//! * A mangled name gets the prefix `5guest<len>M` on the first class or
//!   function name it holds: `_ZN3foo3barEv` in `sdk` becomes
//!   `_ZN5guest3sdk3foo3barEv`. An unscoped name is wrapped into a nested
//!   one (`_Z3fooi` becomes `_ZN5guest3sdk3fooEi`, `_ZL3foov` becomes
//!   `_ZN5guest3sdkL3fooEv`). Local names, thunks, guard variables,
//!   reference temporaries, thread-local routines, virtual tables, VTTs and
//!   typeinfo of class types prefix the first name they hold.
//! * The prefix adds substitution candidates, so every `S_` and `S<seq>_`
//!   reference is renumbered to name the same entity as before:
//!   `_ZN3foo3barERKS_` becomes `_ZN5guest3sdk3foo3barERKS1_`.
//! * `St` as the first component becomes `3std`, and `Sa`, `Sb`, `Ss`, `Si`,
//!   `So` and `Sd` there are written out (`Ss` becomes
//!   `3std12basic_stringIcSt11char_traitsIcESaIcEE`). Abbreviations inside
//!   parameter types and template arguments stay as they are.
//! * A clone suffix such as `.llvm.123` or `.cold` becomes one more ABI tag
//!   on the function's own name: `sfx` followed by the suffix with letters
//!   and digits kept, `.` as `_d`, `_` as `_u` and `$` as `_s`. So
//!   `_ZN3foo3barEv.cold` becomes `_ZN5guest3sdk3foo3barB9sfx_dcoldEv`.
//! * A plain C name becomes a nested name with exactly three components and
//!   no signature: `memcpy` becomes `_ZN5guest3sdk6memcpyE`.
//! * A name with nothing to prefix (`_ZTIPKc`), whose first component is a
//!   substitution, a template parameter or a decltype, or whose result would
//!   not read back to the same input, gets no name. The caller then uses
//!   [`fallback_name`], `guest::M::fn_<address>`.
//!
//! [`host_name`] returns a name only when [`guest_name`] maps it back to the
//! exact module and symbol it came from, so two different inputs never get
//! the same host name; a non-canonical spelling falls back instead.
//!
//! The recognizer follows the structure of LLVM's `ItaniumDemangle.h`
//! (Apache-2.0 WITH LLVM-exception).

mod edit;
mod parse;

#[cfg(test)]
mod tests;

use edit::{Edit, Rewrite, rewrite};
use parse::{CompKind, Form, Inner, Name, Parsed, Span, Top, parse};

/// The written-out forms of the special abbreviations, longest first within
/// each prefix so the reverse direction matches the longest one.
const EXPANSIONS: [(u8, &str); 6] = [
    (b's', "3std12basic_stringIcSt11char_traitsIcESaIcEE"),
    (b'd', "3std14basic_iostreamIcSt11char_traitsIcEE"),
    (b'i', "3std13basic_istreamIcSt11char_traitsIcEE"),
    (b'o', "3std13basic_ostreamIcSt11char_traitsIcEE"),
    (b'b', "3std12basic_string"),
    (b'a', "3std9allocator"),
];

/// The namespace every host name sits in.
const ROOT: &str = "5guest";

/// The text that starts every suffix tag.
const SFX: &str = "sfx";

/// The Itanium naming scheme as a [`relift_guest::Naming`].
#[derive(Debug, Clone, Copy, Default)]
pub struct ItaniumNaming;

impl relift_guest::Naming for ItaniumNaming {
    fn host_name(&self, module: &str, symbol: &str) -> Option<String> {
        host_name(module, symbol)
    }
}

/// What a host name stands for.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GuestName {
    /// The guest symbol, exactly as the module spells it.
    Symbol(String),
    /// An unnamed function at this module offset, from [`fallback_name`].
    Address(u64),
}

/// The host name for `symbol` in `module`, or `None` when the scheme cannot
/// name it (see the crate docs). `module` must be a C identifier.
pub fn host_name(module: &str, symbol: &str) -> Option<String> {
    let h = forward(module, symbol, true)?;
    match guest_name(&h)? {
        (m, GuestName::Symbol(s)) if m == module && s == symbol => Some(h),
        _ => None,
    }
}

/// The host name of an unnamed function at `addr` in `module`:
/// `fallback_name("main", 0x123450)` is `_ZN5guest4main11fn_00123450E`,
/// which demangles as `guest::main::fn_00123450`. `module` must be a C
/// identifier.
pub fn fallback_name(module: &str, addr: u64) -> String {
    let leaf = format!("fn_{addr:08x}");
    format!("_ZN{ROOT}{}{module}{}{leaf}E", module.len(), leaf.len())
}

/// The module and guest name a host name stands for, or `None` when `host`
/// is not a name this crate makes.
pub fn guest_name(host: &str) -> Option<(String, GuestName)> {
    if !is_c_ident(host) {
        return None;
    }
    let p = parse(host.as_bytes(), true).ok()?;
    if p.end != host.len() {
        return None;
    }
    let (name, is_type) = locate(&p.top)?;
    let Form::Nested {
        quals,
        p0,
        comps,
        e_pos,
    } = &name.form
    else {
        return None;
    };
    let module = prefix_module(host, comps)?;
    if let Some(plain) = plain_leaf(host, &p, name) {
        let g = match address_of(&plain) {
            Some(a) => GuestName::Address(a),
            None => GuestName::Symbol(plain),
        };
        return Some((module, g));
    }
    if comps.len() < 3 {
        return None;
    }
    let mut rw = Rewrite::default();
    let rest = &comps[2..];
    let c2 = &rest[0];
    let (head_end, head, used) = if is_source(host, c2, "3std") {
        match expansion_at(host, rest) {
            Some((c, end, used)) => (end, format!("S{}", char::from(c)), used),
            None => (c2.end, "St".to_string(), 1),
        }
    } else {
        (c2.start, String::new(), 0)
    };
    let remaining = rest.len() - used;
    if remaining == 0 && (used != 2 || *quals) {
        return None;
    }
    let unwrap = !quals && remaining == usize::from(used != 2);
    if unwrap {
        rw.push(Edit::replace(
            name.start,
            head_end,
            *p0,
            head.clone(),
            0,
            head.len(),
        ));
        rw.push(Edit::replace(
            *e_pos,
            *e_pos + 1,
            *e_pos + 1,
            String::new(),
            0,
            0,
        ));
    } else {
        rw.push(Edit::replace(
            *p0,
            head_end,
            *p0,
            head.clone(),
            0,
            head.len(),
        ));
    }
    let mut suffix = String::new();
    if let Some(tag) = own_comp(&p.top).and_then(|c| c.tags.last())
        && let Some(s) = decode_tag(&host[tag.start..tag.end])
    {
        suffix = s;
        rw.push(Edit::replace(
            tag.start,
            tag.end,
            tag.end,
            String::new(),
            0,
            0,
        ));
    }
    rw.finish()?;
    if is_type && unwrap {
        let start = rw.out_start(name.start)?;
        let end = rw.out_start(*e_pos)?;
        rw.pair(
            Span {
                start: name.start,
                end: name.end,
            },
            Span { start, end },
        );
    }
    let mut out = rewrite(host, &p, &rw)?;
    out.push_str(&suffix);
    Some((module, GuestName::Symbol(out)))
}

/// The host name before the round-trip guard; with `renumber` off the
/// substitution references keep their old text.
fn forward(module: &str, symbol: &str, renumber: bool) -> Option<String> {
    if !is_c_ident(module) || !symbol.is_ascii() {
        return None;
    }
    let prefix = format!("{ROOT}{}{module}", module.len());
    if !symbol.starts_with("_Z") || parse(symbol.as_bytes(), true).is_err() {
        if !is_c_ident(symbol) {
            return None;
        }
        return Some(format!("_ZN{prefix}{}{symbol}E", symbol.len()));
    }
    let p = parse(symbol.as_bytes(), true).ok()?;
    let suffix = &symbol[p.end..];
    if !suffix.is_empty() && !is_suffix(suffix) {
        return None;
    }
    let (name, is_type) = locate(&p.top)?;
    let mut rw = Rewrite::default();
    let mut tag = None;
    if !suffix.is_empty() {
        let comp = own_comp(&p.top)?;
        if !matches!(
            comp.kind,
            CompKind::Source { .. } | CompKind::Operator | CompKind::Unnamed
        ) {
            return None;
        }
        let text = format!("{SFX}{}", escape(suffix));
        let t = format!("B{}{text}", text.len());
        tag = Some((comp.end, t));
    }
    let wrap = match &name.form {
        Form::Nested { p0, comps, .. } => {
            let c0 = comps.first()?;
            match c0.kind {
                CompKind::Std => {
                    let t = format!("{prefix}3std");
                    let n = t.len();
                    rw.push(Edit::replace(c0.start, c0.end, c0.start, t, 0, n));
                }
                CompKind::Special(c) => {
                    if !c0.tags.is_empty() {
                        return None;
                    }
                    let t = format!("{prefix}{}", expansion(c)?);
                    let n = t.len();
                    rw.push(Edit::replace(c0.start, c0.end, c0.start, t, 0, n));
                }
                CompKind::Subst | CompKind::TemplateParam | CompKind::Decltype => return None,
                _ => rw.push(Edit::insert(*p0, prefix.clone(), 0, 0)),
            }
            false
        }
        Form::Unscoped { std, comp } => {
            let s = name.start;
            if *std {
                let t = format!("N{prefix}3std");
                let n = t.len();
                rw.push(Edit::replace(s, s + 2, s, t, 1, n));
            } else {
                match comp.kind {
                    CompKind::Special(c) => {
                        if !comp.tags.is_empty() {
                            return None;
                        }
                        let t = format!("N{prefix}{}", expansion(c)?);
                        let n = t.len();
                        rw.push(Edit::replace(s, s + 2, s, t, 1, n));
                    }
                    CompKind::Subst => return None,
                    _ => rw.push(Edit::insert(s, format!("N{prefix}"), 1, 0)),
                }
            }
            true
        }
        Form::Local { .. } => return None,
    };
    if let Some((at, t)) = tag {
        let n = t.len();
        rw.push(Edit::insert(at, t, n, n));
    }
    if wrap {
        rw.push(Edit::insert(name.end, "E".to_string(), 1, 0));
    }
    rw.finish()?;
    if is_type && wrap {
        let start = rw.out_start(name.start)?;
        let end = rw.out_end(name.end)?;
        rw.pair(
            Span {
                start: name.start,
                end: name.end,
            },
            Span { start, end },
        );
    }
    let out = if renumber {
        rewrite(&symbol[..p.end], &p, &rw)?
    } else {
        rw.apply(&symbol[..p.end])
    };
    if !is_c_ident(&out) {
        return None;
    }
    Some(out)
}

/// The name that takes the prefix, and whether it is a whole type that is
/// itself a substitution candidate.
fn locate(top: &Top) -> Option<(&Name, bool)> {
    match top {
        Top::Name { name, .. } => locate_name(name, false),
        Top::Special { inner } => match inner.as_ref()? {
            Inner::Encoding(t) => locate(t),
            Inner::Name(n) => locate_name(n, false),
            Inner::Type(n) => locate_name(n, true),
        },
    }
}

fn locate_name(n: &Name, is_type: bool) -> Option<(&Name, bool)> {
    match &n.form {
        Form::Local { encoding, .. } => locate(encoding),
        _ => Some((n, is_type)),
    }
}

/// The unqualified name of the function or object itself, which carries the
/// suffix tag.
fn own_comp(top: &Top) -> Option<&parse::Comp> {
    match top {
        Top::Name { name, .. } => own_comp_name(name),
        Top::Special { .. } => None,
    }
}

fn own_comp_name(n: &Name) -> Option<&parse::Comp> {
    match &n.form {
        Form::Nested { comps, .. } => comps.last(),
        Form::Unscoped { comp, .. } => Some(comp),
        Form::Local { entity, .. } => own_comp_name(entity.as_ref()?),
    }
}

fn expansion(c: u8) -> Option<&'static str> {
    EXPANSIONS.iter().find(|(k, _)| *k == c).map(|(_, t)| *t)
}

/// The longest expansion that the components starting at `rest[0]` spell
/// exactly: its letter, where it ends, and how many components it covers.
fn expansion_at(host: &str, rest: &[parse::Comp]) -> Option<(u8, usize, usize)> {
    let c1 = rest.get(1)?;
    if !c1.tags.is_empty() {
        return None;
    }
    let start = rest[0].start;
    for (c, text) in EXPANSIONS {
        let end = start + text.len();
        if host.get(start..end) != Some(text) {
            continue;
        }
        let boundary = match c1.targs {
            Some(t) => t.end == end || (c1.end == end && matches!(c, b'a' | b'b')),
            None => c1.end == end,
        };
        if boundary {
            return Some((c, end, 2));
        }
    }
    None
}

fn is_source(host: &str, c: &parse::Comp, text: &str) -> bool {
    c.kind == (CompKind::Source { internal: false })
        && c.tags.is_empty()
        && c.targs.is_none()
        && &host[c.start..c.end] == text
}

/// The module of a host name whose first two components are `5guest` and a
/// plain source name.
fn prefix_module(host: &str, comps: &[parse::Comp]) -> Option<String> {
    let c0 = comps.first()?;
    let c1 = comps.get(1)?;
    if !is_source(host, c0, ROOT) || c1.kind != (CompKind::Source { internal: false }) {
        return None;
    }
    if !c1.tags.is_empty() || c1.targs.is_some() {
        return None;
    }
    let text = &host[c1.start..c1.end];
    let digits = text.bytes().take_while(u8::is_ascii_digit).count();
    let module = &text[digits..];
    is_c_ident(module).then(|| module.to_string())
}

/// The plain C name a host name of exactly three components and no type
/// stands for.
fn plain_leaf(host: &str, p: &Parsed, name: &Name) -> Option<String> {
    let Top::Name {
        name: top,
        has_type: false,
    } = &p.top
    else {
        return None;
    };
    if top.start != name.start {
        return None;
    }
    let Form::Nested {
        quals: false,
        comps,
        ..
    } = &name.form
    else {
        return None;
    };
    let [_, _, c2] = comps.as_slice() else {
        return None;
    };
    if c2.kind != (CompKind::Source { internal: false }) || !c2.tags.is_empty() {
        return None;
    }
    if c2.targs.is_some() {
        return None;
    }
    let text = &host[c2.start..c2.end];
    let digits = text.bytes().take_while(u8::is_ascii_digit).count();
    Some(text[digits..].to_string())
}

/// The address a fallback leaf `fn_<hex>` names, in the exact spelling
/// [`fallback_name`] writes.
fn address_of(leaf: &str) -> Option<u64> {
    let hex = leaf.strip_prefix("fn_")?;
    if hex.len() < 8 || hex.len() > 16 {
        return None;
    }
    if !hex
        .bytes()
        .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    {
        return None;
    }
    let a = u64::from_str_radix(hex, 16).ok()?;
    (format!("{a:08x}") == hex).then_some(a)
}

fn is_c_ident(s: &str) -> bool {
    let mut b = s.bytes();
    b.next()
        .is_some_and(|c| c.is_ascii_alphabetic() || c == b'_')
        && b.all(|c| c.is_ascii_alphanumeric() || c == b'_')
}

fn is_suffix(s: &str) -> bool {
    s.starts_with('.')
        && s.bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'$'))
}

fn escape(suffix: &str) -> String {
    let mut out = String::with_capacity(suffix.len() * 2);
    for c in suffix.chars() {
        match c {
            '.' => out.push_str("_d"),
            '_' => out.push_str("_u"),
            '$' => out.push_str("_s"),
            c => out.push(c),
        }
    }
    out
}

/// The suffix a tag `B<len>sfx...` stands for.
fn decode_tag(tag: &str) -> Option<String> {
    let body = tag.strip_prefix('B')?;
    let digits = body.bytes().take_while(u8::is_ascii_digit).count();
    let text = body[digits..].strip_prefix(SFX)?;
    let mut out = String::with_capacity(text.len());
    let mut it = text.chars();
    while let Some(c) = it.next() {
        if c == '_' {
            out.push(match it.next()? {
                'd' => '.',
                'u' => '_',
                's' => '$',
                _ => return None,
            });
        } else {
            out.push(c);
        }
    }
    is_suffix(&out).then_some(out)
}
