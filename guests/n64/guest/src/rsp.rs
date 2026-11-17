//! The RSP frontend of the Nintendo 64 guest and the lifted RSP images.
//!
//! Lifting is `relift-cpu-rsp`'s, reshaped for the runtime's RSP layer
//! (`runtime/cpus/rsp`, ABI `relift/rsp/abi.h`):
//!
//! - an image is lifted at its own offsets (IMEM address minus the base it
//!   runs at, modulo 4 KiB), so it runs wherever the program loads it: the
//!   targets of `J`, `JAL`, `JR` and `JALR`, which are IMEM addresses,
//!   become offsets by subtracting the base (`rl_rsp_base`), and the links
//!   of `JAL`, `JALR`, `BGEZAL` and `BLTZAL` become addresses by adding it;
//! - every instruction costs one RSP cycle, and every block ends in a sync
//!   point whose slow path is `rl_rsp_sync`;
//! - DMEM is the memory of the lifted code, and the lifter masks every
//!   address to it;
//! - `MFC0`, `MTC0` and `BREAK` call the layer (`rl_rsp_mfc0`,
//!   `rl_rsp_mtc0`, `rl_rsp_break`), which ends the budget after a write or
//!   a `BREAK`, so the block's sync point sees the SP side's new state.
//!
//! Each [`relift_n64_analyze::RspImage`] becomes units of at most 4 KiB
//! (one image each; a longer run of RSP code is cut into pieces that
//! overlap by a word and its delay slot). The program lists each image with
//! its bytes and, per word, the words its block goes to without a computed
//! jump, so the layer checks the bytes in IMEM against the image before it
//! enters lifted code there.
//!
//! Each [`relift_n64_analyze::RspLayout`] (an IMEM layout a microcode makes
//! with the overlays its own DMAs load) becomes an image at IMEM 0 too. In
//! it a branch or fall-through to a word the microcode's DMAs may overwrite
//! (or that the layout does not know) goes through dispatch and is no
//! successor, so the image binds where IMEM holds its bytes and the layer
//! checks the overwritten words on their own. An image of all 4 KiB wraps:
//! the delay slot of its last word is its first.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_cpu_rsp::decode::Encoding;
use relift_cpu_rsp::lift::{self, LiftError};
use relift_cpu_rsp::regs;
use relift_emit::{Function, Insn, Options, Output};
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{
    BinOp, Block, CryptoOp, Flow, FpOp, FpRound, Inst, IoMap, Konst, Next, Op, Reg, RegFile, Value,
    Width,
};
use relift_n64_analyze::{Report, RspImage, Source};

/// Bytes of IMEM, and the longest image.
pub const IMEM_SIZE: u32 = 0x1000;
/// The step between the pieces of a longer run of RSP code: each piece
/// starts 8 bytes before the end of the one before it, so a branch in a
/// piece's last word lifts with its delay slot in the next piece.
const PIECE_STEP: u32 = IMEM_SIZE - 8;
/// The intrinsic of the running image's base.
pub const BASE: &str = "rsp.base";
/// The successor entry of a word whose block goes nowhere else without a
/// computed jump (`RL_RSP_NO_SUCC`).
pub const NO_SUCC: u16 = 0xFFFF;
/// The flag of a block that holds the next word as its delay slot
/// (`RL_RSP_SUCC_SLOT`).
pub const SUCC_SLOT: u16 = 1;

/// The context fields after the vector registers, in slot order from `ACCH`.
const FIELDS: [&str; 11] = [
    "acch", "accm", "accl", "vcol", "vcoh", "vccl", "vcch", "vce", "divin", "divout", "divdp",
];

/// The `rl_ctx` field of slot `r` (`RL_RSP_SLOTS` in `relift/rsp/abi.h`).
#[must_use]
pub fn field(r: Reg) -> String {
    let i = r.index();
    match i {
        0..=31 => format!("r[{i}]"),
        32..=63 => format!("v[{}]", i - 32),
        _ => FIELDS.get(i - 64).copied().unwrap_or("?").into(),
    }
}

/// The frontend.
#[derive(Clone, Copy, Debug, Default)]
pub struct Rsp;

