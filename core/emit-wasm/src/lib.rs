//! IR to WebAssembly: one module per batch of guest functions, run against a
//! shared memory, table and runtime imports (`docs/abi.md` section 15).
//!
//! [`emit`] turns the functions of one module into a WebAssembly binary that
//! follows the lifted-code contract of the C emitter (`relift-emit`) where
//! WebAssembly allows it: one function per guest function with the lifted
//! signature `(c: i32, mem: i32) -> i32`, guest registers in locals loaded at
//! entry and stored at every call, trap, slow path and exit, the return code
//! checked after every call, tail calls to other functions, and indirect
//! transfers through the runtime's lookup and a shared funcref table. A
//! module imports its one linear memory, the table and the runtime, so a
//! module compiled later (code found at run time) joins the same program by
//! placing its functions at a later table slot.
//!
//! The emitter knows no CPU and no guest: it reads the [`relift_ir`] blocks
//! and their register file, and the caller names how each intrinsic is
//! carried ([`Intrinsic`]). An instruction that did not lift, or uses an
//! operation or intrinsic this emitter cannot express, reports itself
//! through the runtime's `rt.undefined` at run time, and every such
//! operation is listed in [`Output::errors`].

mod func;
mod helpers;
mod ops;

use std::collections::{BTreeMap, BTreeSet};

use relift_guest::{Frontend, LiftFailure};
use relift_ir::{Block, IoMap, Op, RegFile, Width};
use wasm_encoder::{
    CodeSection, ConstExpr, ElementSection, Elements, EntityType, ExportKind, ExportSection,
    FunctionSection, ImportSection, MemoryType, Module, RefType, TableType, TypeSection, ValType,
};

/// The context layout: byte offsets from the context address `c` in the
/// linear memory. Every guest uses the same header; register slot `k` sits at
/// [`ctx::REGS`] plus 16 bytes per slot.
pub mod ctx {
    use relift_ir::{Reg, RegFile};

    /// The target of the last indirect transfer (u64), read by the lookup's
    /// miss path; in [`crate::Exits::Step`] every exit's target.
    pub const PC: u32 = 0;
    /// The exclusive monitor's address (u64).
    pub const MONITOR_ADDR: u32 = 8;
    /// The exclusive monitor's size in bytes (u32).
    pub const MONITOR_SIZE: u32 = 16;
    /// Whether the exclusive monitor is armed (u32, 0 or 1).
    pub const MONITOR_ARMED: u32 = 20;
    /// The guest clock's budget (i64), `rl_time.budget` of section 13.
    pub const BUDGET: u32 = 24;
    /// The guest clock's deadline (u64), `rl_time.deadline` of section 13.
    pub const DEADLINE: u32 = 32;
    /// The first of 24 bytes the runtime owns (pending record, thread).
    pub const RUNTIME: u32 = 40;
    /// The first register slot.
    pub const REGS: u32 = 64;
    /// The size of one register slot.
    pub const SLOT: u32 = 16;
    /// The alignment the runtime gives each context.
    pub const ALIGN: u32 = 16;

    /// The offset of register slot `r`. A slot holds its value little-endian
    /// and zero-extended; a slot of 64 bits or fewer uses its first 8 bytes.
    pub fn slot(r: Reg) -> u32 {
        REGS + SLOT * u32::from(r.0)
    }

    /// The context size for register file `regs`.
    pub fn size(regs: &RegFile) -> u32 {
        REGS + SLOT * u32::try_from(regs.len()).unwrap_or(u32::MAX / SLOT - REGS)
    }
}

/// The return code of a normal return (`RL_OK`).
pub const RC_OK: i32 = 0;
/// The return code when the host stack must unwind (`RL_PENDING`).
pub const RC_PENDING: i32 = 1;
/// The return code of a transfer to another function when tail calls are
/// off: the target is in [`ctx::PC`] and the caller dispatches it.
pub const RC_CHAIN: i32 = 2;

/// The import module of the memory and the table.
pub const ENV: &str = "env";
/// The import module of the runtime entries.
pub const RT: &str = "rt";
/// The import module of intrinsics carried as [`Intrinsic::Import`].
pub const INTRINSIC: &str = "intrinsic";

