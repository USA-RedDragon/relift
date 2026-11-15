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
mod reuse;
mod time;
mod value;

use reuse::Targets;
pub use reuse::{Key, Store};

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
    /// The landing pads in the function: resume points the unwinder enters
    /// by a computed jump (`RL_FN_LANDING_PADS`).
    pub pads: BTreeSet<u64>,
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
    /// Whether the emitter declares it (`RL_LIFTED`) where it is used: a
    /// function of the build rather than one the frontend's headers
    /// declare, such as a lifted function of another emit or a bundled
    /// mod's switch.
    pub declare: bool,
}

/// The default [`Options::shard_bytes`]: clang at `-O2` needs about 100 MB
/// of memory plus 120 bytes per byte of C, so a 4 MiB shard compiles in
/// well under 1 GB.
pub const SHARD_BYTES: usize = 4 << 20;

/// The default [`Options::piece_bytes`].
pub const PIECE_BYTES: usize = 1 << 20;

/// Emitter options.
#[derive(Clone, Debug)]
pub struct Options {
    /// The most bytes of C per shard file. A shard closes before the
    /// function that would take it past this; a function larger on its own
    /// gets shards of its own.
    pub shard_bytes: usize,
    /// The most bytes of C per C function: a guest function whose C is
    /// longer is split into pieces of about this size, packed into shards
    /// of their own.
    pub piece_bytes: usize,
    /// The unit's name, for reports.
    pub unit_name: String,
    /// The C symbol of the unit table (`const rl_unit`), which the program
    /// lists for registration.
    pub unit_symbol: String,
    /// The guest load address of the module, for the function maps.
    pub module_base: u64,
    /// Whether the build may compile a shard in two parts ([`parts_header`]).
    pub parts: bool,
    /// The guest's I/O map, for I/O accesses of kind `Any`.
    pub io_map: relift_ir::IoMap,
    /// The identity of the guest image set, for `rl_unit.image_id`: the
    /// runtime loads a hot-loaded unit only when its id matches the static
    /// units' (docs/abi.md section 6).
    pub image_id: [u8; 32],
    /// A test hook, set only through `RELIFT_EMIT_TEST_FAULT`: the guest
    /// entry of a function whose first integer add is emitted as a subtract.
    /// It is the miscompile `relift diff` must find.
    pub test_fault: Option<u64>,
    /// When not zero, the guest bytes of a shard window: a shard holds
    /// only functions whose entries lie in one window of this many bytes
    /// from [`Options::module_base`], and is named by the window
    /// (`shard0012`, then `shard0012_1` when the window needs more than
    /// one). A change to one function then moves no function to another
    /// shard outside its window. When zero, functions fill each shard up
    /// to [`Options::shard_bytes`] and shards are numbered in order.
    pub window: u64,
    /// Whether dispatch may enter a function at any of its instructions:
    /// the unit table lists each instruction with the piece that switches
    /// to it on `c->pc` (`RL_FN_FRAGMENT`). For guests whose runtime opens
    /// every instruction at a boundary of its own, so that an interpreted
    /// run can go back to lifted code at the next instruction it reaches.
    /// The value masks instruction addresses: an address with bits outside
    /// it names a continuation block of the instruction at the masked
    /// address, kept in that instruction's piece.
    pub enter_anywhere: Option<u64>,
    /// Whether the C reads and writes the guest registers in their homes
    /// in the context rather than in locals stored and reloaded around
    /// every runtime call. For code built without optimization, where a
    /// local is a stack slot anyway: it halves the machine code of an
    /// instruction that ends in a runtime call.
    pub regs_in_context: bool,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            shard_bytes: SHARD_BYTES,
            piece_bytes: PIECE_BYTES,
            unit_name: "module".into(),
            unit_symbol: "rl_unit_module".into(),
            module_base: 0,
            parts: false,
            io_map: relift_ir::IoMap::memory(),
            image_id: [0; 32],
            test_fault: None,
            window: 0,
            enter_anywhere: None,
            regs_in_context: false,
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
    /// The functions a shard defines, by entry and host name; empty for
    /// other files.
    pub fns: Vec<(u64, String)>,
}

