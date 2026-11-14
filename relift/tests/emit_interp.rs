//! The emitted C means what the IR means: random words of every form in
//! `cpus/a64/gen/forms.txt` are lifted, emitted as one-instruction
//! functions, compiled with clang at the lifted-code flags and run on random
//! states, and every register, the flags, guest memory, the exclusive
//! monitor and the exit target must match the reference interpreter on the
//! same states. Skipped with a note when clang is not on `PATH`.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_cpu_a64::decode::{Encoding, Features, decode};
use relift_cpu_a64::frontend::A64;
use relift_emit::{Function, Insn, Options, emit};
use relift_guest::{Frontend, Home};
use relift_ir::interp::{FlatMemory, Monitor, NoIntrinsics, State, run};
use relift_ir::{Block, Flow, Next, Op};

/// The guest address of the memory window.
const BASE: u64 = 0x10_0000;
/// Its size in bytes.
const WINDOW: usize = 0x4000;
/// Random words per form.
const WORDS: usize = 2;
/// Random states per word.
const STATES: usize = 12;
/// `u64`s of the context block in the harness records.
const CTX: usize = 105;
/// Bytes of C per emitted shard.
const SHARD: usize = 256 << 10;

/// A small xorshift generator.
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

    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// The window contents for `seed`; the harness computes the same bytes.
fn fill(seed: u64) -> Vec<u8> {
    let mut r = Rng(seed | 1);
    (0..WINDOW).map(|_| (r.next() >> 56) as u8).collect()
}

fn fnv(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| {
        (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3)
    })
}

/// Where a register-file slot sits in the harness context block.
#[derive(Clone, Copy, Debug)]
enum Slot {
    /// A 64-bit word at this index.
    Word(usize),
    /// A 32-bit word at this index.
    Word32(usize),
    /// A 128-bit register: the low half at this index, the high half next.
    Wide(usize),
    /// Bit `n` of the packed NZCV word.
    Flag(u32),
    /// A constant zero (the FPSR-flags switch, off in this build).
    Zero,
}

const NZCV: usize = 96;
const EXCL: usize = 101;

fn slots() -> Vec<Slot> {
    let fe = A64 {
        features: Features::ALL,
    };
    let regs = fe.regs();
    (0..regs.len())
        .map(|i| {
            let r = relift_ir::Reg(i as u16);
            match fe.home(r) {
                Home::Flag { bit, .. } => Slot::Flag(bit),
                Home::Field(f) => {
                    let idx = |s: &str| -> usize {
                        s.split(['[', ']'])
                            .nth(1)
                            .and_then(|n| n.parse().ok())
                            .expect("an indexed home")
                    };
                    match f.as_str() {
                        "c->sp" => Slot::Word(31),
                        "c->fpcr" => Slot::Word32(97),
                        "c->fpsr" => Slot::Word32(98),
                        "c->tpidr_el0" => Slot::Word(99),
                        "c->tpidrro_el0" => Slot::Word(100),
                        "RL_FP_FLAGS_ON" => Slot::Zero,
                        s if s.starts_with("c->x[") => Slot::Word(idx(s)),
                        s if s.starts_with("c->v[") => Slot::Wide(32 + 2 * idx(s)),
                        s => panic!("a home the harness does not know: {s}"),
                    }
                }
            }
        })
        .collect()
}

/// A random register value: mostly small, special or window addresses.
fn value(r: &mut Rng) -> u64 {
    const SPECIAL: [u64; 10] = [
        0,
        1,
        2,
        0x7f,
        0x80,
        0xffff_ffff,
        0x8000_0000,
        0x7fff_ffff_ffff_ffff,
        0x8000_0000_0000_0000,
        u64::MAX,
    ];
    match r.below(5) {
        0 => SPECIAL[r.below(SPECIAL.len() as u64) as usize],
        1 => r.below(64),
        2 => BASE + 0x1000 + (r.below(0x2000) & !7),
        _ => r.next(),
    }
}

