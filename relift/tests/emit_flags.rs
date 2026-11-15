//! FPSR flags on host hardware match the exact layer: random words of every
//! floating-point form in `cpus/arm64/gen/forms.txt` are lifted as for a
//! program that produces FPSR flags, emitted as one-instruction functions,
//! compiled at the flagged lifted-code flags (`-DRL_FP_FLAGS
//! -ffp-exception-behavior=strict`) and run on states full of FP special
//! values; the result and the cumulative flags (FPSR with the host's sticky
//! flags folded in) must equal what the runtime's exact slow path
//! (`rl_fp_slow`, itself checked against Arm hardware by `relift-check`)
//! gives for the same word and state. Skipped with a note when clang is not
//! on `PATH`.
//!
//! `RELIFT_EMIT_FLAGS_KEEP=<dir>` keeps the sources and the input there,
//! for running the same cases on another host (`harness.c` reports the
//! mismatches itself).

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_cpu_arm64::decode::{Encoding, Features, decode};
use relift_cpu_arm64::frontend::Arm64;
use relift_cpu_arm64::lift::sail::{SLOW_FLAGS, SLOW_FPCR};
use relift_emit::{Function, Insn, Options, emit};
use relift_guest::Frontend;
use relift_ir::{Block, Flow, Op};

/// The guest address of the memory window.
const BASE: u64 = 0x10_0000;
/// Its size in bytes.
const WINDOW: usize = 0x4000;
/// Random words per form.
const WORDS: usize = 4;
/// Random states per word.
const STATES: usize = 48;
/// `u64`s of the register block in the harness records: x0-x30, sp, v0-v31.
const REGS: usize = 96;
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

/// Values that reach every flag in a format of `e` exponent and `f`
/// fraction bits: zeros, ones, twos, infinities, NaNs of both kinds, the
/// smallest normal and the subnormals next to it, values near overflow and
/// near the square root of the smallest normal, so products, quotients and
/// steps land at the boundaries.
fn special(e: u32, f: u32, r: &mut Rng) -> u64 {
    let bias = (1u64 << (e - 1)) - 1;
    let exp = |x: u64| x << f;
    let quiet = 1u64 << (f - 1);
    let fmax = (1u64 << f) - 1;
    let v = match r.below(16) {
        0 => 0,
        1 => exp(bias),
        2 => exp(bias + 1),
        3 => exp(bias - 1),
        4 => exp((1 << e) - 1),
        5 => exp((1 << e) - 1) | quiet | (r.next() & (quiet - 1)),
        6 => exp((1 << e) - 1) | (1 + r.below(quiet - 1)),
        7 => exp(1),
        8 => fmax,
        9 => 1 + r.below(4),
        10 => exp((1 << e) - 2) | fmax,
        11 => exp((1 << e) - 2) | (r.next() & fmax),
        12 => exp(bias / 2 + r.below(3)) | (r.next() & fmax),
        13 => exp(1) | (r.next() & 0xf),
        14 => exp(bias + r.below(70)) | (r.next() & fmax),
        _ => r.next() & ((1u64 << (e + f)) - 1),
    };
    let sign = (r.below(2)) << (e + f);
    v | sign
}

/// A 128-bit register of lanes of one width, each a special value.
fn vector(r: &mut Rng) -> u128 {
    let (w, e, f) = match r.below(4) {
        0 => (16u32, 5u32, 10u32),
        1 | 2 => (32, 8, 23),
        _ => (64, 11, 52),
    };
    let mut v = 0u128;
    for i in 0..128 / w {
        v |= u128::from(special(e, f, r)) << (i * w);
    }
    v
}

/// A general register: small integers, specials of every width, or random.
fn word(r: &mut Rng) -> u64 {
    match r.below(6) {
        0 => r.below(64),
        1 => special(5, 10, r),
        2 => special(8, 23, r),
        3 => special(11, 52, r),
        4 => r.next() >> r.below(64),
        _ => r.next(),
    }
}

const HARNESS: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <relift/arm64/abi.h>
#include <relift/arm64/fp_slow.h>
#include "arm64.h"

extern const rl_fn cases[];
extern const unsigned case_count;