fn be32(bytes: &[u8], at: usize) -> u32 {
    bytes
        .get(at..at + 4)
        .map_or(0, |w| u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
}

fn failure(word: u32, e: &LiftError) -> LiftFailure {
    LiftFailure {
        insn: u64::from(word),
        message: e.to_string(),
    }
}

fn push(b: &mut Block, op: Op, width: Option<Width>) -> Value {
    b.insts.push(Inst { op, width });
    Value((b.insts.len() - 1) as u32)
}

fn konst(b: &mut Block, v: u64) -> Value {
    push(b, Op::Const(Konst::new(u128::from(v))), Some(Width::W32))
}

/// `v` plus (`add`) or minus the running image's base, wrapped to IMEM.
fn rebase(b: &mut Block, v: Value, add: bool) -> Value {
    let base = push(b, Op::intrinsic(BASE, Vec::new()), Some(Width::W32));
    let op = if add { BinOp::Add } else { BinOp::Sub };
    let r = push(b, Op::Bin(op, v, base), Some(Width::W32));
    let m = konst(b, 0xFFC);
    push(b, Op::Bin(BinOp::And, r, m), Some(Width::W32))
}

/// The register a branch or jump `enc` (word `word`) links, if any.
fn link_reg(enc: Encoding, word: u32) -> Option<u8> {
    match enc {
        Encoding::Jal | Encoding::Bgezal | Encoding::Bltzal => Some(31),
        Encoding::Jalr => Some(((word >> 11) & 31) as u8).filter(|r| *r != 0),
        _ => None,
    }
}

/// Inserts the instructions `f` pushes before instruction `at` of `b` and
/// returns the value `f` gives; later instructions and the exit are
/// renumbered.
fn insert(b: &mut Block, at: usize, f: impl FnOnce(&mut Block) -> Value) -> Value {
    let tail = b.insts.split_off(at);
    let v = f(b);
    let shift = (b.insts.len() - at) as u32;
    let moved = |x: &Value| {
        if x.index() >= at {
            Value(x.0 + shift)
        } else {
            *x
        }
    };
    for mut inst in tail {
        inst.op = inst.op.map_values(&moved);
        b.insts.push(inst);
    }
    b.exit.next = match b.exit.next {
        Next::Dynamic(t) => Next::Dynamic(moved(&t)),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => Next::Select {
            cond: moved(&cond),
            taken,
            not_taken,
        },
        n => n,
    };
    v
}

/// Moves `b`, lifted at offset `pc`, to run at any base: the link becomes
/// an address and an absolute target an offset. The delay slot reads the
/// link before the block writes it back, so every use of the link's value
/// takes the address.
fn relocate(b: &mut Block, pc: u64, enc: Encoding, word: u32) {
    if let Some(r) = link_reg(enc, word) {
        let reg = regs::gpr(r);
        let old = b.insts.iter().find_map(|i| match i.op {
            Op::Set(g, v) if g == reg => Some(v),
            _ => None,
        });
        if let Some(old) = old {
            let link = (pc + 8) & 0xFFF;
            let from = old.index() + 1;
            let a = insert(b, from, |b| {
                let k = konst(b, link);
                rebase(b, k, true)
            });
            let swap = |x: &Value| if *x == old { a } else { *x };
            for i in a.index() + 1..b.insts.len() {
                let op = std::mem::replace(&mut b.insts[i].op, Op::Sync);
                b.insts[i].op = op.map_values(&swap);
            }
            b.exit.next = match b.exit.next {
                Next::Dynamic(t) => Next::Dynamic(swap(&t)),
                Next::Select {
                    cond,
                    taken,
                    not_taken,
                } => Next::Select {
                    cond: swap(&cond),
                    taken,
                    not_taken,
                },
                n => n,
            };
        }
    }
    match (enc, b.exit.next) {
        (Encoding::J | Encoding::Jal, Next::Const(t)) => {
            let k = konst(b, t);
            b.exit.next = Next::Dynamic(rebase(b, k, false));
        }
        (Encoding::Jr | Encoding::Jalr, Next::Dynamic(v)) => {
            b.exit.next = Next::Dynamic(rebase(b, v, false));
        }
        _ => {}
    }
}

impl Rsp {
    /// Lifts the instruction `word` at offset `pc` of an image, with `slot`
    /// the word after it (its delay slot when it has one), for the RSP
    /// layer.
    ///
    /// # Errors
    ///
    /// The instruction does not lift.
    pub fn lift_at(self, pc: u64, word: u32, slot: u32) -> Result<Block, LiftFailure> {
        let l = lift::lift(pc, word, slot).map_err(|e| failure(word, &e))?;
        let mut b = l.block;
        let n = l.encs.len() as u32;
        if let Some(Some(enc)) = l.encs.first() {
            relocate(&mut b, pc, *enc, word);
        }
        b.insts.push(Inst {
            op: Op::Tick(n),
            width: None,
        });
        b.insts.push(Inst {
            op: Op::Sync,
            width: None,
        });
        Ok(b)
    }
}

impl Frontend for Rsp {
    fn granule(&self) -> u64 {
        4
    }

    fn regs(&self) -> RegFile {
        regs::regs()
    }

    fn stack_pointer(&self) -> Option<Reg> {
        None
    }

    fn preserved(&self) -> Vec<Reg> {
        Vec::new()
    }

    /// Lifts the word at the start of `bytes` (big-endian) with the next
    /// word as its delay slot.
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        self.lift_at(pc, be32(bytes, 0), be32(bytes, 4))
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/rsp/abi.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        Home::Field(format!("c->{}", field(reg)))
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Some(match (name, args) {
            ("rsp_mfc0", [rd]) => format!("rl_rsp_mfc0(c, (uint32_t)({rd}))"),
            ("rsp_mtc0", [rd, v, run]) => {
                format!("rl_rsp_mtc0(c, (uint32_t)({rd}), (uint32_t)({v}), (uint32_t)({run}))")
            }
            ("rsp_break", [run]) => format!("rl_rsp_break(c, (uint32_t)({run}))"),
            (BASE, []) => "rl_rsp_base(c)".into(),
            _ => return None,
        })
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_rsp_undefined(c, mem, c->pc, {code:#x}u)")
    }

    fn sync_call(&self, _b: &Block, next: &str) -> Option<String> {
        Some(format!("rl_rsp_sync(c, mem, {next})"))
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        format!(
            "rl_rsp_undefined(c, mem, UINT64_C({pc:#x}), {:#x}u)",
            insn as u32
        )
    }

    fn monitor(&self) -> &'static str {
        "0"
    }

    /// The f64 operations of the VRSQ ROM formula (`rl_rsp_i2f64`,
    /// `rl_rsp_fsqrt64`, `rl_rsp_f2i_ceil64`); the RSP has no other.
    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        _reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        if from != Width::W64 || to != Width::W64 {
            return None;
        }
        let a = args.first()?;
        Some(match op {
            FpOp::FromSInt => format!("rl_rsp_i2f64({a})"),
            FpOp::Sqrt => format!("rl_rsp_fsqrt64({a})"),
            FpOp::ToSInt(FpRound::Ceil) => format!("rl_rsp_f2i_ceil64({a})"),
            _ => return None,
        })
    }

    fn crypto(&self, _op: CryptoOp, _arg: &str) -> Option<String> {
        None
    }

    fn clock(&self) -> Option<&'static str> {
        Some("c->time")
    }

    fn fp_state(&self) -> Vec<Reg> {
        Vec::new()
    }

    fn guard_state(&self, _code: u32) -> Option<Reg> {
        None
    }

    fn guard_slow(&self, _code: u32, _pc: u64, _insn: u64) -> Option<String> {
        None
    }
}

