//! IR to C11: shards, `#line` mapping and listings, the memory strategy per host target.
//!
//! [`emit`] turns the functions of one module into C source following
//! `docs/abi.md`: one `RL_LIFTED` function per guest function, guest
//! registers as locals loaded at entry, every register the function writes
//! (flags included, so the output is the flags-stored build) stored at each
//! call, trap and exit and the used ones reloaded after a call, one label per
//! instruction, direct calls by name followed by the pending check, indirect
//! calls and dynamic exits through `rl_indirect`, tail calls with
//! `RL_MUSTTAIL`, and the module's unit table for registration. Instructions
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

/// One guest function to emit.
#[derive(Clone, Debug)]
pub struct Function<'a> {
    /// The entry address.
    pub entry: u64,
    /// The guest bytes it spans from the entry, for crash mapping.
    pub span: u64,
    /// The instructions by address.
    pub insts: BTreeMap<u64, Insn<'a>>,
}

/// Emitter options.
#[derive(Clone, Debug)]
pub struct Options {
    /// The most functions per shard file.
    pub shard_size: usize,
    /// The unit's name, for reports.
    pub unit_name: String,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            shard_size: 256,
            unit_name: "module".into(),
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

/// The C name of the lifted function at `addr`.
pub fn fn_name(addr: u64) -> String {
    format!("F_{addr:x}")
}

/// Emits the functions `fns` with frontend `fe`.
pub fn emit(fe: &dyn Frontend, fns: &[Function<'_>], opts: &Options) -> Output {
    let entries: BTreeSet<u64> = fns.iter().map(|f| f.entry).collect();
    let mut out = Output::default();
    out.files.push(File {
        name: PRELUDE_NAME.into(),
        text: PRELUDE.into(),
    });
    let header = format!(
        "#include <{}>\n#include \"{PRELUDE_NAME}\"\n",
        fe.abi_header()
    );
    for (i, chunk) in fns.chunks(opts.shard_size.max(1)).enumerate() {
        let mut body = String::new();
        let mut callees = BTreeSet::new();
        for f in chunk {
            let e = func::emit_function(fe, f, &entries);
            out.undefined += e.undefined;
            callees.extend(e.callees);
            body.push_str(&e.text);
        }
        let own: BTreeSet<u64> = chunk.iter().map(|f| f.entry).collect();
        let mut text = header.clone();
        for c in callees.difference(&own) {
            let _ = writeln!(text, "RL_LIFTED({});", fn_name(*c));
        }
        for f in chunk {
            let _ = writeln!(text, "RL_LIFTED({});", fn_name(f.entry));
        }
        text.push_str(&body);
        out.files.push(File {
            name: format!("shard{i:04}.c"),
            text,
        });
    }
    out.files.push(File {
        name: "unit.c".into(),
        text: unit_table(fe, fns, opts),
    });
    out
}

fn unit_table(fe: &dyn Frontend, fns: &[Function<'_>], opts: &Options) -> String {
    let mut t = format!("#include <{}>\n", fe.abi_header());
    for f in fns {
        let _ = writeln!(t, "RL_LIFTED({});", fn_name(f.entry));
    }
    t.push_str("static const rl_fn_entry fns[] = {\n");
    for f in fns {
        let _ = writeln!(
            t,
            "    {{ UINT64_C({:#x}), {}u, RL_FN_MAY_UNWIND, {} }},",
            f.entry,
            u32::try_from(f.span).unwrap_or(u32::MAX),
            fn_name(f.entry)
        );
    }
    if fns.is_empty() {
        t.push_str("    { 0, 0, 0, 0 },\n");
    }
    t.push_str("};\n");
    let _ = write!(
        t,
        "static const rl_unit unit = {{\n    RL_ABI_VERSION,\n    sizeof(rl_ctx),\n    {{ 0 }},\n    {}u,\n    0,\n    fns,\n    0,\n    0,\n    {:?},\n}};\nconst rl_unit *const rl_static_units[] = {{ &unit }};\nconst uint32_t rl_static_unit_count = 1;\n",
        fns.len(),
        opts.unit_name
    );
    t
}

#[cfg(test)]
mod tests;