/// A random 128-bit register: random bits or floating-point specials.
fn vector(r: &mut Rng) -> u128 {
    const FP: [u64; 10] = [
        0,
        0x3ff0_0000_0000_0000,
        0xbff0_0000_0000_0000,
        0x7ff0_0000_0000_0000,
        0x7ff8_0000_0000_0000,
        0x7ff4_0000_0000_0000,
        0x0000_0000_0000_0001,
        0x3f80_0000_3f80_0000,
        0x7fc0_0000_ff80_0000,
        0x0000_0001_8000_0000,
    ];
    let half = |r: &mut Rng| {
        if r.below(3) == 0 {
            FP[r.below(FP.len() as u64) as usize]
        } else {
            r.next()
        }
    };
    (u128::from(half(r)) << 64) | u128::from(half(r))
}

/// A random state for `word`: its base register points into the window
/// most of the time, FPCR and FPSR are zero, and the monitor is armed at
/// the base register's address half the time.
fn state(word: u32, slots: &[Slot], r: &mut Rng) -> State {
    let mut s = State::new(slots.len());
    for (i, slot) in slots.iter().enumerate() {
        s.regs[i] = match slot {
            Slot::Word(31) => u128::from(BASE + 0x2000 + (r.below(0x800) & !15)),
            Slot::Word(_) => u128::from(value(r)),
            Slot::Wide(_) => vector(r),
            Slot::Flag(_) => u128::from(r.below(2) as u8),
            Slot::Word32(_) | Slot::Zero => 0,
        };
    }
    let rn = ((word >> 5) & 31) as usize;
    if r.below(5) != 0 {
        s.regs[rn] = u128::from(BASE + 0x1000 + (r.below(0x2000) & !15));
    }
    if r.below(2) == 0 {
        let addr = s.regs[rn] as u64;
        let size = [1, 2, 4, 8, 16][r.below(5) as usize];
        s.monitor = Some(Monitor { addr, size });
    }
    s
}

fn pack(s: &State, slots: &[Slot], mem: &[u8], out: &mut Vec<u64>) {
    let mut ctx = [0u64; CTX];
    for (slot, &v) in slots.iter().zip(&s.regs) {
        match *slot {
            Slot::Word(i) | Slot::Word32(i) => ctx[i] = v as u64,
            Slot::Wide(i) => {
                ctx[i] = v as u64;
                ctx[i + 1] = (v >> 64) as u64;
            }
            Slot::Flag(bit) => ctx[NZCV] |= ((v as u64) & 1) << bit,
            Slot::Zero => {}
        }
    }
    if let Some(m) = s.monitor {
        let at = |a: u64| -> u64 {
            let off = a.wrapping_sub(BASE) as usize;
            let mut b = [0u8; 8];
            let n = (m.size as usize).min(8);
            if off.checked_add(n).is_some_and(|e| e <= WINDOW) {
                b[..n].copy_from_slice(&mem[off..off + n]);
            }
            u64::from_le_bytes(b)
        };
        ctx[EXCL] = m.addr;
        ctx[EXCL + 1] = u64::from(m.size);
        ctx[EXCL + 2] = at(m.addr);
        ctx[EXCL + 3] = if m.size == 16 {
            at(m.addr.wrapping_add(8))
        } else {
            0
        };
    }
    out.extend_from_slice(&ctx);
}

/// What the interpreter says one run does.
struct Expect {
    case: usize,
    state: State,
    mem: u64,
    next: Option<u64>,
}

const HARNESS: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <relift/a64/abi.h>
#include <relift/a64/fp_slow.h>

extern const rl_fn cases[];
extern const unsigned case_count;

static uint64_t miss;
static int missed;
static uint64_t events;
static uint8_t buf[WINDOW] __attribute__((aligned(64)));

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem) { (void)mem; if (!missed) { missed = 1; miss = c->pc; } return RL_OK; }
int rl_svc(rl_ctx *c, uint8_t *mem, uint32_t imm) { (void)c; (void)mem; (void)imm; events |= 4; return RL_PENDING; }
int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn) { (void)c; (void)mem; (void)pc; (void)insn; events |= 1; return RL_PENDING; }
int rl_fp_slow(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn) { (void)c; (void)mem; (void)pc; (void)insn; events |= 2; return RL_PENDING; }
uint64_t rl_mrs_cntpct(rl_ctx *c) { (void)c; events |= 8; return 0; }
void rl_msr_fpcr(rl_ctx *c, uint64_t v) { events |= 8; c->fpcr = (uint32_t)v; }
void rl_wfe(rl_ctx *c) { (void)c; events |= 8; }
int rl_a64_fp_flags(void) { return 0; }
_Noreturn void rl_bad_resume(rl_ctx *c, uint8_t *mem, uint64_t fn) { (void)c; (void)mem; (void)fn; abort(); }