/// An RSP instruction that did not lift.
#[derive(Clone, Debug)]
pub struct Unlifted {
    /// The image, an index into [`Images::images`].
    pub image: usize,
    /// The IMEM address.
    pub pc: u64,
    /// Why.
    pub failure: LiftFailure,
}

/// One lifted RSP image.
#[derive(Clone, Debug)]
pub struct Image {
    /// Where its bytes come from.
    pub source: Source,
    /// Their offset in the source.
    pub offset: u32,
    /// FNV-1a (64-bit) of the bytes.
    pub hash: u64,
    /// The C symbol of its unit.
    pub symbol: String,
    /// The directory name of its files.
    pub dir: String,
    /// The emitted files.
    pub output: Output,
    /// Its functions.
    pub functions: usize,
    /// The bytes, big-endian.
    pub bytes: Vec<u8>,
    /// Three entries per word: [`NO_SUCC`] or the words (IMEM address / 4)
    /// its block goes to without a computed jump, then [`SUCC_SLOT`] or 0.
    pub succ: Vec<u16>,
}

/// Every RSP image a ROM lifts to.
#[derive(Clone, Debug, Default)]
pub struct Images {
    /// The images, lifted.
    pub images: Vec<Image>,
    /// Instructions lifted.
    pub instructions: usize,
    /// Instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
}

impl Images {
    /// The functions of every image.
    #[must_use]
    pub fn functions(&self) -> usize {
        self.images.iter().map(|i| i.functions).sum()
    }
}

/// The C symbol of the unit of RSP image `k`.
#[must_use]
pub fn unit_symbol(k: usize) -> String {
    format!("rl_unit_n64_rsp{k}")
}

/// The C symbol of the `rl_rsp_image` of RSP image `k`.
#[must_use]
pub fn image_symbol(k: usize) -> String {
    format!("rl_n64_rsp_image{k}")
}

type Blocks = BTreeMap<u64, Result<Block, LiftFailure>>;

/// The successors of `b` inside `[imem, end)`, as word indexes. A block
/// that holds `BREAK` has none: the RSP halts there, its sync point
/// returns to the layer, and a restart enters through dispatch.
fn succ_of(b: &Block, imem: u64, end: u64) -> [u16; 3] {
    let slot = if b.len == 8 { SUCC_SLOT } else { 0 };
    if b.insts
        .iter()
        .any(|i| matches!(&i.op, Op::Intrinsic(c) if c.name == "rsp_break"))
    {
        return [NO_SUCC, NO_SUCC, slot];
    }
    let inside = |t: u64| {
        let t = t & 0xFFF;
        if t >= imem && t < end {
            (t >> 2) as u16
        } else {
            NO_SUCC
        }
    };
    let (a, c) = match (b.exit.flow, b.exit.next) {
        (Flow::Jump, Next::Const(t)) => (inside(t), NO_SUCC),
        (
            Flow::Jump,
            Next::Select {
                taken, not_taken, ..
            },
        ) => (inside(taken), inside(not_taken)),
        _ => (NO_SUCC, NO_SUCC),
    };
    [a, c, slot]
}

