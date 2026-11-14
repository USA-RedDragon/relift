//! IR to C11: shards, `#line` mapping and listings, the memory strategy per host target.
//!
//! [`emit`] turns the functions of one module into C source following
//! `docs/abi.md`: one `RL_TEXT RL_LIFTED` function per guest function, named
//! by the caller (section 11), guest
//! registers as locals loaded at entry, every register the function writes
//! (flags included, so the output is the flags-stored build) stored at each
//! call, trap and exit and the used ones reloaded after a call, one label per
//! instruction, direct calls by name followed by the pending check, indirect
//! calls and dynamic exits through `rl_indirect`, tail calls with
//! `RL_MUSTTAIL`, host overrides called by name and registered over the
//! guest code ([`Override`]), resume points after calls to overrides that
//! may come back through the landing-pad dispatch (section 7.1), one
//! function map per shard (`rl_fn_map`), and the module's unit table
//! (`rl_unit`) for registration. Instructions
//! that did not lift, or use an operation this emitter cannot express,
//! report themselves through the frontend's undefined-instruction entry.

mod func;
mod value;

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_guest::{Frontend, LiftFailure};
use relift_ir::Block;

/// The C helpers every shard includes after the frontend's ABI header.
pub const PRELUDE: &str = include_str!("prelude.h");

/// The name of the prelude header in the output.
pub const PRELUDE_NAME: &str = "relift_prelude.h";

/// One instruction as the emitter takes it.
#[derive(Clone, Copy, Debug)]
pub enum Insn<'a> {
    /// A lifted block and the raw instruction it came from.
    Lifted {
        /// The block.
        block: &'a Block,
        /// The raw instruction unit, for reports.
        raw: u64,
    },
    /// An instruction the frontend could not lift.
    Failed(&'a LiftFailure),
}

/// One guest function to emit. Functions are given in entry order; a map
/// entry's guest range ends at the next function's entry, so the ranges of
/// a shard's map do not overlap.
#[derive(Clone, Debug)]
pub struct Function<'a> {
    /// The entry address.
    pub entry: u64,
    /// The host identifier of the C function.
    pub name: String,
    /// The readable guest name for crash reports, if there is one.
    pub guest_name: Option<String>,
    /// The guest bytes it spans from the entry, for crash mapping.
    pub span: u64,
    /// The instructions by address.
    pub insts: BTreeMap<u64, Insn<'a>>,
    /// Known targets of computed jumps, by the jump's address; the emitter
    /// branches to those inside the function directly.
    pub tables: BTreeMap<u64, Vec<u64>>,
}

/// A host function that replaces the guest function at `entry`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Override {
    /// The guest address it replaces.
    pub entry: u64,
    /// The host function, with the lifted signature, declared by the
    /// frontend's headers.
    pub host_fn: String,
    /// Whether it may return `RL_PENDING` (`RL_FN_MAY_UNWIND`).
    pub may_unwind: bool,
    /// Whether control may come back to the return address of a call to it
    /// through the landing-pad dispatch (a `setjmp`).
    pub resume_point: bool,
}

/// Emitter options.
#[derive(Clone, Debug)]
pub struct Options {
    /// The most functions per shard file.
    pub shard_size: usize,
    /// The unit's name, for reports.
    pub unit_name: String,
    /// The C symbol of the unit table (`const rl_unit`), which the program
    /// lists for registration.
    pub unit_symbol: String,
    /// The guest load address of the module, for the function maps.
    pub module_base: u64,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            shard_size: 256,
            unit_name: "module".into(),
            unit_symbol: "rl_unit_module".into(),
            module_base: 0,
        }
    }
}

/// One emitted file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct File {
    /// The file name, relative to the output directory.
    pub name: String,
    /// The C source.
    pub text: String,
}

/// What [`emit`] produced.
#[derive(Clone, Debug, Default)]
pub struct Output {
    /// The prelude, the shards and the unit table.
    pub files: Vec<File>,
    /// How many instructions report themselves as undefined at run time.
    pub undefined: usize,
}

/// What a direct transfer to an entry calls.
#[derive(Clone, Debug)]
pub(crate) struct Target {
    /// The C function.
    pub(crate) name: String,
    /// Whether it is a lifted function of this module (declared by the
    /// shard) rather than an override (declared by the headers).
    pub(crate) lifted: bool,
    /// Whether calls to it are resume points.
    pub(crate) resume_point: bool,
}

