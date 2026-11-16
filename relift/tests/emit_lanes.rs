//! The emitted C of the lane operations (docs/abi.md section 16.1) means
//! what the IR means. Every lane kind at every lane width it allows and
//! every immediate, byte permutations of one and two sources, and random
//! chains that mix lane operations with 128-bit integer operations are
//! emitted as one-instruction functions, compiled with clang for each
//! vector path of the host (SSE2, SSSE3 and SSE4.1 on x86-64, NEON on
//! arm64), the portable scalar path (`RL_LANE_SCALAR`), without
//! optimization, and with the 128-bit registers kept in the context, and
//! run on random vectors and vectors of edge lanes. Every register must
//! match the reference interpreter. A mutation control swaps two helpers
//! of nearby meaning in the emitted C and must be seen. Each build directory keeps its sources,
//! `in.bin` and the expected `expect.bin`, so the same check runs on
//! another host by compiling there and comparing the output with `cmp`.
//! Skipped with a note when clang is not on `PATH`.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_emit::{Function, Insn, Options, emit};
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::interp::{FlatMemory, NoIntrinsics, State, run};
use relift_ir::{
    BinOp, Block, Builder, FpOp, LaneKind, LaneOp, LaneWidth, Reg, RegDecl, RegFile, Value, Width,
};

static DECLS: [RegDecl; 8] = [
    RegDecl {
        name: "q0",
        width: Width::W128,
    },
    RegDecl {
        name: "q1",
        width: Width::W128,
    },
    RegDecl {
        name: "q2",
        width: Width::W128,
    },
    RegDecl {
        name: "q3",
        width: Width::W128,
    },
    RegDecl {
        name: "s8",
        width: Width::W8,
    },
    RegDecl {
        name: "s16",
        width: Width::W16,
    },
    RegDecl {
        name: "s32",
        width: Width::W32,
    },
    RegDecl {
        name: "s64",
        width: Width::W64,
    },
];

/// The words of one record: the case (or the return code), the four
/// vectors as low and high halves, and the four scalars.
const REC: usize = 13;

fn file() -> RegFile {
    RegFile::new("lanes", &DECLS)
}

/// The scalar register of `lane`'s width.
fn scalar(lane: LaneWidth) -> Reg {
    Reg(match lane {
        LaneWidth::L8 => 4,
        LaneWidth::L16 => 5,
        LaneWidth::L32 => 6,
        LaneWidth::L64 => 7,
    })
}

/// A frontend that only names where its registers live.
struct Lanes;