/// The runtime entries every module imports from [`RT`], in import order,
/// with their parameter and result types.
pub const RUNTIME_IMPORTS: &[(&str, &[ValType], &[ValType])] = &[
    (
        "slow",
        &[
            ValType::I32,
            ValType::I32,
            ValType::I32,
            ValType::I64,
            ValType::I64,
        ],
        &[ValType::I32],
    ),
    ("lookup", &[ValType::I32, ValType::I64], &[ValType::I32]),
    (
        "trap",
        &[ValType::I32, ValType::I32, ValType::I32, ValType::I64],
        &[ValType::I32],
    ),
    (
        "undefined",
        &[ValType::I32, ValType::I32, ValType::I64, ValType::I64],
        &[ValType::I32],
    ),
    (
        "fma32",
        &[ValType::F32, ValType::F32, ValType::F32],
        &[ValType::F32],
    ),
    (
        "fma64",
        &[ValType::F64, ValType::F64, ValType::F64],
        &[ValType::F64],
    ),
    (
        "div128",
        &[
            ValType::I64,
            ValType::I64,
            ValType::I64,
            ValType::I64,
            ValType::I32,
            ValType::I32,
        ],
        &[ValType::I64, ValType::I64],
    ),
    (
        "io_read",
        &[ValType::I32, ValType::I32, ValType::I64, ValType::I32],
        &[ValType::I64],
    ),
    (
        "io_write",
        &[
            ValType::I32,
            ValType::I32,
            ValType::I64,
            ValType::I32,
            ValType::I64,
        ],
        &[],
    ),
    (
        "sync",
        &[ValType::I32, ValType::I32, ValType::I64],
        &[ValType::I32],
    ),
];

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
    /// The code bank of a banked function (`Block::bank`); a function with a
    /// bank is reached only through the runtime's lookup.
    pub bank: Option<u64>,
    /// The export name, when [`Options::export`] is set.
    pub name: String,
    /// The instructions by address.
    pub insts: BTreeMap<u64, Insn<'a>>,
    /// Known targets of computed jumps, by the jump's address; the emitter
    /// branches to those inside the function directly.
    pub tables: BTreeMap<u64, Vec<u64>>,
}

/// How block exits leave a function.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exits {
    /// Real modules: branches inside the function stay inside, calls call,
    /// returns return, transfers to other functions are tail calls, and
    /// sync points call `rt.sync`, which runs a taken interrupt itself.
    Chain,
    /// The checker's mode: every exit stores its target in [`ctx::PC`] and
    /// returns [`RC_OK`], so the caller steps one exit at a time, as the IR
    /// interpreter does; a sync point calls `rt.sync`, which redirects
    /// [`ctx::PC`] to a taken interrupt's entry.
    Step,
}

/// How an [`Op::Intrinsic`] is carried; the frontend says, through
/// [`Frontend::wasm_intrinsic`]. An intrinsic of [`Intrinsic::Import`]
/// becomes an import [`INTRINSIC`]`.<name>` with parameters `(c: i32, mem:
/// i32)` then one `i64` per operand of up to 64 bits or a low and high
/// `i64` per wider one, and no result, one `i64`, or a low and high `i64`;
/// the clock budget is stored before the call and reloaded after it.
pub use relift_guest::WasmIntrinsic as Intrinsic;

/// How the frontend carries each intrinsic the functions use, for
/// [`Options::intrinsics`]; names it does not know are left out, and their
/// uses become emit errors.
pub fn intrinsics_of(frontend: &dyn Frontend, fns: &[Function<'_>]) -> BTreeMap<String, Intrinsic> {
    let mut out = BTreeMap::new();
    for b in fns.iter().flat_map(lifted) {
        for inst in &b.insts {
            if let Op::Intrinsic(name, _) = inst.op
                && let Some(k) = frontend.wasm_intrinsic(name)
            {
                out.insert(name.to_string(), k);
            }
        }
    }
    out
}

/// Emitter options.
#[derive(Clone, Debug)]
pub struct Options {
    /// How exits leave functions.
    pub exits: Exits,
    /// Whether transfers to other functions use `return_call` and
    /// `return_call_indirect`. Without them a transfer stores its target in
    /// [`ctx::PC`] and returns [`RC_CHAIN`], and every call site runs the
    /// module's dispatch loop on the result.
    pub tail_calls: bool,
    /// The table slot of the first function; function `i` goes to slot
    /// `table_base + i`.
    pub table_base: u32,
    /// The minimum pages the memory import declares.
    pub memory_pages: u64,
    /// The guest's I/O map, for I/O accesses of kind `Any`.
    pub io_map: IoMap,
    /// How each intrinsic is carried, by name.
    pub intrinsics: BTreeMap<String, Intrinsic>,
    /// Whether every function is exported under [`Function::name`].
    pub export: bool,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            exits: Exits::Chain,
            tail_calls: true,
            table_base: 0,
            memory_pages: 1,
            io_map: IoMap::memory(),
            intrinsics: BTreeMap::new(),
            export: false,
        }
    }
}