/// Emits the functions `fns` and the overrides `overrides` with frontend `fe`.
pub fn emit(
    fe: &dyn Frontend,
    fns: &[Function<'_>],
    overrides: &[Override],
    opts: &Options,
) -> Output {
    emit_with(fe, fns, overrides, opts, &mut |_, _| {})
}

fn shard_count(functions: usize, opts: &Options) -> usize {
    functions.div_ceil(opts.shard_size.max(1))
}

/// As [`emit`], calling `on(done, total)` after every shard.
pub fn emit_with(
    fe: &dyn Frontend,
    fns: &[Function<'_>],
    overrides: &[Override],
    opts: &Options,
    on: &mut dyn FnMut(usize, usize),
) -> Output {
    let mut targets: BTreeMap<u64, Target> = fns
        .iter()
        .map(|f| {
            (
                f.entry,
                Target {
                    name: f.name.clone(),
                    lifted: true,
                    resume_point: false,
                },
            )
        })
        .collect();
    for o in overrides {
        targets.insert(
            o.entry,
            Target {
                name: o.host_fn.clone(),
                lifted: false,
                resume_point: o.resume_point,
            },
        );
    }
    let fns: Vec<&Function<'_>> = fns
        .iter()
        .filter(|f| targets.get(&f.entry).is_some_and(|t| t.lifted))
        .collect();
    let mut out = Output::default();
    out.files.push(File {
        name: PRELUDE_NAME.into(),
        text: PRELUDE.into(),
    });
    let header = format!("{}#include \"{PRELUDE_NAME}\"\n", includes(fe));
    let mut maps = Vec::new();
    let mut flags: BTreeMap<u64, &'static str> = BTreeMap::new();
    let shards = shard_count(fns.len(), opts);
    for (i, chunk) in fns.chunks(opts.shard_size.max(1)).enumerate() {
        let mut body = String::new();
        let mut callees = BTreeSet::new();
        let mut infos = String::new();
        for (k, f) in chunk.iter().enumerate() {
            let size = chunk
                .get(k + 1)
                .map_or(f.span, |n| f.span.min(n.entry - f.entry));
            let e = func::emit_function(fe, f, &targets);
            out.undefined += e.undefined;
            callees.extend(e.callees);
            body.push_str(&e.text);
            flags.insert(f.entry, e.flags);
            let _ = writeln!(
                infos,
                "    {{ UINT64_C({:#x}), {}u, {}, {}, {:?}, {} }},",
                f.entry,
                u32::try_from(size).unwrap_or(u32::MAX),
                e.flags,
                f.name,
                f.name,
                f.guest_name
                    .as_ref()
                    .map_or_else(|| "0".to_string(), |g| format!("{g:?}"))
            );
        }
        let own: BTreeSet<u64> = chunk.iter().map(|f| f.entry).collect();
        let mut text = header.clone();
        for c in callees.difference(&own) {
            if let Some(t) = targets.get(c).filter(|t| t.lifted) {
                let _ = writeln!(text, "RL_LIFTED({});", t.name);
            }
        }
        for f in chunk {
            let _ = writeln!(text, "RL_LIFTED({});", f.name);
        }
        text.push_str(&body);
        let shard = format!("shard{i:04}");
        let map = format!("{}_map{i}", opts.unit_symbol);
        let _ = write!(
            text,
            "\nRL_TEXT_BOUNDS_DECLARE();\nstatic const rl_fn_info fns[] = {{\n{infos}}};\nconst rl_fn_map {map};\nconst rl_fn_map {map} = {{ RL_FN_MAP_VERSION, {}u, fns, {:?}, {shard:?}, UINT64_C({:#x}), RL_TEXT_LO, RL_TEXT_HI }};\n",
            chunk.len(),
            opts.unit_name,
            opts.module_base
        );
        maps.push(map);
        out.files.push(File {
            name: format!("{shard}.c"),
            text,
        });
        on(i + 1, shards);
    }
    out.files.push(File {
        name: "unit.c".into(),
        text: unit_table(fe, &fns, overrides, &flags, &maps, opts),
    });
    out
}

fn includes(fe: &dyn Frontend) -> String {
    fe.abi_headers()
        .iter()
        .map(|h| format!("#include <{h}>\n"))
        .collect()
}

fn unit_table(
    fe: &dyn Frontend,
    fns: &[&Function<'_>],
    overrides: &[Override],
    flags: &BTreeMap<u64, &'static str>,
    maps: &[String],
    opts: &Options,
) -> String {
    let mut t = includes(fe);
    for f in fns {
        let _ = writeln!(t, "RL_LIFTED({});", f.name);
    }
    for m in maps {
        let _ = writeln!(t, "extern const rl_fn_map {m};");
    }
    let mut entries: Vec<(u64, String)> = fns
        .iter()
        .map(|f| {
            (
                f.entry,
                format!(
                    "    {{ UINT64_C({:#x}), {}u, {}, {} }},",
                    f.entry,
                    u32::try_from(f.span).unwrap_or(u32::MAX),
                    flags.get(&f.entry).copied().unwrap_or("RL_FN_MAY_UNWIND"),
                    f.name
                ),
            )
        })
        .collect();
    for o in overrides {
        let fl = if o.may_unwind {
            "RL_FN_OVERRIDE | RL_FN_MAY_UNWIND"
        } else {
            "RL_FN_OVERRIDE"
        };
        entries.push((
            o.entry,
            format!(
                "    {{ UINT64_C({:#x}), 0u, {fl}, {} }},",
                o.entry, o.host_fn
            ),
        ));
    }
    entries.sort();
    t.push_str("static const rl_fn_entry fns[] = {\n");
    for (_, e) in &entries {
        t.push_str(e);
        t.push('\n');
    }
    if entries.is_empty() {
        t.push_str("    { 0, 0, 0, 0 },\n");
    }
    t.push_str("};\nstatic const rl_fn_map *const maps[] = {");
    for m in maps {
        let _ = write!(t, " &{m},");
    }
    if maps.is_empty() {
        t.push_str(" 0");
    }
    let _ = write!(
        t,
        " }};\nconst rl_unit {sym};\nconst rl_unit {sym} = {{\n    RL_ABI_VERSION,\n    sizeof(rl_ctx),\n    {{ 0 }},\n    {}u,\n    0,\n    fns,\n    0,\n    0,\n    {:?},\n    {}u,\n    maps,\n}};\n",
        entries.len(),
        opts.unit_name,
        maps.len(),
        sym = opts.unit_symbol
    );
    t
}

#[cfg(test)]
mod tests;