/// The name of the header that says which functions of each shard go into
/// part 1, in a module emitted with [`Options::parts`].
pub const PARTS_NAME: &str = "relift_parts.h";

/// The text of [`PARTS_NAME`] for a module emitted with [`Options::parts`].
///
/// Such a module's shards can each be compiled twice from one file: as
/// part 0 (the default) and as part 1 (with `-DRL_PART=1`). A function
/// named here is defined only in part 1, every other only in part 0, and
/// each part has its own function map, so the build can give a few
/// functions their own compile flags without emitting the module again.
/// `part1` lists, per shard file, the host names of its part-1 functions;
/// only those shards need a part-1 compile.
pub fn parts_header(part1: &[(&str, Vec<&str>)]) -> String {
    let mut t = String::from("/* Generated by the build: the part-1 functions of each shard. */\n");
    for (file, names) in part1 {
        if names.is_empty() {
            continue;
        }
        let _ = writeln!(t, "#define {} 1", shard_part_macro(file));
        for n in names {
            let _ = writeln!(t, "#define RL_PART_{n} 1");
        }
    }
    t
}

/// The macro [`parts_header`] defines for a shard file with part-1 functions.
fn shard_part_macro(file: &str) -> String {
    format!("RL_PART1_{}", file.trim_end_matches(".c"))
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
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Target {
    /// The C function.
    pub(crate) name: String,
    /// Whether it is a lifted function of this module (declared by the
    /// shard) rather than an override (declared by the headers).
    pub(crate) lifted: bool,
    /// Whether calls to it are resume points.
    pub(crate) resume_point: bool,
    /// Whether a shard that calls it declares it.
    pub(crate) declare: bool,
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

/// As [`emit`], calling `on(done, total)` with the functions emitted so far
/// and the total after every shard.
pub fn emit_with(
    fe: &dyn Frontend,
    fns: &[Function<'_>],
    overrides: &[Override],
    opts: &Options,
    on: &mut dyn FnMut(usize, usize),
) -> Output {
    let heads: Vec<Head> = fns.iter().map(Head::of).collect();
    let mut out = Output::default();
    out.undefined = emit_core(
        fe,
        &heads,
        overrides,
        opts,
        on,
        &mut |ks, targets| {
            ks.iter()
                .map(|&k| func::emit_function(fe, &fns[k], &Targets::new(targets), opts))
                .collect()
        },
        &mut |f| out.files.push(f),
    );
    out
}

/// One function as [`emit_streamed`] takes it before its code: what the
/// unit table, the function maps and calls to it need.
#[derive(Clone, Debug)]
pub struct Head {
    /// The entry address.
    pub entry: u64,
    /// The host identifier of the C function.
    pub name: String,
    /// The readable guest name for crash reports, if there is one.
    pub guest_name: Option<String>,
    /// The guest bytes it spans from the entry, for crash mapping.
    pub span: u64,
    /// The landing pads in the function.
    pub pads: BTreeSet<u64>,
}

impl Head {
    /// The head of `f`.
    pub fn of(f: &Function<'_>) -> Head {
        Head {
            entry: f.entry,
            name: f.name.clone(),
            guest_name: f.guest_name.clone(),
            span: f.span,
            pads: f.pads.clone(),
        }
    }
}

/// Lends functions to [`emit_streamed`]: called with indices into its
/// heads, in order, it calls the sink with each index and that function,
/// whose head must be the one at the index.
pub type Lend<'l> = dyn Fn(&[usize], &mut dyn FnMut(usize, &Function<'_>)) + Sync + 'l;

/// How many functions [`emit_streamed`] emits between two shard packings.
const BATCH: usize = 1024;

/// How many neighboring functions one thread takes at a time.
const GROUP: usize = 16;

/// As [`emit_with`], for the functions `heads` whose code `lend` gives a
/// few at a time, so that only those are held; `fe` turns them into C on
/// up to one thread per CPU. Each file goes to `sink` when it is complete,
/// in the order [`emit`] gives them. Gives the number of instructions that
/// report themselves as undefined at run time ([`Output::undefined`]).
pub fn emit_streamed(
    fe: &(dyn Frontend + Sync),
    heads: &[Head],
    lend: &Lend<'_>,
    overrides: &[Override],
    opts: &Options,
    on: &mut dyn FnMut(usize, usize),
    sink: &mut dyn FnMut(File),
) -> usize {
    let from = Stream {
        heads,
        lend,
        store: None,
    };
    emit_reusing(fe, &from, overrides, opts, on, sink)
}

/// The functions [`emit_reusing`] emits: their heads, what lends their
/// code ([`emit_streamed`]), and the store of earlier emits, if any.
pub struct Stream<'a> {
    /// The functions before their code.
    pub heads: &'a [Head],
    /// Lends their code.
    pub lend: &'a Lend<'a>,
    /// Where the C of earlier emits is kept.
    pub store: Option<&'a dyn Store>,
}