impl Frontend for Lanes {
    fn granule(&self) -> u64 {
        1
    }
    fn regs(&self) -> RegFile {
        file()
    }
    fn stack_pointer(&self) -> Option<Reg> {
        None
    }
    fn preserved(&self) -> Vec<Reg> {
        Vec::new()
    }
    fn lift(&self, _bytes: &[u8], _pc: u64) -> Result<Block, LiftFailure> {
        Err(LiftFailure {
            insn: 0,
            message: "the test frontend lifts nothing".into(),
        })
    }
    fn abi_headers(&self) -> &'static [&'static str] {
        &["lanesabi.h"]
    }
    fn home(&self, reg: Reg) -> Home {
        let i = reg.index();
        if i < 4 {
            Home::Field(format!("c->q[{i}]"))
        } else {
            Home::Field(format!("c->{}", DECLS[i].name))
        }
    }
    fn intrinsic(&self, _name: &str, _args: &[String]) -> Option<String> {
        None
    }
    fn trap(&self, _code: u32) -> String {
        "RL_PENDING".into()
    }
    fn undefined(&self, _pc: u64, _insn: u64) -> String {
        "RL_PENDING".into()
    }
    fn monitor(&self) -> &'static str {
        "0"
    }
    fn fp(
        &self,
        _op: FpOp,
        _from: Width,
        _to: Width,
        _args: &[String],
        _reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        None
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

const ABI_H: &str = r#"#ifndef LANESABI_H
#define LANESABI_H
#include <relift/abi_core.h>
struct rl_ctx {
    rl_u128 q[4];
    uint8_t s8;
    uint16_t s16;
    uint32_t s32;
    uint64_t s64;
    uint64_t pc;
};
RL_RUNTIME_API int rl_dispatch_miss(rl_ctx *c, uint8_t *mem);
static inline rl_fn rl_indirect(rl_ctx *c, uint64_t target)
{
    c->pc = target;
    return rl_dispatch_miss;
}
#endif
"#;

const HARNESS: &str = r#"
#include <stdio.h>
#include <lanesabi.h>
extern const rl_fn cases[];
extern const unsigned case_count;
int rl_dispatch_miss(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return RL_OK; }
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out)
        return 2;
    static rl_ctx ctx;
    static uint8_t mem[64];
    uint64_t r[13];
    while (fread(r, sizeof r, 1, in) == 1) {
        if (r[0] >= case_count)
            return 3;
        memset(&ctx, 0, sizeof ctx);
        for (int i = 0; i < 4; i++)
            memcpy(&ctx.q[i], &r[1 + 2 * i], 16);
        ctx.s8 = (uint8_t)r[9];
        ctx.s16 = (uint16_t)r[10];
        ctx.s32 = (uint32_t)r[11];
        ctx.s64 = r[12];
        r[0] = (uint64_t)cases[r[0]](&ctx, mem);
        for (int i = 0; i < 4; i++)
            memcpy(&r[1 + 2 * i], &ctx.q[i], 16);
        r[9] = ctx.s8;
        r[10] = ctx.s16;
        r[11] = ctx.s32;
        r[12] = ctx.s64;
        fwrite(r, sizeof r, 1, out);
    }
    fclose(out);
    return 0;
}
"#;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    fn below(&mut self, n: usize) -> usize {
        (self.next() % n.max(1) as u64) as usize
    }

    fn wide(&mut self) -> u128 {
        (u128::from(self.next()) << 64) | u128::from(self.next())
    }
}

const WIDTHS: [LaneWidth; 4] = [
    LaneWidth::L8,
    LaneWidth::L16,
    LaneWidth::L32,
    LaneWidth::L64,
];

const KINDS: [LaneKind; 52] = {
    use LaneKind::*;
    [
        Add,
        Sub,
        AddSatS,
        AddSatU,
        SubSatS,
        SubSatU,
        MulLo,
        MulHiS,
        MulHiU,
        MulHiSU,
        And,
        Or,
        Xor,
        AndNot,
        Not,
        MinS,
        MinU,
        MaxS,
        MaxU,
        Abs,
        Neg,
        Eq,
        Ne,
        Slt,
        Sle,
        Ult,
        Ule,
        ShlImm,
        LShrImm,
        AShrImm,
        Shl,
        LShr,
        AShr,
        Select,
        WidenLoS,
        WidenLoU,
        WidenHiS,
        WidenHiU,
        NarrowTrunc,
        NarrowSS,
        NarrowUU,
        NarrowSU,
        MulWideLoSS,
        MulWideLoUU,
        MulWideLoSU,
        MulWideHiSS,
        MulWideHiUU,
        MulWideHiSU,
        Broadcast,
        Extract,
        Insert,
        Splat,
    ]
};

/// Every immediate `kind` takes at `lane`.
fn imms(kind: LaneKind, lane: LaneWidth) -> Vec<u8> {
    use LaneKind::*;
    match kind {
        ShlImm | LShrImm | AShrImm => (0..lane.bits() as u8).collect(),
        Broadcast | Extract | Insert => (0..lane.lanes() as u8).collect(),
        _ => vec![0],
    }
}

/// Every operation of every kind, width and immediate.
fn ops() -> Vec<LaneOp> {
    let mut out = Vec::new();
    for kind in KINDS {
        for lane in WIDTHS.into_iter().filter(|&l| kind.allows(l)) {
            for imm in imms(kind, lane) {
                out.push(if kind.has_imm() {
                    LaneOp::with_imm(kind, lane, imm)
                } else {
                    LaneOp::new(kind, lane)
                });
            }
        }
    }
    out
}