/// The words a run from `entry` reaches without a computed jump, stopping
/// at those of `taken` (other functions', which a jump there enters
/// through dispatch).
fn reach(blocks: &Blocks, entry: u64, taken: &BTreeSet<u64>) -> BTreeSet<u64> {
    let mut seen = BTreeSet::new();
    let mut todo = vec![entry];
    while let Some(pc) = todo.pop() {
        if !blocks.contains_key(&pc) || (pc != entry && taken.contains(&pc)) || !seen.insert(pc) {
            continue;
        }
        let Some(Ok(b)) = blocks.get(&pc) else {
            continue;
        };
        match b.exit.next {
            Next::Const(t) => todo.push(t & 0xFFF),
            Next::Select {
                taken, not_taken, ..
            } => {
                todo.push(taken & 0xFFF);
                todo.push(not_taken & 0xFFF);
            }
            Next::Dynamic(_) => {}
        }
    }
    seen
}

fn source_bytes<'a>(rom: &'a [u8], streams: &'a [Vec<u8>], s: Source) -> &'a [u8] {
    match s {
        Source::Rom => rom,
        Source::Stream(i) => streams.get(i as usize).map_or(&[][..], Vec::as_slice),
    }
}

/// FNV-1a (64-bit) of `b`.
fn fnv(b: &[u8]) -> u64 {
    b.iter().fold(0xCBF2_9CE4_8422_2325, |h, &x| {
        (h ^ u64::from(x)).wrapping_mul(0x0100_0000_01B3)
    })
}

/// The offsets and words of the instructions whose C reports them undefined
/// at run time (`rl_rsp_undefined` at a constant offset): those that did
/// not lift and those the emitter has no C form of.
fn undefined_in(output: &Output) -> Vec<(u64, u32)> {
    const CALL: &str = "rl_rsp_undefined(c, mem, UINT64_C(";
    let mut out = Vec::new();
    for f in &output.files {
        for (at, _) in f.text.match_indices(CALL) {
            let rest = &f.text[at + CALL.len()..];
            let Some((pc, rest)) = rest.split_once("), ") else {
                continue;
            };
            let Some((insn, _)) = rest.split_once("u)") else {
                continue;
            };
            let hex = |s: &str| u64::from_str_radix(s.trim_start_matches("0x"), 16).ok();
            if let (Some(pc), Some(insn)) = (hex(pc), hex(insn)) {
                out.push((pc, insn as u32));
            }
        }
    }
    out.sort_unstable();
    out.dedup();
    out
}

/// The pieces the images of the analysis become: per source, the union of
/// the images' byte ranges, cut into pieces of at most 4 KiB that overlap
/// by 8 bytes, as (source, offset, length).
fn pieces(images: &[RspImage]) -> Vec<(Source, u32, u32)> {
    let mut by: BTreeMap<(u8, u32), Vec<(u32, u32)>> = BTreeMap::new();
    for im in images {
        let key = match im.source {
            Source::Rom => (0, 0),
            Source::Stream(n) => (1, n),
        };
        let start = im.offset & !3;
        by.entry(key)
            .or_default()
            .push((start, (im.offset + im.len).next_multiple_of(4)));
    }
    let mut out = Vec::new();
    for ((kind, n), mut ranges) in by {
        let source = if kind == 0 {
            Source::Rom
        } else {
            Source::Stream(n)
        };
        ranges.sort_unstable();
        let mut merged: Vec<(u32, u32)> = Vec::new();
        for (a, b) in ranges {
            match merged.last_mut() {
                Some(last) if a <= last.1 => last.1 = last.1.max(b),
                _ => merged.push((a, b)),
            }
        }
        for (a, b) in merged {
            let mut at = a;
            loop {
                let len = (b - at).min(IMEM_SIZE);
                out.push((source, at, len));
                if at + len >= b {
                    break;
                }
                at += PIECE_STEP;
            }
        }
    }
    out
}

/// One image to lift: where its bytes come from, the bytes, which words are
/// code and which a transfer enters only through dispatch.
struct Plan {
    source: Source,
    offset: u32,
    bytes: Vec<u8>,
    /// Per word: it is code of the image.
    known: Vec<bool>,
    /// Per word: a branch or fall-through there goes through dispatch.
    cut: Vec<bool>,
}

/// The images of the IMEM layouts of `report`: each at IMEM 0, up to its
/// last known word, with the words a DMA of its microcode may overwrite
/// (all, when the analysis could not tell) and the words it does not know
/// entered only through dispatch.
fn layout_plans<'a>(src: &dyn Fn(Source) -> &'a [u8], report: &Report) -> Vec<Plan> {
    let mut out = Vec::new();
    for l in &report.rsp_layouts {
        let Some(first) = l.parts.first() else {
            continue;
        };
        let end = l.parts.iter().map(|p| p.imem + p.len).max().unwrap_or(0);
        let words = (end / 4) as usize;
        let mut bytes = vec![0u8; end as usize];
        let mut known = vec![false; words];
        for p in &l.parts {
            let Some(b) = src(p.source).get(p.offset as usize..(p.offset + p.len) as usize) else {
                continue;
            };
            bytes[p.imem as usize..(p.imem + p.len) as usize].copy_from_slice(b);
            for k in p.imem / 4..(p.imem + p.len) / 4 {
                known[k as usize] = true;
            }
        }
        let cut = (0..words)
            .map(|k| {
                let a = 4 * k as u32;
                l.unknown || !known[k] || l.ranges.iter().any(|&(lo, hi)| (lo..hi).contains(&a))
            })
            .collect();
        out.push(Plan {
            source: first.source,
            offset: first.offset,
            bytes,
            known,
            cut,
        });
    }
    out
}