static uint8_t buf[WINDOW] __attribute__((aligned(64)));
static int fatal;

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return RL_OK; }
int rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return RL_OK; }
int rl_svc(rl_ctx *c, uint8_t *mem, uint32_t imm) { (void)c; (void)mem; (void)imm; abort(); }
int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn) { (void)c; (void)mem; (void)pc; (void)insn; fatal = 1; return RL_PENDING; }
int rl_guest_fatal(rl_ctx *c, const char *tag, const char *fmt, ...) { (void)c; (void)tag; (void)fmt; fatal = 1; return RL_PENDING; }
int rl_guest_stop_at(rl_ctx *c, const char *kind, uint64_t pc, const char *fmt, ...) { (void)c; (void)kind; (void)pc; (void)fmt; fatal = 1; return RL_PENDING; }
uint64_t rl_mrs_cntpct(rl_ctx *c) { (void)c; return 0; }
void rl_msr_fpcr(rl_ctx *c, uint64_t v) { c->fpcr = (uint32_t)v; }
void rl_wfe(rl_ctx *c) { (void)c; }
_Noreturn void rl_bad_resume(rl_ctx *c, uint8_t *mem, uint64_t fn) { (void)c; (void)mem; (void)fn; abort(); }
int rl_arm64_fp_flags_on;
int rl_arm64_fp_flags(void) { return rl_arm64_fp_flags_on; }
int rl_arm64_ret_far(rl_ctx *c, uint64_t target, uint64_t pc) { (void)c; (void)target; (void)pc; return RL_OK; }
int rl_arm64_jump_far(rl_ctx *c, uint8_t *mem, rl_fn fn) { return fn(c, mem); }