/// `op` on q0, q1, q2 (and the scalar of its width), into q3 or, for
/// `Extract`, into the scalar.
fn single(op: LaneOp, pc: u64) -> Block {
    let mut b = Builder::new(file(), Width::W64, pc);
    let args: Vec<Value> = (0..op.kind.arity())
        .map(|i| {
            if op.operand_width(i) == Width::W128 {
                b.get(Reg(i as u16))
            } else {
                b.get(scalar(op.lane))
            }
        })
        .collect();
    let v = b.lane(op, &args);
    let to = if op.kind == LaneKind::Extract {
        scalar(op.lane)
    } else {
        Reg(3)
    };
    b.set(to, v);
    b.jump(1, 0)
}

/// A byte permutation of q0 (and q1) into q3.
fn perm(pattern: [u8; 16], two: bool, pc: u64) -> Block {
    let mut b = Builder::new(file(), Width::W64, pc);
    let x = b.get(Reg(0));
    let v = if two {
        let y = b.get(Reg(1));
        b.permute2(x, y, pattern)
    } else {
        b.permute(x, pattern)
    };
    b.set(Reg(3), v);
    b.jump(1, 0)
}

fn patterns(r: &mut Rng) -> Vec<[u8; 16]> {
    let mut out: Vec<[u8; 16]> = vec![
        std::array::from_fn(|i| i as u8),
        std::array::from_fn(|i| 15 - i as u8),
        std::array::from_fn(|i| 16 + i as u8),
        std::array::from_fn(|i| 31 - i as u8),
        std::array::from_fn(|i| (2 * i) as u8),
        std::array::from_fn(|i| if i % 2 == 0 { i as u8 } else { 16 + i as u8 }),
        [0xff; 16],
        [5; 16],
        [17; 16],
        std::array::from_fn(|i| if i < 8 { i as u8 } else { 0x80 }),
    ];
    for k in 0..30 {
        let top = [32, 48, 256][k % 3];
        out.push(std::array::from_fn(|_| r.below(top) as u8));
    }
    out
}

/// A random chain of lane operations, permutations and 128-bit integer
/// operations over q0 to q2, a constant and the scalars, writing q0, q3
/// and s64.
fn chain(r: &mut Rng, all: &[LaneOp], pc: u64) -> Block {
    let mut b = Builder::new(file(), Width::W64, pc);
    let mut vecs: Vec<Value> = (0..3).map(|i| b.get(Reg(i))).collect();
    let k = r.wide();
    vecs.push(b.konst(Width::W128, k));
    let mut last = Vec::new();
    for _ in 0..2 + r.below(5) {
        let pick = |r: &mut Rng, vecs: &[Value]| vecs[r.below(vecs.len())];
        let v = match r.below(8) {
            0 => {
                let (x, y) = (pick(r, &vecs), pick(r, &vecs));
                let op = [BinOp::Add, BinOp::Xor, BinOp::And, BinOp::Mul][r.below(4)];
                b.bin(op, x, y)
            }
            1 => {
                let x = pick(r, &vecs);
                let lo = b.extract(x, 64 * r.below(2) as u32, Width::W64);
                let hi = b.extract(x, 8 * r.below(9) as u32, Width::W64);
                b.concat(lo, hi)
            }
            2 => {
                let pattern = std::array::from_fn(|_| r.below(40) as u8);
                let x = pick(r, &vecs);
                if r.below(2) == 0 {
                    b.permute(x, pattern)
                } else {
                    let y = pick(r, &vecs);
                    b.permute2(x, y, pattern)
                }
            }
            _ => {
                let op = all[r.below(all.len())];
                let args: Vec<Value> = (0..op.kind.arity())
                    .map(|i| {
                        let w = op.operand_width(i);
                        if w == Width::W128 {
                            pick(r, &vecs)
                        } else if r.below(2) == 0 {
                            b.get(scalar(op.lane))
                        } else {
                            let x = pick(r, &vecs);
                            let lo = r.below(129 - w.bits() as usize) as u32;
                            b.extract(x, lo, w)
                        }
                    })
                    .collect();
                let v = b.lane(op, &args);
                if op.kind == LaneKind::Extract {
                    let e = if op.lane == LaneWidth::L64 {
                        v
                    } else {
                        b.zext(v, Width::W64)
                    };
                    last.push(e);
                    continue;
                }
                v
            }
        };
        vecs.push(v);
    }
    let n = vecs.len();
    b.set(Reg(3), vecs[n - 1]);
    b.set(Reg(0), vecs[n - 2]);
    if let Some(&e) = last.last() {
        b.set(Reg(7), e);
    } else {
        let e = b.extract(vecs[n - 1], 8 * r.below(9) as u32, Width::W64);
        b.set(Reg(7), e);
    }
    b.jump(1, 0)
}