/// Sends the constant targets of `b`'s exit that `cut` holds through
/// dispatch: the transfer becomes a computed one, which `rl_indirect`
/// enters after the layer checks the bytes there.
fn cut_exit(b: &mut Block, cut: impl Fn(u64) -> bool) {
    let sync = matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)).then(|| b.insts.pop());
    match b.exit.next {
        Next::Const(t) if matches!(b.exit.flow, Flow::Jump) && cut(t) => {
            b.exit.next = Next::Dynamic(konst(b, t));
        }
        Next::Select {
            cond,
            taken,
            not_taken,
        } if matches!(b.exit.flow, Flow::Jump) && (cut(taken) || cut(not_taken)) => {
            let t = konst(b, taken);
            let n = konst(b, not_taken);
            b.exit.next = Next::Dynamic(push(b, Op::Select(cond, t, n), Some(Width::W32)));
        }
        _ => {}
    }
    b.insts.extend(sync.flatten());
}

/// Lifts every RSP image of `report` (the analysis of the big-endian image
/// `rom`), one unit each: the runs of RSP code as relocatable images of at
/// most 4 KiB, and each IMEM layout the microcodes make (with their
/// overlays) as an image at IMEM 0. `image_id` identifies the ROM to the
/// runtime.
#[must_use]
pub fn lift(rom: &[u8], report: &Report, image_id: [u8; 32]) -> Images {
    let streams: Vec<Vec<u8>> = report
        .streams
        .iter()
        .map(|s| s.kind.decode(&rom[s.offset..]).unwrap_or_default())
        .collect();
    let src = |s: Source| source_bytes(rom, &streams, s);
    let mut ranges: Vec<RspImage> = report.rsp.clone();
    ranges.extend(report.rsp_layouts.iter().flat_map(|l| {
        l.parts.iter().map(|p| RspImage {
            source: p.source,
            offset: p.offset,
            len: p.len,
            imem: Some(p.imem),
            hash: 0,
            origin: relift_n64_analyze::RspOrigin::Scan,
        })
    }));
    let mut plans: Vec<Plan> = Vec::new();
    for (source, start, len) in pieces(&ranges) {
        let Some(bytes) = src(source).get(start as usize..(start + len) as usize) else {
            continue;
        };
        let words = (len / 4) as usize;
        plans.push(Plan {
            source,
            offset: start,
            bytes: bytes.to_vec(),
            known: vec![true; words],
            cut: vec![false; words],
        });
    }
    plans.extend(layout_plans(&src, report));
    let mut out = Images::default();
    for plan in plans {
        lift_plan(&mut out, plan, image_id);
    }
    out
}