static void load(rl_ctx *c, const uint64_t *r)
{
    memset(c, 0, sizeof *c);
    memcpy(c->x, r, 31 * 8);
    c->sp = r[31];
    for (int i = 0; i < 32; i++) {
        memcpy(&c->v[i].b[0], &r[32 + 2 * i], 8);
        memcpy(&c->v[i].b[8], &r[33 + 2 * i], 8);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *in = fopen(argv[1], "rb");
    if (!in)
        return 2;
    static rl_ctx a, b;
    uint64_t rec[3 + REGS];
    uint8_t *mem = (uint8_t *)((uintptr_t)buf - (uintptr_t)BASE);
    unsigned long runs = 0, slow = 0, unsupported = 0, bad = 0;
    while (fread(rec, sizeof rec, 1, in) == 1) {
        if (rec[0] >= case_count)
            return 3;
        load(&b, rec + 3);
        fatal = 0;
        rl_arm64_fp_flags_on = 0;
        int rb = rl_fp_slow(&b, mem, rec[1], (uint32_t)rec[2]);
        if (rb != RL_OK || fatal) {
            unsupported++;
            continue;
        }
        load(&a, rec + 3);
        rl_arm64_fp_flags_on = 1;
        rl_fp_host_clear();
        int ra = cases[rec[0]](&a, mem);
        uint32_t fa = (a.fpsr | rl_fp_host_flags()) & 0x9Fu;
        rl_fp_host_clear();
        runs++;
        if (fatal || ra != RL_OK) {
            slow++;
            continue;
        }
        int same = fa == (b.fpsr & 0x9Fu) && !memcmp(a.x, b.x, sizeof a.x) && !memcmp(a.v, b.v, sizeof a.v) && a.nzcv == b.nzcv;
        if (!same) {
            bad++;
            int vi = -1;
            for (int i = 0; i < 32 && vi < 0; i++)
                if (memcmp(&a.v[i], &b.v[i], 16))
                    vi = i;
            printf("BAD %08x case %llu fpsr %#x want %#x", (unsigned)rec[2], (unsigned long long)rec[0], fa, b.fpsr & 0x9Fu);
            if (vi >= 0) {
                uint64_t h[2], w[2];
                memcpy(h, &a.v[vi], 16);
                memcpy(w, &b.v[vi], 16);
                printf(" v%d %016llx%016llx want %016llx%016llx", vi, (unsigned long long)h[1], (unsigned long long)h[0], (unsigned long long)w[1], (unsigned long long)w[0]);
            }
            printf(" in");
            for (int i = 0; i < 4; i++)
                printf(" v%d=%016llx%016llx", i, (unsigned long long)rec[3 + 33 + 2 * i], (unsigned long long)rec[3 + 32 + 2 * i]);
            printf(" x0=%llx x1=%llx\n", (unsigned long long)rec[3], (unsigned long long)rec[4]);
        }
    }
    printf("DONE runs %lu slow %lu unsupported %lu bad %lu\n", runs, slow, unsupported, bad);
    return 0;
}
"#;

fn write(path: &Path, bytes: &[u8]) {
    relift_cache::write_if_changed(path, bytes).expect("the scratch file is writable");
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

/// Whether the block has floating point whose flags the host or the IR
/// gives: an FP operation, an FP guard or an IR-raised flag.
fn has_fp(b: &Block) -> bool {
    b.insts.iter().any(|i| match &i.op {
        Op::Fp(..) | Op::Guard(_, SLOW_FPCR | SLOW_FLAGS) => true,
        Op::Intrinsic(c) => c.name == relift_cpu_arm64::lift::system::FP_RAISE,
        _ => false,
    })
}

/// Whether the block has an intrinsic other than the flag ones.
fn other_intrinsic(b: &Block) -> bool {
    b.insts.iter().any(|i| match &i.op {
        Op::Intrinsic(c) => c.name != relift_cpu_arm64::lift::system::FP_RAISE,
        _ => false,
    })
}

#[test]
fn host_flags_match_the_exact_layer_on_every_fp_form() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let forms = std::fs::read_to_string(root.join("cpus/arm64/gen/forms.txt"))
        .expect("forms.txt is readable");
    let by_name: BTreeMap<&str, Encoding> = Encoding::all().map(|e| (e.name(), e)).collect();
    let fe = Arm64 {
        features: Features::ALL,
    };
    let mut r = Rng(0xf1a6_5eed);
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
            if !has_fp(&b) || other_intrinsic(&b) || !matches!(b.exit.flow, Flow::Jump) {
                continue;
            }
            cases.push((pc, w, b));
            got += 1;
        }
    }
    let mut input = Vec::new();
    for (k, (pc, w, _)) in cases.iter().enumerate() {
        for _ in 0..STATES {
            input.extend([k as u64, *pc, u64::from(*w)]);
            for i in 0..31 {
                input.push(if i == ((w >> 5) & 31) as usize {
                    BASE + 0x1000 + (r.below(0x2000) & !15)
                } else {
                    word(&mut r)
                });
            }
            input.push(BASE + 0x2000 + (r.below(0x800) & !15));
            for _ in 0..32 {
                let v = vector(&mut r);
                input.extend([v as u64, (v >> 64) as u64]);
            }
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
            pads: Default::default(),
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
    let dir = std::env::var_os("RELIFT_EMIT_FLAGS_KEEP")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-flags"));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    let mut table = String::from("#include <relift/arm64/abi.h>\n");
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
    let mut lifted = vec![dir.join("cases.c")];
    write(&lifted[0], table.as_bytes());
    write(&dir.join("harness.c"), HARNESS.as_bytes());
    for f in &out.files {
        write(&dir.join(&f.name), f.text.as_bytes());
        if f.name.starts_with("shard") {
            lifted.push(dir.join(&f.name));
        }
    }
    let rt = root.join("runtime");
    let x86 = cfg!(target_arch = "x86_64");
    let common: Vec<String> = [
        "-std=c11",
        "-O2",
        "-ffp-contract=off",
        "-w",
        "-D_GNU_SOURCE",
        &format!("-DWINDOW={WINDOW}"),
        &format!("-DBASE=UINT64_C({BASE:#x})"),
        &format!("-DREGS={REGS}"),
        &format!("-I{}", rt.join("core/include").display()),
        &format!("-I{}", rt.join("cpus/arm64/include").display()),
        &format!("-I{}", rt.join("cpus/arm64").display()),
        &format!("-I{}", rt.join("core").display()),
    ]
    .iter()
    .map(|s| s.to_string())
    .chain(x86.then(|| "-mcx16".to_string()))
    .collect();
    let flagged: Vec<String> = common
        .iter()
        .cloned()
        .chain(["-DRL_FP_FLAGS", "-ffp-exception-behavior=strict"].map(String::from))
        .collect();
    let runtime: Vec<String> = common
        .iter()
        .cloned()
        .chain(["-DRL_BUILDING_RUNTIME".to_string()])
        .collect();
    let mut jobs: Vec<(PathBuf, &[String])> =
        lifted.iter().map(|s| (s.clone(), &flagged[..])).collect();
    jobs.push((dir.join("harness.c"), &runtime[..]));
    for c in ["fp.c", "fp_slow.c", "fp16.c", "crypto.c"] {
        jobs.push((rt.join("cpus/arm64").join(c), &runtime[..]));
    }
    let objs: Vec<PathBuf> = (0..jobs.len())
        .map(|i| dir.join(format!("o{i}.o")))
        .collect();
    std::thread::scope(|s| {
        for ((src, flags), obj) in jobs.iter().zip(&objs) {
            let mut a = flags.to_vec();
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
    let bytes: Vec<u8> = input.iter().flat_map(|w| w.to_le_bytes()).collect();
    write(&dir.join("in.bin"), &bytes);
    let run = relift_proc::command(&exe)
        .arg(dir.join("in.bin"))
        .output()
        .expect("the harness runs");
    let text = String::from_utf8_lossy(&run.stdout);
    assert!(
        run.status.success(),
        "harness: {}",
        String::from_utf8_lossy(&run.stderr)
    );
    let done = text
        .lines()
        .find(|l| l.starts_with("DONE"))
        .expect("the harness reports");
    eprintln!("{} cases; {done}", cases.len());
    let bad: Vec<&str> = text.lines().filter(|l| l.starts_with("BAD")).collect();
    assert!(
        bad.is_empty(),
        "{} mismatches:\n{}",
        bad.len(),
        bad.iter().take(40).copied().collect::<Vec<_>>().join("\n")
    );
    let runs: u64 = done
        .split_whitespace()
        .nth(2)
        .and_then(|n| n.parse().ok())
        .unwrap_or(0);
    assert!(
        runs as usize * 2 >= cases.len() * STATES,
        "only {runs} runs compared"
    );
}