static void fill(uint64_t seed)
{
    uint64_t x = seed | 1;
    for (size_t i = 0; i < WINDOW; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        buf[i] = (uint8_t)((x * UINT64_C(0x2545f4914f6cdd1d)) >> 56);
    }
}

static uint64_t fnv(void)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < WINDOW; i++)
        h = (h ^ buf[i]) * UINT64_C(0x100000001b3);
    return h;
}

static void load(rl_ctx *c, const uint64_t *r)
{
    memcpy(c->x, r, 31 * 8);
    c->sp = r[31];
    for (int i = 0; i < 32; i++) {
        memcpy(&c->v[i].b[0], &r[32 + 2 * i], 8);
        memcpy(&c->v[i].b[8], &r[33 + 2 * i], 8);
    }
    c->nzcv = (uint32_t)r[96];
    c->fpcr = (uint32_t)r[97];
    c->fpsr = (uint32_t)r[98];
    c->tpidr_el0 = r[99];
    c->tpidrro_el0 = r[100];
    c->excl.addr = r[101];
    c->excl.size = (uint32_t)r[102];
    c->excl.lo = r[103];
    c->excl.hi = r[104];
}

static void save(const rl_ctx *c, uint64_t *r)
{
    memcpy(r, c->x, 31 * 8);
    r[31] = c->sp;
    for (int i = 0; i < 32; i++) {
        memcpy(&r[32 + 2 * i], &c->v[i].b[0], 8);
        memcpy(&r[33 + 2 * i], &c->v[i].b[8], 8);
    }
    r[96] = c->nzcv;
    r[97] = c->fpcr;
    r[98] = c->fpsr;
    r[99] = c->tpidr_el0;
    r[100] = c->tpidrro_el0;
    r[101] = c->excl.addr;
    r[102] = c->excl.size;
    r[103] = c->excl.lo;
    r[104] = c->excl.hi;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out)
        return 2;
    static rl_ctx ctx;
    uint64_t rec[2 + CTX];
    uint8_t *mem = (uint8_t *)((uintptr_t)buf - (uintptr_t)BASE);
    while (fread(rec, sizeof rec, 1, in) == 1) {
        memset(&ctx, 0, sizeof ctx);
        load(&ctx, rec + 2);
        fill(rec[1]);
        missed = 0;
        miss = 0;
        events = 0;
        if (rec[0] >= case_count)
            return 3;
        uint64_t res[4 + CTX];
        res[0] = (uint64_t)cases[rec[0]](&ctx, mem);
        res[1] = missed ? miss : UINT64_MAX;
        res[2] = events;
        res[3] = fnv();
        save(&ctx, res + 4);
        fwrite(res, sizeof res, 1, out);
    }
    fclose(out);
    return 0;
}
"#;

fn write(path: &Path, bytes: &[u8]) {
    relift_cache::write_if_changed(path, bytes).expect("the scratch file is writable");
}

fn words(bytes: &[u64]) -> Vec<u8> {
    bytes.iter().flat_map(|w| w.to_le_bytes()).collect()
}