/// A lane of `bits` bits that is likely an edge: 0, 1, the signed limits,
/// all ones, shift amounts around the width, or random.
fn edge_lane(r: &mut Rng, bits: u32) -> u128 {
    let m = if bits == 128 {
        u128::MAX
    } else {
        (1u128 << bits) - 1
    };
    let top = 1u128 << (bits - 1);
    let pool = [
        0,
        1,
        2,
        top - 1,
        top,
        top + 1,
        m,
        m - 1,
        u128::from(bits) - 1,
        u128::from(bits),
        u128::from(bits) + 1,
        1 << (bits / 2),
        (1 << (bits / 2)) - 1,
        0x80,
        0x7f,
    ];
    match r.below(pool.len() + 3) {
        i if i < pool.len() => pool[i] & m,
        _ => r.wide() & m,
    }
}

fn edge(r: &mut Rng, lane: LaneWidth) -> u128 {
    let bits = lane.bits();
    (0..lane.lanes()).fold(0, |v, i| v | (edge_lane(r, bits) << (i * bits)))
}

/// Random states for a case on `lane`-bit lanes: a few of random bits,
/// the rest of edge lanes.
fn states(r: &mut Rng, lane: LaneWidth, n: usize) -> Vec<State> {
    (0..n)
        .map(|k| {
            let mut s = State::new(DECLS.len());
            for i in 0..4 {
                s.regs[i] = if k < 4 { r.wide() } else { edge(r, lane) };
            }
            for (i, w) in [8, 16, 32, 64].into_iter().enumerate() {
                let m = (1u128 << w) - 1;
                s.regs[4 + i] = if k < 4 { r.wide() & m } else { edge_lane(r, w) };
            }
            s
        })
        .collect()
}

fn record(head: u64, s: &State, out: &mut Vec<u64>) {
    out.push(head);
    for i in 0..4 {
        out.push(s.regs[i] as u64);
        out.push((s.regs[i] >> 64) as u64);
    }
    out.extend(s.regs[4..].iter().map(|&v| v as u64));
}

fn write(path: &Path, bytes: &[u8]) {
    relift_cache::write_if_changed(path, bytes).expect("the scratch file is writable");
}