/// Lifts `plan` as the next image of `out`.
fn lift_plan(out: &mut Images, plan: Plan, image_id: [u8; 32]) {
    let fe = Rsp;
    let Plan {
        source,
        offset: start,
        bytes,
        known,
        cut,
    } = plan;
    let len = bytes.len() as u32;
    let k = out.images.len();
    let base = 0u64;
    let end = u64::from(len);
    let words = known.len();
    let whole = len == IMEM_SIZE;
    let is_cut = |t: u64| {
        let i = ((t & 0xFFF) / 4) as usize;
        t < end && cut.get(i).copied().unwrap_or(false) || whole && t == end && cut[0]
    };
    let mut blocks: Blocks = BTreeMap::new();
    let mut raws: BTreeMap<u64, u64> = BTreeMap::new();
    for i in (0..words).filter(|i| known[*i]) {
        let off = 4 * i;
        let pc = base + off as u64;
        let w = be32(&bytes, off);
        let slot = match (i + 1 < words, whole) {
            (true, _) => be32(&bytes, off + 4),
            (false, true) => be32(&bytes, 0),
            (false, false) => 0,
        };
        let mut r = fe.lift_at(pc, w, slot);
        match &mut r {
            Err(e) => out.unlifted.push(Unlifted {
                image: k,
                pc,
                failure: e.clone(),
            }),
            Ok(b) => cut_exit(b, is_cut),
        }
        raws.insert(pc, u64::from(w) | (u64::from(slot) << 32));
        blocks.insert(pc, r);
    }
    out.instructions += blocks.len();
    let mut succ = Vec::with_capacity(3 * words);
    for i in 0..words {
        match blocks.get(&(4 * i as u64)) {
            Some(Ok(b)) => succ.extend(succ_of(b, base, end)),
            _ => succ.extend([NO_SUCC, NO_SUCC, 0]),
        }
    }
    let mut covered: BTreeSet<u64> = BTreeSet::new();
    let mut fns: BTreeMap<u64, BTreeSet<u64>> = BTreeMap::new();
    let pcs: Vec<u64> = blocks.keys().copied().collect();
    for pc in pcs {
        if covered.contains(&pc) {
            continue;
        }
        let r = reach(&blocks, pc, &covered);
        covered.extend(&r);
        fns.insert(pc, r);
    }
    let functions: Vec<Function<'_>> = fns
        .iter()
        .map(|(&entry, pcs)| {
            let last = pcs
                .iter()
                .copied()
                .filter(|p| *p >= entry)
                .max()
                .unwrap_or(entry);
            Function {
                entry,
                name: format!("n64_rsp{k}_{entry:03x}"),
                guest_name: None,
                span: last - entry + 4,
                insts: pcs
                    .iter()
                    .map(|pc| {
                        let i = match &blocks[pc] {
                            Ok(block) => Insn::Lifted {
                                block,
                                raw: raws[pc],
                            },
                            Err(e) => Insn::Failed(e),
                        };
                        (*pc, i)
                    })
                    .collect(),
                tables: BTreeMap::new(),
                guesses: BTreeMap::new(),
                entries: BTreeMap::new(),
                pads: BTreeSet::new(),
            }
        })
        .collect();
    let symbol = unit_symbol(k);
    let hash = fnv(&bytes);
    let opts = Options {
        unit_name: format!("rsp{k}"),
        unit_symbol: symbol.clone(),
        module_base: base,
        io_map: IoMap::default(),
        image_id,
        enter_anywhere: Some(u64::MAX),
        entry_ranges: true,
        ..Options::default()
    };
    let output = relift_emit::emit(&fe, &functions, &[], &opts);
    for (pc, insn) in undefined_in(&output) {
        if !out.unlifted.iter().any(|u| u.image == k && u.pc == pc) {
            out.unlifted.push(Unlifted {
                image: k,
                pc,
                failure: LiftFailure {
                    insn: u64::from(insn),
                    message: "the emitter has no C form of it".into(),
                },
            });
        }
    }
    let nfns = functions.len();
    drop(functions);
    out.images.push(Image {
        source,
        offset: start,
        hash,
        symbol,
        dir: format!("rsp{k}"),
        output,
        functions: nfns,
        bytes,
        succ,
    });
}