/// An instruction the emitter could not express; it reports itself through
/// `rt.undefined` at run time.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EmitError {
    /// The entry of the function holding it.
    pub entry: u64,
    /// The instruction's address.
    pub pc: u64,
    /// What could not be expressed.
    pub message: String,
}

/// What [`emit`] produced.
#[derive(Clone, Debug, Default)]
pub struct Output {
    /// The module binary.
    pub bytes: Vec<u8>,
    /// Each function's entry, bank and table slot, for the runtime's lookup.
    pub slots: Vec<(u64, Option<u64>, u32)>,
    /// The instructions that use something the emitter cannot express.
    pub errors: Vec<EmitError>,
    /// How many instructions report themselves as undefined at run time:
    /// the failed lifts and the [`Output::errors`].
    pub undefined: usize,
}

pub(crate) const T_LIFTED: u32 = 0;
pub(crate) const T_MULHI: u32 = RUNTIME_IMPORTS.len() as u32 + 1;
pub(crate) const T_SH128: u32 = T_MULHI + 1;
pub(crate) const T_F32: u32 = T_MULHI + 2;
pub(crate) const T_F64: u32 = T_MULHI + 3;
pub(crate) const T_PAIR: u32 = T_MULHI + 4;
pub(crate) const T_UN64: u32 = T_MULHI + 5;
pub(crate) const T_RUN: u32 = T_MULHI + 6;
const N_FIXED_TYPES: u32 = T_RUN + 1;

/// The runtime import indices, which are also their function indices.
pub(crate) mod rt {
    pub(crate) const SLOW: u32 = 0;
    pub(crate) const LOOKUP: u32 = 1;
    pub(crate) const TRAP: u32 = 2;
    pub(crate) const UNDEFINED: u32 = 3;
    pub(crate) const FMA32: u32 = 4;
    pub(crate) const FMA64: u32 = 5;
    pub(crate) const DIV128: u32 = 6;
    pub(crate) const IO_READ: u32 = 7;
    pub(crate) const IO_WRITE: u32 = 8;
    pub(crate) const SYNC: u32 = 9;
}

/// The helper functions every module defines, in order after the imports.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Helper {
    MulHi,
    Shl128,
    LShr128,
    AShr128,
    RoundAway32,
    RoundAway64,
    BitReverse64,
    ByteSwap64,
    Run,
}

pub(crate) const HELPERS: [Helper; 9] = [
    Helper::MulHi,
    Helper::Shl128,
    Helper::LShr128,
    Helper::AShr128,
    Helper::RoundAway32,
    Helper::RoundAway64,
    Helper::BitReverse64,
    Helper::ByteSwap64,
    Helper::Run,
];

/// What every function of one module shares.
pub(crate) struct ModuleCx<'a> {
    pub(crate) opts: &'a Options,
    pub(crate) regs: RegFile,
    /// Function indices of this module's functions by entry.
    pub(crate) targets: BTreeMap<u64, u32>,
    /// Function index and import type of each imported intrinsic.
    pub(crate) intrinsics: BTreeMap<&'static str, u32>,
    pub(crate) n_imports: u32,
}

impl ModuleCx<'_> {
    pub(crate) fn helper(&self, h: Helper) -> u32 {
        self.n_imports + HELPERS.iter().position(|x| *x == h).unwrap_or(0) as u32
    }
}

fn val_types(w: Option<Width>) -> Vec<ValType> {
    match w {
        None => Vec::new(),
        Some(w) if w.bits() <= 64 => vec![ValType::I64],
        Some(_) => vec![ValType::I64, ValType::I64],
    }
}

/// The parameter and result types of intrinsic `op` in `b`, when it is one.
fn intrinsic_sig(b: &Block, op: &Op, width: Option<Width>) -> Option<(Vec<ValType>, Vec<ValType>)> {
    let Op::Intrinsic(_, args) = op else {
        return None;
    };
    let mut params = vec![ValType::I32, ValType::I32];
    for a in args {
        params.extend(val_types(b.width_of(*a)));
    }
    Some((params, val_types(width)))
}

fn lifted<'a>(f: &Function<'a>) -> impl Iterator<Item = &'a Block> {
    f.insts.values().filter_map(|i| match i {
        Insn::Lifted { block, .. } => Some(*block),
        Insn::Failed(_) => None,
    })
}