fn bytes(words: &[u64]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

fn clang(cc: &Path, args: &[String]) {
    let out = relift_proc::command(cc)
        .args(args)
        .output()
        .expect("clang runs");
    assert!(
        out.status.success(),
        "clang {}:\n{}",
        args.join(" "),
        String::from_utf8_lossy(&out.stderr)
    );
}

/// Emits `blocks` (registers in the context with `in_context`), replaces
/// each `(from, to)` of `swaps` in the C, builds it with `flags` in `dir`
/// and returns the program.
fn build(
    cc: &Path,
    blocks: &[Block],
    in_context: bool,
    flags: &[&str],
    swaps: &[(&str, &str)],
    dir: &Path,
) -> PathBuf {
    let _ = std::fs::remove_dir_all(dir);
    std::fs::create_dir_all(dir.join("inc")).expect("the scratch dir is creatable");
    write(&dir.join("inc/lanesabi.h"), ABI_H.as_bytes());
    let fns: Vec<Function<'_>> = blocks
        .iter()
        .enumerate()
        .map(|(k, b)| Function {
            entry: b.pc,
            name: format!("F_{k}"),
            guest_name: None,
            span: 1,
            insts: BTreeMap::from([(b.pc, Insn::Lifted { block: b, raw: 0 })]),
            tables: BTreeMap::new(),
            pads: Default::default(),
        })
        .collect();
    let out = emit(
        &Lanes,
        &fns,
        &[],
        &Options {
            shard_bytes: 64 << 10,
            regs_in_context: in_context,
            ..Options::default()
        },
    );
    assert_eq!(out.undefined, 0, "every block is expressed");
    let mut cases = String::from("#include <lanesabi.h>\n");
    for f in &fns {
        cases.push_str(&format!("RL_LIFTED({});\n", f.name));
    }
    cases.push_str("const rl_fn cases[] = {\n");
    for f in &fns {
        cases.push_str(&format!("    {},\n", f.name));
    }
    cases.push_str(&format!(
        "}};\nconst unsigned case_count = {}u;\n",
        fns.len()
    ));
    let mut sources = vec![dir.join("cases.c"), dir.join("harness.c")];
    write(&sources[0], cases.as_bytes());
    write(&sources[1], HARNESS.as_bytes());
    for f in &out.files {
        let text = swaps
            .iter()
            .fold(f.text.clone(), |t, (from, to)| t.replace(from, to));
        write(&dir.join(&f.name), text.as_bytes());
        if f.name.starts_with("shard") {
            sources.push(dir.join(&f.name));
        }
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let mut base: Vec<String> = vec!["-std=c11".into(), "-O2".into(), "-w".into()];
    base.extend(flags.iter().map(|f| f.to_string()));
    base.push(format!("-I{}", root.join("runtime/core/include").display()));
    base.push(format!("-I{}", dir.join("inc").display()));
    base.push(format!("-I{}", dir.display()));
    let objs: Vec<PathBuf> = (0..sources.len())
        .map(|i| dir.join(format!("o{i}.o")))
        .collect();
    std::thread::scope(|s| {
        for (src, obj) in sources.iter().zip(&objs) {
            let mut a = base.clone();
            a.extend([
                "-c".into(),
                src.display().to_string(),
                "-o".into(),
                obj.display().to_string(),
            ]);
            s.spawn(move || clang(cc, &a));
        }
    });
    let exe = dir.join("harness");
    let mut a: Vec<String> = objs.iter().map(|o| o.display().to_string()).collect();
    a.extend(["-o".into(), exe.display().to_string()]);
    clang(cc, &a);
    exe
}

/// The builds to check on this host: name, registers in the context, and
/// compiler flags.
fn variants() -> Vec<(&'static str, bool, Vec<&'static str>)> {
    let mut v: Vec<(&'static str, bool, Vec<&'static str>)> = Vec::new();
    if cfg!(target_arch = "x86_64") {
        v.push(("sse2", false, vec!["-mno-ssse3", "-mno-sse4.1"]));
        v.push(("ssse3", false, vec!["-mssse3", "-mno-sse4.1"]));
        v.push(("sse4.1", false, vec!["-msse4.1"]));
    } else {
        v.push(("native", false, Vec::new()));
    }
    v.push(("scalar", false, vec!["-DRL_LANE_SCALAR"]));
    v.push(("O0", false, vec!["-O0"]));
    v.push(("context", true, Vec::new()));
    v.push(("context-scalar", true, vec!["-DRL_LANE_SCALAR"]));
    v
}

#[test]
fn lane_operations_match_the_interpreter() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let mut r = Rng(0x1a9e_5eed);
    let all = ops();
    let mut blocks: Vec<Block> = Vec::new();
    let mut what: Vec<String> = Vec::new();
    let mut lanes: Vec<LaneWidth> = Vec::new();
    let pc = |n: usize| 0x1000 + n as u64;
    for &op in &all {
        blocks.push(single(op, pc(blocks.len())));
        what.push(format!("{:?}.{} imm {}", op.kind, op.lane.bits(), op.imm));
        lanes.push(op.lane);
    }
    for p in patterns(&mut r) {
        for two in [false, true] {
            blocks.push(perm(p, two, pc(blocks.len())));
            what.push(format!("perm{} {p:?}", if two { "2" } else { "" }));
            lanes.push(LaneWidth::L8);
        }
    }
    for _ in 0..150 {
        blocks.push(chain(&mut r, &all, pc(blocks.len())));
        what.push("chain".into());
        lanes.push(WIDTHS[r.below(4)]);
    }
    for b in &blocks {
        b.verify().expect("the test block is valid IR");
    }
    let mut input = Vec::new();
    let mut expect = Vec::new();
    let mut runs = Vec::new();
    for (k, b) in blocks.iter().enumerate() {
        for s0 in states(&mut r, lanes[k], 16) {
            let mut s1 = s0.clone();
            let mut mem = FlatMemory {
                base: 0,
                bytes: vec![0; 64],
            };
            run(b, &mut s1, &mut mem, &mut NoIntrinsics).expect("the interpreter runs");
            record(k as u64, &s0, &mut input);
            record(0, &s1, &mut expect);
            runs.push(k);
        }
    }
    let base = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-lanes");
    let check = |name: &str, in_context: bool, flags: &[&str], swaps: &[(&str, &str)]| {
        let dir = base.join(name);
        let exe = build(&cc, &blocks, in_context, flags, swaps, &dir);
        write(&dir.join("in.bin"), &bytes(&input));
        write(&dir.join("expect.bin"), &bytes(&expect));
        let out = relift_proc::command(&exe)
            .arg(dir.join("in.bin"))
            .arg(dir.join("out.bin"))
            .output()
            .expect("the harness runs");
        assert!(out.status.success(), "{name}: the harness fails: {out:?}");
        let got = std::fs::read(dir.join("out.bin")).expect("the harness wrote its output");
        assert_eq!(got.len(), expect.len() * 8, "{name}: one record per run");
        let got: Vec<u64> = got
            .as_chunks::<8>()
            .0
            .iter()
            .map(|c| u64::from_le_bytes(*c))
            .collect();
        let mut bad = Vec::new();
        for (i, (g, e)) in got
            .as_chunks::<REC>()
            .0
            .iter()
            .zip(expect.as_chunks::<REC>().0)
            .enumerate()
        {
            if g != e {
                let k = runs[i];
                let inp = &input[i * REC..(i + 1) * REC];
                bad.push(format!(
                    "F_{k} {}: in {:x?}\n  got {g:x?}\n  want {e:x?}",
                    what[k], inp
                ));
            }
        }
        bad
    };
    let mut report = Vec::new();
    for (name, in_context, flags) in variants() {
        let bad = check(name, in_context, &flags, &[]);
        assert!(
            bad.is_empty(),
            "{name}: {} of {} runs differ:\n{}",
            bad.len(),
            runs.len(),
            bad[..bad.len().min(20)].join("\n")
        );
        report.push(format!("{name} {}/{}", runs.len(), runs.len()));
    }
    let swaps = [
        ("rl_lane_add_sat_s16(", "rl_lane_add16("),
        ("rl_lane_ashri32(", "rl_lane_lshri32("),
    ];
    let bad = check("control", false, &[], &swaps);
    let seen = |kind: &str| bad.iter().any(|b| b.contains(kind));
    assert!(
        seen("AddSatS.16") && seen("AShrImm.32"),
        "the mutation control is not seen: {} runs differ",
        bad.len()
    );
    eprintln!(
        "{} cases ({} operations), {} runs each: {}; the control differs in {} runs",
        blocks.len(),
        all.len(),
        runs.len(),
        report.join(", "),
        bad.len()
    );
}