/// As [`emit_streamed`], taking the C of each function whose input and
/// direct-call targets are unchanged from `store` and keeping the C of
/// every function there. The files are the same as without a store.
pub fn emit_reusing(
    fe: &(dyn Frontend + Sync),
    from: &Stream<'_>,
    overrides: &[Override],
    opts: &Options,
    on: &mut dyn FnMut(usize, usize),
    sink: &mut dyn FnMut(File),
) -> usize {
    let Stream { heads, lend, store } = *from;
    let salt = reuse::salt(fe, opts);
    emit_core(
        fe,
        heads,
        overrides,
        opts,
        on,
        &mut |ks, targets| {
            let groups: Vec<&[usize]> = ks.chunks(GROUP).collect();
            parallel(&groups, |g| {
                let mut out = Vec::with_capacity(g.len());
                lend(g, &mut |_, f| {
                    let fault = opts.test_fault == Some(f.entry);
                    let emit = |seen: &Targets<'_>| func::emit_function(fe, f, seen, opts);
                    let Some(store) = store else {
                        out.push((emit(&Targets::new(targets)), None));
                        return;
                    };
                    let key = store.key(&reuse::input(fe, &salt, f, fault));
                    if let Some(bytes) = store.get(&key)
                        && let Some(e) = reuse::reuse(&bytes, targets)
                    {
                        out.push((e, Some((f.entry, key, bytes, true))));
                        return;
                    }
                    let seen = Targets::new(targets);
                    let e = emit(&seen);
                    let kept = reuse::output(&e, &seen).map(|b| (f.entry, key, b, false));
                    out.push((e, kept));
                });
                out
            })
            .into_iter()
            .flatten()
            .map(|(e, kept): (func::Emitted1, Kept)| {
                if let (Some(s), Some((entry, key, bytes, reused))) = (store, kept) {
                    s.keep(entry, &key, bytes, reused);
                }
                e
            })
            .collect()
        },
        sink,
    )
}

/// `f` of every item of `items`, in order, on up to one thread per CPU.
fn parallel<T: Sync, R: Send>(items: &[T], f: impl Fn(&T) -> R + Sync) -> Vec<R> {
    let threads = std::thread::available_parallelism()
        .map_or(1, |n| n.get())
        .min(items.len());
    if threads <= 1 {
        return items.iter().map(f).collect();
    }
    let next = std::sync::atomic::AtomicUsize::new(0);
    let mut out: Vec<(usize, R)> = std::thread::scope(|s| {
        let workers: Vec<_> = (0..threads)
            .map(|_| {
                s.spawn(|| {
                    let mut mine = Vec::new();
                    loop {
                        let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        let Some(item) = items.get(i) else {
                            break;
                        };
                        mine.push((i, f(item)));
                    }
                    mine
                })
            })
            .collect();
        workers
            .into_iter()
            .flat_map(|w| w.join().unwrap_or_else(|e| std::panic::resume_unwind(e)))
            .collect()
    });
    out.sort_unstable_by_key(|r| r.0);
    out.into_iter().map(|r| r.1).collect()
}

