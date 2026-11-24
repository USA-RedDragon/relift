//! The emitted C of the A64 frontend means what the Thor's CPU does: every
//! case of the known-answer files in `cpus/arm64/tests/kat` (recorded from
//! the hardware by `relift-check kat record`) that lifts as straight-line
//! code is emitted as one function through `relift-emit`, compiled with
//! clang at the flagged lifted-code flags (`-DRL_FP_FLAGS
//! -ffp-exception-behavior=strict`) together with the runtime's FP, pointer
//! authentication and crypto code, and run on the case's registers and
//! memory. The registers, flags, FPSR cumulative bits, memory and next
//! address must equal the hardware's. FPCR modes run through the guarded
//! blocks' slow path (`rl_fp_slow`), as in a program.
//!
//! Left out, and counted: cases the hardware faulted on; cases that read or
//! write the program counter (ADR, ADRP, BL, BLR, literal loads), since each
//! function is lifted at an address of its own; and branches, whose exit is
//! not a fall-through. The interpreter replay (`cpus/arm64/tests/kat.rs`)
//! runs those. Skipped with a note when clang is not on `PATH`.
//!
//! `RELIFT_EMIT_A64_KAT_KEEP=<dir>` keeps the sources there.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::{Features, decode_for};
use relift_cpu_arm64::frontend::Arm64;
use relift_cpu_arm64::regs::CHECK_PAC_KEYS;
use relift_emit::{Function, Insn, Options, emit};
use relift_guest::Frontend;
use relift_ir::{Block, Flow};

#[path = "../../cpus/arm64/tests/kat/format.rs"]
mod format;

use format::{Case, DATA, DATA_SIZE, Result as Answer, State};

/// The guest address of the first case; each case gets its own page.
const PC0: u64 = 0x40_0000_0000;
/// Bytes of C per emitted shard.
const SHARD: usize = 256 << 10;
/// `u64`s of an input record: the case, the seed, `x0` to `x30`, SP, `v0`
/// to `v31` as halves, NZCV, FPCR and FPSR.
const IN_WORDS: usize = 2 + 31 + 1 + 64 + 3;
/// `u64`s of an output record before the window: the status and the same
/// registers as the input, then the next address.
const OUT_WORDS: usize = 1 + 31 + 1 + 64 + 3 + 1;

const HARNESS: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <relift/arm64/abi.h>
#include <relift/arm64/fp_slow.h>
#include <relift/arm64/pac.h>
#include "arm64.h"

extern const rl_fn cases[];
extern const unsigned case_count;

static uint8_t buf[WINDOW] __attribute__((aligned(64)));
static int fatal;

uint64_t rl_dispatch_miss(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return RL_OK; }
uint64_t rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem) { (void)c; (void)mem; return RL_OK; }
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
uint64_t rl_arm64_ret_elsewhere(rl_ctx *c, uint64_t to, uint64_t pc) { (void)c; (void)to; (void)pc; fatal = 1; return RL_PENDING; }
int rl_align_fault(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn) { (void)c; (void)mem; (void)pc; (void)insn; fatal = 1; return RL_PENDING; }

void rl_preempt(rl_ctx *c) { c->time.budget = INT64_MAX; }

