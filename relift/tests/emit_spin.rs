//! A lifted spin-wait on a plain guest load sees another thread's store: the
//! A64 loop `ldr w0, [x1]; cmp w0, #1; b.ne` is emitted, compiled at `-O2`
//! and run while a host thread sets the word after 20 ms, once as A64 emits
//! it and once from a frontend without a preemption clock. The loop holds
//! no call, so it has no preemption point either way (docs/abi.md section
//! 14.1) and only the loop fence (section 9) keeps clang from loading the
//! word once; a loop that never ends fails the test through the harness's
//! alarm. Skipped with a note when clang is not on `PATH` or the host is not
//! POSIX.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_cpu_arm64::decode::Features;
use relift_cpu_arm64::frontend::Arm64;
use relift_emit::{Function, Insn, Options, emit};
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{Block, FpOp, Reg, RegFile, Width};

/// The spin loop and a return, at guest address 0x1000.
const CODE: [u32; 4] = [0xb940_0020, 0x7100_041f, 0x54ff_ffc1, 0xd65f_03c0];

const HARNESS: &str = r#"#include <relift/arm64/abi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

RL_LIFTED(F_spin);
RL_LIFTED(F_plain);

static uint8_t *g_mem;
static int g_preempts;

void rl_preempt(rl_ctx *c)
{
    g_preempts++;
    c->time.budget = INT64_MAX;
}

static void *setter(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 20000000 };
    nanosleep(&ts, NULL);
    __atomic_store_n((uint32_t *)(void *)(g_mem + 0x2000), 1u, __ATOMIC_RELEASE);
    return NULL;
}

static int spin(rl_fn fn, rl_ctx *c)
{
    __atomic_store_n((uint32_t *)(void *)(g_mem + 0x2000), 0u, __ATOMIC_RELEASE);
    c->x[0] = 0;
    c->x[1] = 0x2000;
    c->time.budget = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, setter, NULL) != 0)
        return 2;
    uint64_t rc = fn(c, g_mem);
    pthread_join(t, NULL);
    return rc == RL_OK && c->x[0] == 1 ? 0 : 1;
}

int main(void)
{
    alarm(60);
    g_mem = calloc(1, 0x4000);
    rl_ctx *c = aligned_alloc(64, sizeof(rl_ctx));
    memset(c, 0, sizeof *c);
    int plain = spin(F_plain, c);
    int arm64 = spin(F_spin, c);
    printf("plain %d arm64 %d preempts %d\n", plain, arm64, g_preempts);
    return !plain && !arm64 ? 0 : 1;
}
"#;

/// A64 without a preemption clock, so only the loop fence keeps the load
/// in the loop.
struct Plain(Arm64);

impl Frontend for Plain {
    fn granule(&self) -> u64 {
        self.0.granule()
    }
    fn regs(&self) -> RegFile {
        self.0.regs()
    }
    fn stack_pointer(&self) -> Option<Reg> {
        self.0.stack_pointer()
    }
    fn pad_resume(&self) -> Option<&'static str> {
        self.0.pad_resume()
    }
    fn preserved(&self) -> Vec<Reg> {
        self.0.preserved()
    }
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        self.0.lift(bytes, pc)
    }
    fn abi_headers(&self) -> &'static [&'static str] {
        self.0.abi_headers()
    }
    fn home(&self, reg: Reg) -> Home {
        self.0.home(reg)
    }
    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        self.0.intrinsic(name, args)
    }
    fn trap(&self, code: u32) -> String {
        self.0.trap(code)
    }
    fn undefined(&self, pc: u64, insn: u64) -> String {
        self.0.undefined(pc, insn)
    }
    fn monitor(&self) -> &'static str {
        self.0.monitor()
    }
    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        self.0.fp(op, from, to, args, reg)
    }
    fn fp_state(&self) -> Vec<Reg> {
        self.0.fp_state()
    }
    fn guard_state(&self, code: u32) -> Option<Reg> {
        self.0.guard_state(code)
    }
    fn guard_flag(&self, r: Reg, value: &str) -> String {
        self.0.guard_flag(r, value)
    }
    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        self.0.guard_slow(code, pc, insn)
    }
}

/// The shards of the spin loop emitted by `fe` as `name`, and the headers
/// they include.
fn emitted(fe: &dyn Frontend, name: &str) -> Vec<(String, String)> {
    let blocks: Vec<(u64, Block)> = CODE
        .iter()
        .enumerate()
        .map(|(i, w)| {
            let pc = 0x1000 + 4 * i as u64;
            (pc, fe.lift(&w.to_le_bytes(), pc).expect("the word lifts"))
        })
        .collect();
    let f = Function {
        entry: 0x1000,
        name: name.into(),
        guest_name: None,
        span: 16,
        insts: blocks
            .iter()
            .zip(CODE)
            .map(|((pc, b), w)| {
                (
                    *pc,
                    Insn::Lifted {
                        block: b,
                        raw: u64::from(w),
                    },
                )
            })
            .collect::<BTreeMap<_, _>>(),
        tables: BTreeMap::new(),
        pads: Default::default(),
        guesses: Default::default(),
        entries: Default::default(),
    };
    let opts = Options {
        unit_symbol: format!("rl_unit_{name}"),
        ..Options::default()
    };
    emit(fe, &[f], &[], &opts)
        .files
        .into_iter()
        .filter(|f| f.name.starts_with("shard") || f.name.ends_with(".h"))
        .map(|f| {
            let file = if f.name.starts_with("shard") {
                format!("{name}_{}", f.name)
            } else {
                f.name
            };
            (file, f.text)
        })
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

#[test]
fn a_lifted_spin_wait_sees_another_threads_store() {
    if !cfg!(unix) {
        eprintln!("skipped: the harness needs POSIX threads");
        return;
    }
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let fe = Arm64 {
        cpu: relift_cpu_arm64::cpu::Cpu::new(Features::ALL, relift_cpu_arm64::cpu::Va::BITS39),
    };
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-spin");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    let mut srcs = vec![dir.join("harness.c")];
    relift_cache::write_if_changed(&srcs[0], HARNESS.as_bytes()).expect("the harness is writable");
    let shards = emitted(&fe, "F_spin")
        .into_iter()
        .chain(emitted(&Plain(fe), "F_plain"));
    for (name, text) in shards {
        let p = dir.join(&name);
        relift_cache::write_if_changed(&p, text.as_bytes()).expect("the shard is writable");
        if name.ends_with(".c") {
            srcs.push(p);
        }
    }
    let rt = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
    let mut args: Vec<String> = [
        "-std=c11",
        "-O2",
        "-w",
        "-D_GNU_SOURCE",
        "-pthread",
        &format!("-I{}", rt.join("core/include").display()),
        &format!("-I{}", rt.join("cpus/arm64/include").display()),
    ]
    .iter()
    .map(|s| s.to_string())
    .chain(cfg!(target_arch = "x86_64").then(|| "-mcx16".to_string()))
    .collect();
    args.extend(srcs.iter().map(|s| s.display().to_string()));
    let exe = dir.join("spin");
    args.extend(["-o".into(), exe.display().to_string()]);
    clang(&cc, &args);
    let run = relift_proc::command(&exe)
        .output()
        .expect("the harness runs");
    assert!(
        run.status.success(),
        "the spin loop did not end: {:?} {}",
        run.status,
        String::from_utf8_lossy(&run.stdout)
    );
}