/// `rsp_images.c`: the `rl_rsp_image` of every image and the list the program
/// gives the runtime (`rl_n64_rsp_images`, `rl_n64_rsp_count`).
#[must_use]
pub fn images_c(images: &[Image]) -> String {
    let mut s = String::from("#include <stddef.h>\n#include <stdint.h>\n#include \"rsp.h\"\n");
    for (k, im) in images.iter().enumerate() {
        let _ = writeln!(s, "extern const rl_unit {};", im.symbol);
        let _ = write!(s, "static const uint8_t bytes{k}[] = {{");
        for (i, b) in im.bytes.iter().enumerate() {
            if i % 16 == 0 {
                s.push_str("\n   ");
            }
            let _ = write!(s, " {b:#04x},");
        }
        let _ = write!(s, "\n}};\nstatic const uint16_t succ{k}[] = {{");
        for (i, v) in im.succ.iter().enumerate() {
            if i % 12 == 0 {
                s.push_str("\n   ");
            }
            let _ = write!(s, " {v:#x},");
        }
        let _ = writeln!(
            s,
            "\n}};\nstatic const rl_rsp_image {} = {{ &{}, {:#x}u, UINT64_C({:#x}), bytes{k}, succ{k} }};",
            image_symbol(k),
            im.symbol,
            im.bytes.len(),
            im.hash
        );
    }
    let list: Vec<String> = images
        .iter()
        .enumerate()
        .map(|(k, _)| format!("&{}", image_symbol(k)))
        .collect();
    let _ = writeln!(
        s,
        "extern const rl_rsp_image *const rl_n64_rsp_images[];\nconst rl_rsp_image *const rl_n64_rsp_images[] = {{ {} }};\nextern const uint32_t rl_n64_rsp_count;\nconst uint32_t rl_n64_rsp_count = {}u;",
        if list.is_empty() {
            "NULL".into()
        } else {
            list.join(", ")
        },
        images.len()
    );
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ops(b: &Block) -> Vec<&Op> {
        b.insts.iter().map(|i| &i.op).collect()
    }

    #[test]
    fn every_block_ticks_and_ends_in_a_sync_point() {
        let add = 0x0022_1820;
        let b = Rsp.lift_at(0x100, add, 0).expect("lifts");
        b.verify().expect("verifies");
        assert!(matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)));
        assert!(ops(&b).iter().any(|o| matches!(o, Op::Tick(1))));
        let beq = 0x1000_0004;
        let b = Rsp.lift_at(0x100, beq, add).expect("lifts");
        b.verify().expect("verifies");
        assert_eq!(b.len, 8);
        assert!(ops(&b).iter().any(|o| matches!(o, Op::Tick(2))));
        assert_eq!(
            Rsp.sync_call(&b, "n").as_deref(),
            Some("rl_rsp_sync(c, mem, n)")
        );
    }

    #[test]
    fn successors_stay_inside_the_image_and_mark_delay_slots() {
        let b_ = 0x1000_0013;
        let b = Rsp.lift_at(0x100, b_, 0).expect("lifts");
        assert_eq!(succ_of(&b, 0x100, 0x200), [0x54, 0x42, SUCC_SLOT]);
        assert_eq!(succ_of(&b, 0x180, 0x200), [NO_SUCC, NO_SUCC, SUCC_SLOT]);
        let j = 0x0800_0050;
        let b = Rsp.lift_at(0x100, j, 0).expect("lifts");
        assert_eq!(succ_of(&b, 0, 0x1000), [NO_SUCC, NO_SUCC, SUCC_SLOT]);
        let jr = 0x03E0_0008;
        let b = Rsp.lift_at(0x100, jr, 0).expect("lifts");
        assert_eq!(succ_of(&b, 0, 0x1000), [NO_SUCC, NO_SUCC, SUCC_SLOT]);
        let add = 0x0022_1820;
        let b = Rsp.lift_at(0xFFC, add, 0).expect("lifts");
        assert_eq!(succ_of(&b, 0, 0x1000), [0, NO_SUCC, 0], "IMEM wraps");
    }

    #[test]
    fn cop0_and_break_call_the_layer() {
        assert_eq!(
            Rsp.intrinsic("rsp_mtc0", &["a".into(), "b".into(), "r".into()])
                .as_deref(),
            Some("rl_rsp_mtc0(c, (uint32_t)(a), (uint32_t)(b), (uint32_t)(r))")
        );
        assert_eq!(
            Rsp.intrinsic("rsp_break", &["r".into()]).as_deref(),
            Some("rl_rsp_break(c, (uint32_t)(r))")
        );
        let brk = 0x0000_000D;
        let b = Rsp.lift_at(0, brk, 0).expect("lifts");
        b.verify().expect("verifies");
        assert_eq!(succ_of(&b, 0, 0x1000), [NO_SUCC, NO_SUCC, 0], "BREAK halts");
    }

    fn intrinsic_count(b: &Block, name: &str) -> usize {
        b.insts
            .iter()
            .filter(|i| matches!(&i.op, Op::Intrinsic(c) if c.name == name))
            .count()
    }

    #[test]
    fn absolute_targets_and_links_move_with_the_base() {
        let jal = 0x0C00_0050;
        let b = Rsp.lift_at(0x100, jal, 0x0022_1820).expect("lifts");
        b.verify().expect("verifies");
        assert!(
            matches!(b.exit.next, Next::Dynamic(_)),
            "J targets are addresses"
        );
        assert_eq!(intrinsic_count(&b, BASE), 2);
        let link = b
            .insts
            .iter()
            .find_map(|i| match i.op {
                Op::Set(r, v) if r == regs::gpr(31) => Some(v),
                _ => None,
            })
            .expect("the link");
        assert!(matches!(b.insts[link.index()].op, Op::Bin(BinOp::And, ..)));
        let bgezal = 0x0411_0004;
        let b = Rsp.lift_at(0x100, bgezal, 0).expect("lifts");
        b.verify().expect("verifies");
        assert!(matches!(b.exit.next, Next::Select { taken: 0x114, .. }));
        assert_eq!(intrinsic_count(&b, BASE), 1);
        let jr = 0x03E0_0008;
        let b = Rsp.lift_at(0x100, jr, 0).expect("lifts");
        b.verify().expect("verifies");
        assert_eq!(intrinsic_count(&b, BASE), 1);
        let beq = 0x1000_0004;
        let b = Rsp.lift_at(0x100, beq, 0).expect("lifts");
        assert_eq!(intrinsic_count(&b, BASE), 0, "branches are relative");
        assert_eq!(Rsp.intrinsic(BASE, &[]).as_deref(), Some("rl_rsp_base(c)"));
    }

    /// Whether `v` of `b` is computed from the running image's base.
    fn uses_base(b: &Block, v: Value) -> bool {
        let op = &b.insts[v.index()].op;
        if matches!(op, Op::Intrinsic(c) if c.name == BASE) {
            return true;
        }
        let ins = std::cell::RefCell::new(Vec::new());
        let _ = op.clone().map_values(&|x: &Value| {
            ins.borrow_mut().push(*x);
            *x
        });
        ins.into_inner().into_iter().any(|x| uses_base(b, x))
    }

    #[test]
    fn a_delay_slot_reads_the_link_as_an_address() {
        let jal = 0x0C00_07ED;
        let ori_r12_r31 = 0x37EC_0000;
        let b = Rsp.lift_at(0x170, jal, ori_r12_r31).expect("lifts");
        b.verify().expect("verifies");
        let r12 = b
            .insts
            .iter()
            .find_map(|i| match i.op {
                Op::Set(r, v) if r == regs::gpr(12) => Some(v),
                _ => None,
            })
            .expect("the delay slot's write");
        assert!(uses_base(&b, r12));
    }

    #[test]
    fn images_merge_by_source_and_cut_into_overlapping_pieces() {
        let im = |offset: u32, len: u32, source: Source| RspImage {
            source,
            offset,
            len,
            imem: None,
            hash: 0,
            origin: relift_n64_analyze::RspOrigin::Scan,
        };
        let p = pieces(&[
            im(0x1000, 0x100, Source::Rom),
            im(0x1080, 0x100, Source::Rom),
            im(0x8000, 0x1800, Source::Rom),
            im(0x10, 0x20, Source::Stream(2)),
        ]);
        assert_eq!(
            p,
            vec![
                (Source::Rom, 0x1000, 0x180),
                (Source::Rom, 0x8000, 0x1000),
                (Source::Rom, 0x8FF8, 0x808),
                (Source::Stream(2), 0x10, 0x20),
            ]
        );
    }

    #[test]
    fn a_branch_into_imem_a_dma_may_overwrite_goes_through_dispatch() {
        let beq = 0x1000_0004;
        let add = 0x0022_1820;
        let mut b = Rsp.lift_at(0x100, beq, add).expect("lifts");
        cut_exit(&mut b, |t| {
            (0x100..0x200).contains(&(t & 0xFFF)) && t != 0x108
        });
        b.verify().expect("verifies");
        assert!(matches!(b.exit.next, Next::Dynamic(_)));
        assert!(matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)));
        assert_eq!(succ_of(&b, 0, 0x1000), [NO_SUCC, NO_SUCC, SUCC_SLOT]);
        let mut b = Rsp.lift_at(0x100, add, 0).expect("lifts");
        cut_exit(&mut b, |t| t == 0x200);
        assert_eq!(succ_of(&b, 0, 0x1000), [0x41, NO_SUCC, 0]);
        cut_exit(&mut b, |t| t == 0x104);
        assert!(matches!(b.exit.next, Next::Dynamic(_)));
        assert_eq!(succ_of(&b, 0, 0x1000), [NO_SUCC, NO_SUCC, 0]);
    }

    #[test]
    fn the_divide_unit_instructions_have_c_forms() {
        let words = [
            0x4A01_0030u32,
            0x4A01_0031,
            0x4A01_0034,
            0x4A01_0035,
            0x4A01_0036,
        ];
        let blocks: Vec<Block> = words
            .iter()
            .enumerate()
            .map(|(i, w)| Rsp.lift_at(4 * i as u64, *w, 0).expect("lifts"))
            .collect();
        let f = Function {
            entry: 0,
            name: "f".into(),
            guest_name: None,
            span: 4 * words.len() as u64,
            insts: blocks
                .iter()
                .enumerate()
                .map(|(i, block)| {
                    (
                        4 * i as u64,
                        Insn::Lifted {
                            block,
                            raw: u64::from(words[i]),
                        },
                    )
                })
                .collect(),
            tables: BTreeMap::new(),
            guesses: BTreeMap::new(),
            entries: BTreeMap::new(),
            pads: BTreeSet::new(),
        };
        let opts = Options {
            enter_anywhere: Some(u64::MAX),
            ..Options::default()
        };
        let out = relift_emit::emit(&Rsp, &[f], &[], &opts);
        assert_eq!(out.undefined, 0);
        assert!(undefined_in(&out).is_empty());
        let text: String = out.files.iter().map(|f| f.text.as_str()).collect();
        assert!(
            text.contains("rl_rsp_fsqrt64("),
            "VRSQ reads the ROM formula"
        );
    }

    #[test]
    fn undefined_calls_are_found_in_the_c() {
        let f = |t: &str| Output {
            files: vec![relift_emit::File {
                name: "s.c".into(),
                text: t.into(),
                fns: Vec::new(),
            }],
            undefined: 1,
        };
        let out = f("x;\n    return rl_rsp_undefined(c, mem, UINT64_C(0xc4), 0x4a010034u);\n}");
        assert_eq!(undefined_in(&out), vec![(0xC4, 0x4A01_0034)]);
        assert!(undefined_in(&f("rl_rsp_undefined(c, mem, c->pc, 0x2u)")).is_empty());
    }

    #[test]
    fn the_fields_follow_the_c_context() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../runtime/cpus/rsp/include/relift/rsp/abi.h"
        );
        let text = std::fs::read_to_string(path).expect("reads abi.h");
        let start = text.find("#define RL_RSP_SLOTS(X)").expect("the slot list");
        let mut n = 0;
        for line in text[start..].lines().skip(1) {
            let line = line.trim().trim_end_matches('\\').trim();
            let Some(body) = line.strip_prefix("X(").and_then(|l| l.strip_suffix(')')) else {
                break;
            };
            let parts: Vec<&str> = body.split(',').map(str::trim).collect();
            assert_eq!(field(Reg(n)), parts[1], "slot {n}");
            n += 1;
        }
        assert_eq!(usize::from(n), regs::regs().len());
    }
}