static void fill(uint8_t *w, uint64_t seed)
{
    uint64_t s = seed;
    for (unsigned i = 0; i < WINDOW; i += 8) {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        memcpy(w + i, &z, 8);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out)
        return 2;
    static const uint64_t keys[10] = KEYS;
    for (int i = 0; i < 10; i++)
        rl_pac_keys[i] = keys[i];
    static rl_ctx c;
    uint64_t rec[IN_WORDS];
    uint8_t *mem = (uint8_t *)((uintptr_t)buf - (uintptr_t)BASE);
    while (fread(rec, sizeof rec, 1, in) == 1) {
        if (rec[0] >= case_count)
            return 3;
        memset(&c, 0, sizeof c);
        fill(buf, rec[1]);
        memcpy(c.x, rec + 2, 31 * 8);
        c.sp = rec[33];
        for (int i = 0; i < 32; i++) {
            memcpy(&c.v[i].b[0], &rec[34 + 2 * i], 8);
            memcpy(&c.v[i].b[8], &rec[35 + 2 * i], 8);
        }
        for (int i = 0; i < 4; i++)
            c.nzcv[i] = (rec[98] >> (3 - i)) & 1;
        c.fpcr = (uint32_t)rec[99];
        c.fpsr = (uint32_t)rec[100];
        fatal = 0;
        rl_arm64_fp_flags_on = 1;
        rl_fp_host_clear();
        uint64_t r = cases[rec[0]](&c, mem);
        uint32_t fpsr = (c.fpsr | rl_fp_host_flags()) & RL_FPSR_MASK;
        rl_fp_host_clear();
        uint64_t st = (fatal || r != RL_OK) ? 1 : 0;
        uint64_t nz = (uint64_t)c.nzcv[0] << 3 | (uint64_t)c.nzcv[1] << 2 | (uint64_t)c.nzcv[2] << 1 | c.nzcv[3];
        uint64_t tail[4] = {nz, c.fpcr, fpsr, 0};
        fwrite(&st, 8, 1, out);
        fwrite(c.x, 8, 31, out);
        fwrite(&c.sp, 8, 1, out);
        for (int i = 0; i < 32; i++)
            fwrite(&c.v[i].b[0], 16, 1, out);
        fwrite(tail, 8, 4, out);
        fwrite(buf, WINDOW, 1, out);
        fflush(out);
    }
    fclose(out);
    return 0;
}
"#;

/// A lifted function: its entry and the blocks of it (address, word, IR).
type Lifted = (u64, Vec<(u64, u32, Block)>);

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

/// Whether the case reads or writes the program counter in a way a lift at
/// another address changes: ADR and ADRP, BL and BLR (the link), and literal
/// loads.
fn pc_sensitive(c: &Case, f: &Features) -> bool {
    let w = *c.words.last().expect("a word");
    decode_for(w, f).is_some_and(|i| {
        let n = i.encoding.name();
        let m = i.encoding.mnemonic();
        matches!(m, "ADR" | "ADRP" | "BL" | "BLR")
            || m.starts_with("BLRA")
            || n.contains("_loadlit")
            || n.contains("_literal")
    })
}

fn is_pac(c: &Case, f: &Features) -> bool {
    let w = *c.words.last().expect("a word");
    decode_for(w, f).is_some_and(|i| {
        let m = i.encoding.mnemonic();
        ["PAC", "AUT", "XPAC", "BRA", "BLRA", "RETA", "LDRA"]
            .iter()
            .any(|p| m.starts_with(p))
    })
}