/// One function's C as [`Store::keep`] takes it: its entry, key, bytes
/// and whether they came from the store.
type Kept = Option<(u64, Key, Vec<u8>, bool)>;

/// Gives the C of a run of functions, by index, with the direct-call
/// targets of the module.
type Compute<'c> = dyn FnMut(&[usize], &BTreeMap<u64, Target>) -> Vec<func::Emitted1> + 'c;

/// Emits the functions `heads`, whose C `compute` gives for a run of
/// indices into them, and gives each file to `sink`.
fn emit_core(
    fe: &dyn Frontend,
    heads: &[Head],
    overrides: &[Override],
    opts: &Options,
    on: &mut dyn FnMut(usize, usize),
    compute: &mut Compute<'_>,
    sink: &mut dyn FnMut(File),
) -> usize {
    let mut targets: BTreeMap<u64, Target> = heads
        .iter()
        .map(|f| {
            (
                f.entry,
                Target {
                    name: f.name.clone(),
                    lifted: true,
                    resume_point: false,
                    declare: true,
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
                declare: o.declare,
            },
        );
    }
    let kept: Vec<usize> = (0..heads.len())
        .filter(|&k| targets.get(&heads[k].entry).is_some_and(|t| t.lifted))
        .collect();
    let fns: Vec<&Head> = kept.iter().map(|&k| &heads[k]).collect();
    let mut undefined = 0;
    sink(File {
        name: PRELUDE_NAME.into(),
        text: PRELUDE.into(),
        fns: Vec::new(),
    });
    let mut w = Shards {
        header: format!("{}#include \"{PRELUDE_NAME}\"\n", includes(fe)),
        opts,
        targets: &targets,
        maps: Vec::new(),
        sink,
        cur: Shard::default(),
        window: None,
        last: None,
    };
    let mut flags: BTreeMap<u64, &'static str> = BTreeMap::new();
    let mut inner: BTreeMap<u64, String> = BTreeMap::new();
    let budget = opts.shard_bytes.max(1);
    let mut done = std::collections::VecDeque::new();
    for (k, f) in fns.iter().enumerate() {
        if done.is_empty() {
            let end = kept.len().min(k + BATCH);
            done.extend(compute(&kept[k..end], &targets));
        }
        let Some(e) = done.pop_front() else {
            break;
        };
        let window =
            (opts.window > 0).then(|| f.entry.saturating_sub(opts.module_base) / opts.window);
        if window != w.window && !w.cur.fns.is_empty() {
            w.flush();
            on(k, fns.len());
        }
        w.window = window;
        undefined += e.undefined;
        flags.insert(f.entry, e.flags);
        for (pc, piece) in &e.inner {
            inner.entry(*pc).or_insert_with(|| piece.clone());
        }
        // A shard map's range stops at the next function of the module, so
        // maps of different shards never overlap either.
        let span = fns
            .get(k + 1)
            .map_or(f.span, |n| f.span.min(n.entry.saturating_sub(f.entry)));
        let entry = MapEntry {
            entry: f.entry,
            fn_addr: f.entry,
            span,
            flags: e.flags,
            host: String::new(),
            guest_name: f.guest_name.clone(),
            sym: f.name.clone(),
            part: f.name.clone(),
        };
        if e.pieces.len() == 1 {
            if !w.cur.fns.is_empty() && w.cur.body.len() + e.len() > budget {
                w.flush();
                on(k, fns.len());
            }
            w.cur.callees.extend(e.callees);
            let p = e
                .pieces
                .into_iter()
                .next()
                .map(|p| p.text)
                .unwrap_or_default();
            w.push(&f.name, &p);
            w.cur.defined.push(f.name.clone());
            w.cur.fns.push(MapEntry {
                host: f.name.clone(),
                ..entry
            });
            continue;
        }
        // A split function: its pieces fill shards of their own, and each
        // piece has a map entry over the guest range it holds. The stub
        // has an empty range at the lowest address, so the table stays
        // sorted. A function split only to be entered anywhere that fits
        // in a shard shares it.
        let shared = opts.enter_anywhere.is_some() && e.len() <= budget;
        if !w.cur.fns.is_empty() && (!shared || w.cur.body.len() + e.len() > budget) {
            w.flush();
            on(k, fns.len());
        }
        let names: Vec<String> = e.pieces.iter().map(|p| p.name.clone()).collect();
        let callees = e.callees;
        if shared {
            w.cur.callees.extend(callees.iter().copied());
            w.cur.declared.extend(names.iter().cloned());
        }
        let end = f.entry.saturating_add(span);
        let starts: Vec<u64> = e.pieces.iter().filter_map(|p| p.first_pc).collect();
        let lo = if shared {
            f.entry
        } else {
            starts.first().map_or(f.entry, |&s| s.min(f.entry))
        };
        let starts: Vec<u64> = if shared {
            starts.iter().map(|&s| s.clamp(lo, end)).collect()
        } else {
            starts
        };
        let mut held = 0;
        for p in e.pieces {
            if !w.cur.fns.is_empty() && w.cur.body.len() + p.text.len() > budget {
                w.flush();
            }
            if w.cur.fns.is_empty() {
                w.cur.callees.extend(callees.iter().copied());
                w.cur.declared.extend(names.iter().cloned());
            }
            let (start, size) = match p.first_pc {
                None => (lo, 0),
                Some(pc) => {
                    let start = if held == 0 {
                        lo
                    } else {
                        starts.get(held).copied().unwrap_or(pc)
                    };
                    held += 1;
                    let stop = starts.get(held).copied().unwrap_or(end).min(end);
                    (start, stop.saturating_sub(start))
                }
            };
            w.push(&f.name, &p.text);
            w.cur.fns.push(MapEntry {
                entry: start,
                span: size,
                host: p.name.clone(),
                sym: p.name.clone(),
                ..entry.clone()
            });
            w.cur.defined.push(p.name);
        }
        if !shared {
            w.flush();
            on(k + 1, fns.len());
        }
    }
    if !w.cur.fns.is_empty() {
        w.flush();
        on(fns.len(), fns.len());
    }
    let maps = std::mem::take(&mut w.maps);
    (w.sink)(File {
        name: "unit.c".into(),
        text: unit_table(fe, &fns, overrides, &flags, &inner, &maps, opts),
        fns: Vec::new(),
    });
    undefined
}

/// One function-map entry of a shard.
#[derive(Clone)]
struct MapEntry {
    /// The first guest address the entry covers.
    entry: u64,
    /// The entry of the guest function, which a piece belongs to.
    fn_addr: u64,
    span: u64,
    flags: &'static str,
    /// The host function the entry names.
    host: String,
    /// Its symbol name, for reports.
    sym: String,
    guest_name: Option<String>,
    /// The C name of the guest function, which names its part macro.
    part: String,
}

/// The shard being filled.
#[derive(Default)]
struct Shard {
    body: String,
    callees: BTreeSet<u64>,
    /// The C functions it defines.
    defined: Vec<String>,
    /// More C functions it declares (the pieces of a split function).
    declared: Vec<String>,
    fns: Vec<MapEntry>,
}

/// The shards written so far.
struct Shards<'a> {
    header: String,
    opts: &'a Options,
    targets: &'a BTreeMap<u64, Target>,
    /// The map and the file name of each shard written.
    maps: Vec<(String, String)>,
    sink: &'a mut dyn FnMut(File),
    cur: Shard,
    /// The window of the shard being filled, with [`Options::window`].
    window: Option<u64>,
    /// The window and number in it of the last shard written.
    last: Option<(u64, u32)>,
}