/// Emits the functions `fns` as one module.
pub fn emit(fns: &[Function<'_>], opts: &Options) -> Output {
    let regs = fns
        .iter()
        .flat_map(lifted)
        .map(|b| b.regs)
        .next()
        .unwrap_or_else(|| RegFile::new("none", &[]));
    let mut types = TypeSection::new();
    let mut sigs: Vec<(Vec<ValType>, Vec<ValType>)> = Vec::new();
    sigs.push((vec![ValType::I32, ValType::I32], vec![ValType::I32]));
    for (_, p, r) in RUNTIME_IMPORTS {
        sigs.push((p.to_vec(), r.to_vec()));
    }
    use ValType::{F32, F64, I32, I64};
    sigs.push((vec![I64, I64], vec![I64]));
    sigs.push((vec![I64, I64, I64], vec![I64, I64]));
    sigs.push((vec![F32], vec![F32]));
    sigs.push((vec![F64], vec![F64]));
    sigs.push((vec![], vec![I64, I64]));
    sigs.push((vec![I64], vec![I64]));
    sigs.push((vec![I32, I32, I32], vec![I32]));
    debug_assert_eq!(sigs.len() as u32, N_FIXED_TYPES);
    let mut imports = ImportSection::new();
    imports.import(
        ENV,
        "memory",
        EntityType::Memory(MemoryType {
            minimum: opts.memory_pages,
            maximum: None,
            memory64: false,
            shared: false,
            page_size_log2: None,
        }),
    );
    imports.import(
        ENV,
        "table",
        EntityType::Table(TableType {
            element_type: RefType::FUNCREF,
            table64: false,
            minimum: u64::from(opts.table_base) + fns.len() as u64,
            maximum: None,
            shared: false,
        }),
    );
    for (i, (name, _, _)) in RUNTIME_IMPORTS.iter().enumerate() {
        imports.import(RT, name, EntityType::Function(1 + i as u32));
    }
    let mut wanted: BTreeMap<&'static str, (Vec<ValType>, Vec<ValType>)> = BTreeMap::new();
    let mut clash: BTreeSet<&'static str> = BTreeSet::new();
    for b in fns.iter().flat_map(lifted) {
        for inst in &b.insts {
            if let Op::Intrinsic(name, _) = inst.op
                && opts.intrinsics.get(name) == Some(&Intrinsic::Import)
                && let Some(sig) = intrinsic_sig(b, &inst.op, inst.width)
            {
                match wanted.get(name) {
                    Some(have) if *have != sig => {
                        clash.insert(name);
                    }
                    Some(_) => {}
                    None => {
                        wanted.insert(name, sig);
                    }
                }
            }
        }
    }
    let mut intrinsics = BTreeMap::new();
    let mut n_imports = RUNTIME_IMPORTS.len() as u32;
    for (name, sig) in &wanted {
        if clash.contains(name) {
            continue;
        }
        let ty = match sigs.iter().position(|s| s == sig) {
            Some(t) => t as u32,
            None => {
                sigs.push(sig.clone());
                sigs.len() as u32 - 1
            }
        };
        imports.import(INTRINSIC, name, EntityType::Function(ty));
        intrinsics.insert(*name, n_imports);
        n_imports += 1;
    }
    for (p, r) in &sigs {
        types.ty().function(p.iter().copied(), r.iter().copied());
    }
    let first_fn = n_imports + HELPERS.len() as u32;
    let cx = ModuleCx {
        opts,
        regs,
        targets: fns
            .iter()
            .enumerate()
            .filter(|(_, f)| f.bank.is_none())
            .map(|(i, f)| (f.entry, first_fn + i as u32))
            .collect(),
        intrinsics,
        n_imports,
    };
    let mut funcs = FunctionSection::new();
    let mut code = CodeSection::new();
    for h in HELPERS {
        funcs.function(helpers::type_of(h));
        code.function(&helpers::body(h));
    }
    let mut out = Output::default();
    let mut exports = ExportSection::new();
    for (i, f) in fns.iter().enumerate() {
        let idx = first_fn + i as u32;
        funcs.function(T_LIFTED);
        let e = func::emit_function(&cx, f, &clash);
        code.function(&e.func);
        out.undefined += e.undefined;
        out.errors.extend(e.errors);
        out.slots
            .push((f.entry, f.bank, opts.table_base + i as u32));
        if opts.export {
            exports.export(&f.name, ExportKind::Func, idx);
        }
    }
    let mut elems = ElementSection::new();
    let idxs: Vec<u32> = (0..fns.len() as u32).map(|i| first_fn + i).collect();
    elems.active(
        Some(0),
        &ConstExpr::i32_const(opts.table_base as i32),
        Elements::Functions(idxs.into()),
    );
    let mut m = Module::new();
    m.section(&types);
    m.section(&imports);
    m.section(&funcs);
    m.section(&exports);
    m.section(&elems);
    m.section(&code);
    out.bytes = m.finish();
    out
}