#[test]
fn emitted_c_matches_the_thor_on_every_known_answer() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let features = Features::parse(format::THOR_FEATURES).expect("the Thor's features");
    let fe = Arm64 {
        cpu: Cpu::new(features, Va::BITS39),
    };
    let dir = root.join("cpus/arm64/tests/kat");
    let mut paths: Vec<_> = std::fs::read_dir(&dir)
        .expect("the kat directory")
        .map(|e| e.expect("entry").path())
        .filter(|p| p.extension().is_some_and(|x| x == "txt") && !p.ends_with("skipped.txt"))
        .collect();
    paths.sort();
    // Lifted functions by the case index, and each case's input.
    let mut fns: Vec<Lifted> = Vec::new();
    let mut cases: Vec<Case> = Vec::new();
    let (mut faulted, mut pc_rel, mut flow, mut unlifted) = (0, 0, 0, 0);
    let mut seeds: Vec<u64> = Vec::new();
    for p in paths {
        let text = std::fs::read_to_string(&p).expect("readable");
        for l in text.lines() {
            let c = format::parse(l);
            assert_eq!(c.src, "thor", "answers come from the Thor");
            if matches!(c.out, Answer::Fault(_)) {
                faulted += 1;
                continue;
            }
            if matches!(&c.out, Answer::Done(o) if o.pc != 4 * c.words.len() as u64) {
                flow += 1;
                continue;
            }
            if pc_sensitive(&c, &features) {
                pc_rel += 1;
                continue;
            }
            let pc = PC0 + (fns.len() as u64) * 0x1000;
            let mut blocks = Vec::new();
            let mut ok = true;
            for (i, w) in c.words.iter().enumerate() {
                let at = pc + 4 * i as u64;
                match fe.lift(&w.to_le_bytes(), at) {
                    Ok(b) if matches!(b.exit.flow, Flow::Jump) => blocks.push((at, *w, b)),
                    Ok(_) => {
                        flow += 1;
                        ok = false;
                        break;
                    }
                    Err(_) => {
                        unlifted += 1;
                        ok = false;
                        break;
                    }
                }
            }
            if !ok {
                continue;
            }
            // The seed: the window the case started from (recorded as the
            // fill the input's memory holds).
            let seed = l
                .split(' ')
                .find_map(|t| t.strip_prefix("seed="))
                .and_then(|s| u64::from_str_radix(s, 16).ok())
                .expect("a seed");
            seeds.push(seed);
            fns.push((pc, blocks));
            cases.push(c);
        }
    }
    assert!(!cases.is_empty());
    let mut input = Vec::new();
    for (k, c) in cases.iter().enumerate() {
        input.extend([k as u64, seeds[k]]);
        input.extend(c.input.x);
        input.push(c.input.sp);
        for v in c.input.v {
            input.extend([v as u64, (v >> 64) as u64]);
        }
        input.extend([
            u64::from(c.input.nzcv),
            u64::from(c.input.fpcr),
            u64::from(c.input.fpsr),
        ]);
    }
    assert_eq!(input.len(), cases.len() * IN_WORDS);
    let functions: Vec<Function<'_>> = fns
        .iter()
        .enumerate()
        .map(|(k, (pc, blocks))| Function {
            entry: *pc,
            name: format!("F_{k}"),
            guest_name: None,
            span: 4 * blocks.len() as u64,
            insts: blocks
                .iter()
                .map(|(at, w, b)| {
                    (
                        *at,
                        Insn::Lifted {
                            block: b,
                            raw: u64::from(*w),
                        },
                    )
                })
                .collect::<BTreeMap<_, _>>(),
            tables: BTreeMap::new(),
            pads: Default::default(),
            guesses: Default::default(),
            entries: Default::default(),
        })
        .collect();
    let out = emit(
        &fe,
        &functions,
        &[],
        &Options {
            shard_bytes: SHARD,
            ..Options::default()
        },
    );
    let work = std::env::var_os("RELIFT_EMIT_A64_KAT_KEEP")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-a64-kat"));
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).expect("the scratch dir is creatable");
    let mut table = String::from("#include <relift/arm64/abi.h>\n");
    for f in &functions {
        table.push_str(&format!("RL_LIFTED({});\n", f.name));
    }
    table.push_str("const rl_fn cases[] = {\n");
    for f in &functions {
        table.push_str(&format!("    {},\n", f.name));
    }
    table.push_str(&format!(
        "}};\nconst unsigned case_count = {}u;\n",
        functions.len()
    ));
    let mut lifted = vec![work.join("cases.c")];
    write(&lifted[0], table.as_bytes());
    write(&work.join("harness.c"), HARNESS.as_bytes());
    for f in &out.files {
        write(&work.join(&f.name), f.text.as_bytes());
        if f.name.starts_with("shard") {
            lifted.push(work.join(&f.name));
        }
    }
    let rt = root.join("runtime");
    let x86 = cfg!(target_arch = "x86_64");
    let keys = CHECK_PAC_KEYS
        .iter()
        .map(|k| format!("UINT64_C({k:#x})"))
        .collect::<Vec<_>>()
        .join(",");
    let common: Vec<String> = [
        "-std=c11",
        "-O2",
        "-ffp-contract=off",
        "-w",
        "-D_GNU_SOURCE",
        &format!("-DWINDOW={DATA_SIZE}"),
        &format!("-DBASE=UINT64_C({DATA:#x})"),
        &format!("-DKEYS={{{keys}}}"),
        &format!("-DIN_WORDS={IN_WORDS}"),
        &format!("-DOUT_WORDS={OUT_WORDS}"),
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
    jobs.push((work.join("harness.c"), &runtime[..]));
    for c in ["fp.c", "fp_slow.c", "fp16.c", "crypto.c", "pac.c"] {
        jobs.push((rt.join("cpus/arm64").join(c), &runtime[..]));
    }
    let objs: Vec<PathBuf> = (0..jobs.len())
        .map(|i| work.join(format!("o{i}.o")))
        .collect();
    let next = std::sync::atomic::AtomicUsize::new(0);
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get().min(16));
    std::thread::scope(|s| {
        for _ in 0..threads.min(jobs.len()) {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some((src, flags)) = jobs.get(i) else {
                        break;
                    };
                    let mut a = flags.to_vec();
                    a.extend([
                        "-c".into(),
                        src.display().to_string(),
                        "-o".into(),
                        objs[i].display().to_string(),
                    ]);
                    clang(&cc, &a);
                }
            });
        }
    });
    let exe = work.join("harness");
    let mut a: Vec<String> = objs.iter().map(|o| o.display().to_string()).collect();
    a.extend([
        "-lm".into(),
        "-lpthread".into(),
        "-o".into(),
        exe.display().to_string(),
    ]);
    clang(&cc, &a);
    let bytes: Vec<u8> = input.iter().flat_map(|w| w.to_le_bytes()).collect();
    write(&work.join("in.bin"), &bytes);
    let run = relift_proc::command(&exe)
        .arg(work.join("in.bin"))
        .arg(work.join("out.bin"))
        .output()
        .expect("the harness runs");
    assert!(
        run.status.success(),
        "harness: {}",
        String::from_utf8_lossy(&run.stderr)
    );
    let res = std::fs::read(work.join("out.bin")).expect("results");
    let rec = OUT_WORDS * 8 + DATA_SIZE;
    assert_eq!(res.len(), cases.len() * rec, "a result per case");
    let word = |r: &[u8], i: usize| u64::from_le_bytes(r[8 * i..8 * i + 8].try_into().expect("8"));
    let mut bad: Vec<String> = Vec::new();
    let mut aborted = 0;
    for (k, c) in cases.iter().enumerate() {
        let r = &res[k * rec..(k + 1) * rec];
        if word(r, 0) != 0 {
            aborted += 1;
            continue;
        }
        let mut s = State::base();
        for i in 0..31 {
            s.x[i] = word(r, 1 + i);
        }
        s.sp = word(r, 32);
        for i in 0..32 {
            s.v[i] = u128::from(word(r, 33 + 2 * i)) | (u128::from(word(r, 34 + 2 * i)) << 64);
        }
        s.nzcv = word(r, 97) as u8;
        s.fpcr = word(r, 98) as u32;
        s.fpsr = word(r, 99) as u32;
        s.pc = 4 * c.words.len() as u64;
        s.mem = r[OUT_WORDS * 8..].to_vec();
        let got = Answer::Done(Box::new(s));
        // Only the cumulative bits the runtime models.
        let mut want = c.clone();
        if let Answer::Done(w) = &mut want.out {
            w.fpsr &= 0x0800_009f;
        }
        let d = if is_pac(c, &features) {
            format::pac_diff(&got, &want)
        } else {
            format::diff(&got, &want, true)
        };
        if !d.is_empty() {
            let w: Vec<String> = c.words.iter().map(|w| format!("{w:08x}")).collect();
            bad.push(format!("{}: {}", w.join(","), d.join("; ")));
        }
    }
    eprintln!(
        "{} cases compared; left out: {faulted} faulted on the Thor (FPAC, alignment, undefined), {pc_rel} pc-relative, {flow} with a branch exit, {unlifted} not lifted; {aborted} stopped in the harness; {} differ",
        cases.len() - aborted,
        bad.len()
    );
    assert!(
        bad.is_empty(),
        "{} cases differ from the Thor, first:\n{}",
        bad.len(),
        bad.iter().take(20).cloned().collect::<Vec<_>>().join("\n")
    );
    assert!(
        aborted * 20 <= cases.len(),
        "{aborted} of {} cases stopped in the harness",
        cases.len()
    );
}