impl Shards<'_> {
    /// Adds C of the guest function named `part` to the current shard,
    /// inside its part guard when the module is emitted in parts.
    fn push(&mut self, part: &str, text: &str) {
        if self.opts.parts {
            let _ = writeln!(self.cur.body, "#if defined(RL_PART_{part}) == RL_PART");
        }
        self.cur.body.push_str(text);
        if self.opts.parts {
            self.cur.body.push_str("#endif\n");
        }
    }

    /// Closes the current shard into a file.
    fn flush(&mut self) {
        let sh = std::mem::take(&mut self.cur);
        let i = self.maps.len();
        let opts = self.opts;
        let own: BTreeSet<&str> = sh.defined.iter().map(String::as_str).collect();
        let mut text = self.header.clone();
        if opts.parts {
            let _ = writeln!(
                text,
                "#include \"{PARTS_NAME}\"\n#ifndef RL_PART\n#define RL_PART 0\n#endif"
            );
        }
        let mut decl: BTreeSet<&str> = BTreeSet::new();
        for c in &sh.callees {
            if let Some(t) = self.targets.get(c).filter(|t| t.declare)
                && !own.contains(t.name.as_str())
                && decl.insert(&t.name)
            {
                let _ = writeln!(text, "RL_LIFTED({});", t.name);
            }
        }
        for d in sh.declared.iter().chain(&sh.defined) {
            if decl.insert(d) {
                let _ = writeln!(text, "RL_LIFTED({d});");
            }
        }
        text.push_str(&sh.body);
        let mut infos = String::new();
        for (k, f) in sh.fns.iter().enumerate() {
            let size = sh
                .fns
                .get(k + 1)
                .map_or(f.span, |n| f.span.min(n.entry - f.entry));
            if opts.parts {
                let _ = writeln!(infos, "#if defined(RL_PART_{}) == RL_PART", f.part);
            }
            let _ = writeln!(
                infos,
                "    {{ UINT64_C({:#x}), UINT64_C({:#x}), {}u, {}, {}, {:?}, {} }},",
                f.entry,
                f.fn_addr,
                u32::try_from(size).unwrap_or(u32::MAX),
                f.flags,
                f.host,
                f.sym,
                f.guest_name
                    .as_ref()
                    .map_or_else(|| "0".to_string(), |g| format!("{g:?}"))
            );
            if opts.parts {
                infos.push_str("#endif\n");
            }
        }
        let (n, sub) = match self.window {
            None => (i as u64, 0),
            Some(w) => match self.last {
                Some((l, s)) if l == w => (w, s + 1),
                _ => (w, 0),
            },
        };
        if let Some(w) = self.window {
            self.last = Some((w, sub));
        }
        let suffix = if sub == 0 {
            String::new()
        } else {
            format!("_{sub}")
        };
        let shard = format!("shard{n:04}{suffix}");
        let map = format!("{}_map{n}{suffix}", opts.unit_symbol);
        let mut owners: Vec<(u64, String)> = Vec::new();
        for f in &sh.fns {
            let o = (f.fn_addr, f.part.clone());
            if !owners.contains(&o) {
                owners.push(o);
            }
        }
        if opts.parts {
            // Each part maps the functions it defines; a zero entry ends
            // the table so that it is never empty.
            let _ = write!(
                text,
                "\nRL_TEXT_BOUNDS_DECLARE();\nstatic const rl_fn_info fns[] = {{\n{infos}    {{ 0 }},\n}};\n#if RL_PART == 0\n#define RL_PART_MAP {map}\n#else\n#define RL_PART_MAP {map}_p1\n#endif\nconst rl_fn_map RL_PART_MAP;\nconst rl_fn_map RL_PART_MAP = {{ RL_FN_MAP_VERSION, (uint32_t)(sizeof fns / sizeof fns[0]) - 1u, fns, {:?}, {shard:?}, UINT64_C({:#x}), RL_TEXT_LO, RL_TEXT_HI }};\n",
                opts.unit_name, opts.module_base
            );
            self.maps.push((map, shard.clone()));
            (self.sink)(File {
                name: format!("{shard}.c"),
                text,
                fns: owners,
            });
            return;
        }
        let _ = write!(
            text,
            "\nRL_TEXT_BOUNDS_DECLARE();\nstatic const rl_fn_info fns[] = {{\n{infos}}};\nconst rl_fn_map {map};\nconst rl_fn_map {map} = {{ RL_FN_MAP_VERSION, {}u, fns, {:?}, {shard:?}, UINT64_C({:#x}), RL_TEXT_LO, RL_TEXT_HI }};\n",
            sh.fns.len(),
            opts.unit_name,
            opts.module_base
        );
        self.maps.push((map, shard.clone()));
        (self.sink)(File {
            name: format!("{shard}.c"),
            text,
            fns: owners,
        });
    }
}