fn read_words(path: &Path) -> Vec<u64> {
    std::fs::read(path)
        .expect("the harness wrote its output")
        .as_chunks::<8>()
        .0
        .iter()
        .map(|c| u64::from_le_bytes(*c))
        .collect()
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

/// Whether the block leaves through a constant target equal to its own pc,
/// which the one-instruction function would run forever.
fn loops(b: &Block) -> bool {
    match b.exit.next {
        Next::Const(t) => t == b.pc,
        Next::Select {
            taken, not_taken, ..
        } => taken == b.pc || not_taken == b.pc,
        Next::Dynamic(_) => false,
    }
}

#[test]
fn emitted_c_matches_the_interpreter_on_every_form() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let forms = std::fs::read_to_string(root.join("cpus/a64/gen/forms.txt"))
        .expect("forms.txt is readable");
    let by_name: BTreeMap<&str, Encoding> = Encoding::all().map(|e| (e.name(), e)).collect();
    let fe = A64 {
        features: Features::ALL,
    };
    let mut r = Rng(0x5eed_0e0e);
    // Cases spaced 1 GiB apart so no branch reaches another case.
    let mut cases: Vec<(u64, u32, Block)> = Vec::new();
    for name in forms.lines().map(str::trim).filter(|l| !l.is_empty()) {
        let enc = *by_name
            .get(name)
            .unwrap_or_else(|| panic!("form {name} decodes"));
        let mut got = 0;
        for _ in 0..200 {
            if got == WORDS {
                break;
            }
            let pc = 0x40_0000_0000 + (cases.len() as u64) * 0x4000_0000;
            let w = enc.value() | (r.next() as u32 & !enc.mask());
            if decode(w).map(|i| i.encoding) != Some(enc) {
                continue;
            }
            let Ok(b) = fe.lift(&w.to_le_bytes(), pc) else {
                continue;
            };
            let special = b.insts.iter().any(|i| matches!(i.op, Op::Intrinsic(..)));
            if loops(&b) || special || matches!(b.exit.flow, Flow::Trap(_)) {
                continue;
            }
            cases.push((pc, w, b));
            got += 1;
        }
    }
    let slots = slots();
    let mut input = Vec::new();
    let mut expect = Vec::new();
    for (k, (_, w, b)) in cases.iter().enumerate() {
        for _ in 0..STATES {
            let s0 = state(*w, &slots, &mut r);
            let seed = r.next();
            let bytes = fill(seed);
            let mut mem = FlatMemory {
                base: BASE,
                bytes: bytes.clone(),
            };
            let mut s1 = s0.clone();
            let Ok(o) = run(b, &mut s1, &mut mem, &mut NoIntrinsics) else {
                continue;
            };
            input.push(k as u64);
            input.push(seed);
            pack(&s0, &slots, &bytes, &mut input);
            expect.push(Expect {
                case: k,
                state: s1,
                mem: fnv(&mem.bytes),
                next: (o.flow != Flow::Return).then_some(o.next),
            });
        }
    }
    let fns: Vec<Function<'_>> = cases
        .iter()
        .enumerate()
        .map(|(k, (pc, w, b))| Function {
            entry: *pc,
            name: format!("F_{k}"),
            guest_name: None,
            span: 4,
            insts: BTreeMap::from([(
                *pc,
                Insn::Lifted {
                    block: b,
                    raw: u64::from(*w),
                },
            )]),
            tables: BTreeMap::new(),
        })
        .collect();
    let out = emit(
        &fe,
        &fns,
        &[],
        &Options {
            shard_bytes: SHARD,
            ..Options::default()
        },
    );
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-interp");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    let mut table = String::from("#include <relift/a64/abi.h>\n");
    for f in &fns {
        table.push_str(&format!("RL_LIFTED({});\n", f.name));
    }
    table.push_str("const rl_fn cases[] = {\n");
    for f in &fns {
        table.push_str(&format!("    {},\n", f.name));
    }
    table.push_str(&format!(
        "}};\nconst unsigned case_count = {}u;\n",
        fns.len()
    ));
    let mut sources = vec![dir.join("cases.c"), dir.join("harness.c")];
    write(&sources[0], table.as_bytes());
    write(&sources[1], HARNESS.as_bytes());
    for f in &out.files {
        write(&dir.join(&f.name), f.text.as_bytes());
        if f.name.starts_with("shard") {
            sources.push(dir.join(&f.name));
        }
    }
    let rt = root.join("runtime");
    let flags: Vec<String> = [
        "-std=c11",
        "-O2",
        "-ffp-contract=off",
        "-mcx16",
        "-w",
        "-D_GNU_SOURCE",
        &format!("-DWINDOW={WINDOW}"),
        &format!("-DBASE=UINT64_C({BASE:#x})"),
        &format!("-DCTX={CTX}"),
        &format!("-I{}", rt.join("core/include").display()),
        &format!("-I{}", rt.join("cpus/a64/include").display()),
        &format!("-I{}", rt.join("cpus/a64").display()),
        &format!("-I{}", rt.join("core").display()),
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    sources.push(rt.join("cpus/a64/fp.c"));
    let objs: Vec<PathBuf> = sources
        .iter()
        .enumerate()
        .map(|(i, _)| dir.join(format!("o{i}.o")))
        .collect();
    std::thread::scope(|s| {
        for (src, obj) in sources.iter().zip(&objs) {
            let mut a = flags.clone();
            a.extend([
                "-c".into(),
                src.display().to_string(),
                "-o".into(),
                obj.display().to_string(),
            ]);
            let cc = &cc;
            s.spawn(move || clang(cc, &a));
        }
    });
    let exe = dir.join("harness");
    let mut a: Vec<String> = objs.iter().map(|o| o.display().to_string()).collect();
    a.extend(["-lm".into(), "-o".into(), exe.display().to_string()]);
    clang(&cc, &a);
    write(&dir.join("in.bin"), &words(&input));
    let run = relift_proc::command(&exe)
        .arg(dir.join("in.bin"))
        .arg(dir.join("out.bin"))
        .output()
        .expect("the harness runs");
    assert!(
        run.status.success(),
        "harness: {}",
        String::from_utf8_lossy(&run.stderr)
    );
    let got = read_words(&dir.join("out.bin"));
    assert_eq!(got.len(), expect.len() * (4 + CTX), "one record per state");
    let mut compared = 0;
    let mut skipped = 0;
    let mut forms_seen = std::collections::BTreeSet::new();
    let mut bad = Vec::new();
    for (e, rec) in expect.iter().zip(got.as_chunks::<{ 4 + CTX }>().0) {
        let (pc, w, _) = &cases[e.case];
        if rec[2] != 0 {
            skipped += 1;
            continue;
        }
        compared += 1;
        forms_seen.insert(decode(*w).map(|i| i.encoding.name()));
        let ctx = &rec[4..];
        let mut diffs = Vec::new();
        if rec[0] != 0 {
            diffs.push(format!("returned {}", rec[0]));
        }
        if let Some(n) = e.next
            && rec[1] != n
        {
            diffs.push(format!("next {:#x}, want {n:#x}", rec[1]));
        }
        if rec[3] != e.mem {
            diffs.push("guest memory".into());
        }
        for (i, (slot, &want)) in slots.iter().zip(&e.state.regs).enumerate() {
            let have = match *slot {
                Slot::Word(k) | Slot::Word32(k) => u128::from(ctx[k]),
                Slot::Wide(k) => (u128::from(ctx[k + 1]) << 64) | u128::from(ctx[k]),
                Slot::Flag(bit) => u128::from((ctx[NZCV] >> bit) & 1),
                Slot::Zero => want,
            };
            if have != want {
                let name = fe.regs().decl(relift_ir::Reg(i as u16)).name;
                diffs.push(format!("{name} {have:#x}, want {want:#x}"));
            }
        }
        let monitor = (ctx[EXCL + 1] != 0).then(|| Monitor {
            addr: ctx[EXCL],
            size: ctx[EXCL + 1] as u32,
        });
        if monitor != e.state.monitor {
            diffs.push(format!("monitor {monitor:?}, want {:?}", e.state.monitor));
        }
        if !diffs.is_empty() {
            bad.push(format!(
                "{w:08x} at {pc:#x} (F_{}): {}",
                e.case,
                diffs.join("; ")
            ));
        }
    }
    eprintln!(
        "{} cases, {compared} runs compared over {} forms, {skipped} left for the runtime",
        cases.len(),
        forms_seen.len()
    );
    assert!(
        bad.is_empty(),
        "{} mismatches:\n{}",
        bad.len(),
        bad.join("\n")
    );
    assert!(
        forms_seen.len() * 10 >= forms.lines().count() * 8,
        "only {} forms compared",
        forms_seen.len()
    );
}