fn includes(fe: &dyn Frontend) -> String {
    fe.abi_headers()
        .iter()
        .map(|h| format!("#include <{h}>\n"))
        .collect()
}

fn unit_table(
    fe: &dyn Frontend,
    fns: &[&Head],
    overrides: &[Override],
    flags: &BTreeMap<u64, &'static str>,
    inner: &BTreeMap<u64, String>,
    maps: &[(String, String)],
    opts: &Options,
) -> String {
    let mut t = includes(fe);
    if opts.parts {
        let _ = writeln!(t, "#include \"{PARTS_NAME}\"");
    }
    for f in fns {
        let _ = writeln!(t, "RL_LIFTED({});", f.name);
    }
    for o in overrides.iter().filter(|o| o.declare) {
        let _ = writeln!(t, "RL_LIFTED({});", o.host_fn);
    }
    let pieces: BTreeSet<&str> = inner.values().map(String::as_str).collect();
    for p in &pieces {
        let _ = writeln!(t, "RL_LIFTED({p});");
    }
    for (m, shard) in maps {
        let _ = writeln!(t, "extern const rl_fn_map {m};");
        if opts.parts {
            let _ = writeln!(
                t,
                "#ifdef {}\nextern const rl_fn_map {m}_p1;\n#endif",
                shard_part_macro(shard)
            );
        }
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
    let entry_of: BTreeSet<u64> = entries.iter().map(|(a, _)| *a).collect();
    let pads_of: BTreeSet<u64> = if fe.pad_resume().is_some() {
        fns.iter().flat_map(|f| f.pads.iter().copied()).collect()
    } else {
        BTreeSet::new()
    };
    for (pc, piece) in inner {
        if !entry_of.contains(pc)
            && !pads_of.contains(pc)
            && !overrides.iter().any(|o| o.entry == *pc)
        {
            entries.push((
                *pc,
                format!(
                    "    {{ UINT64_C({pc:#x}), 0u, RL_FN_MAY_UNWIND | RL_FN_FRAGMENT, {piece} }},"
                ),
            ));
        }
    }
    if let Some(resume) = fe.pad_resume() {
        let taken: BTreeSet<u64> = entries.iter().map(|(a, _)| *a).collect();
        let mut pads: BTreeSet<u64> = fns.iter().flat_map(|f| f.pads.iter().copied()).collect();
        pads.retain(|p| !taken.contains(p) && !overrides.iter().any(|o| o.entry == *p));
        for p in pads {
            entries.push((
                p,
                format!(
                    "    {{ UINT64_C({p:#x}), 0u, RL_FN_MAY_UNWIND | RL_FN_FRAGMENT, {resume} }},"
                ),
            ));
        }
    }
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
    for (m, shard) in maps {
        let _ = write!(t, " &{m},");
        if opts.parts {
            let _ = write!(
                t,
                "\n#ifdef {}\n &{m}_p1,\n#endif\n",
                shard_part_macro(shard)
            );
        }
    }
    if maps.is_empty() {
        t.push_str(" 0");
    }
    let _ = write!(
        t,
        " }};\nconst rl_unit {sym};\nconst rl_unit {sym} = {{\n    RL_ABI_VERSION,\n    sizeof(rl_ctx),\n    {{ {} }},\n    {}u,\n    0,\n    fns,\n    0,\n    0,\n    {:?},\n    {},\n    maps,\n}};\n",
        opts.image_id
            .iter()
            .map(|b| format!("{b:#04x}"))
            .collect::<Vec<_>>()
            .join(", "),
        entries.len(),
        opts.unit_name,
        if opts.parts && !maps.is_empty() {
            "(uint32_t)(sizeof maps / sizeof maps[0])".to_string()
        } else {
            format!("{}u", maps.len())
        },
        sym = opts.unit_symbol
    );
    t
}

#[cfg(test)]
mod tests;
