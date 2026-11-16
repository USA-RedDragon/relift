use std::path::{Path, PathBuf};

use relift_cpu_arm64::decode::Features;
use relift_cpu_arm64::frontend::Arm64;
use relift_guest::{Frontend, Home};
use relift_ir::interp::{Monitor, State};

/// The guest address of the memory window.
pub const BASE: u64 = 0x10_0000;
/// Its size in bytes.
pub const WINDOW: usize = 0x4000;
/// Random words per form.
pub const WORDS: usize = 2;
/// Random states per word.
pub const STATES: usize = 12;
/// `u64`s of the context block in the harness records.
pub const CTX: usize = 115;
/// Bytes of C per emitted shard.
pub const SHARD: usize = 256 << 10;

/// A small xorshift generator.
pub struct Rng(pub u64);

impl Rng {
    pub fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// The window contents for `seed`; the harness computes the same bytes.
pub fn fill(seed: u64) -> Vec<u8> {
    let mut r = Rng(seed | 1);
    (0..WINDOW).map(|_| (r.next() >> 56) as u8).collect()
}

pub fn fnv(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| {
        (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3)
    })
}

/// Where a register-file slot sits in the harness context block.
#[derive(Clone, Copy, Debug)]
pub enum Slot {
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

pub const NZCV: usize = 96;
pub const EXCL: usize = 101;
pub const PAC_KEYS: usize = 105;

pub fn slots() -> Vec<Slot> {
    let fe = Arm64 {
        cpu: relift_cpu_arm64::cpu::Cpu::new(Features::ALL, relift_cpu_arm64::cpu::Va::BITS39),
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
                        s if s.starts_with("rl_pac_keys[") => Slot::Word(PAC_KEYS + idx(s)),
                        s => panic!("a home the harness does not know: {s}"),
                    }
                }
            }
        })
        .collect()
}

/// A random register value: mostly small, special or window addresses.
pub fn value(r: &mut Rng) -> u64 {
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
pub fn vector(r: &mut Rng) -> u128 {
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
pub fn state(word: u32, slots: &[Slot], r: &mut Rng) -> State {
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

pub fn pack(s: &State, slots: &[Slot], mem: &[u8], out: &mut Vec<u64>) {
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

/// The harness's event bit for a host fault inside the emitted code.
pub const FAULT: u64 = 16;

/// Whether the run needed the runtime, which the harness does not have.
pub fn left_for_runtime(rec: &[u64]) -> bool {
    rec[2] & !FAULT != 0
}

/// What the interpreter says one run does.
pub struct Expect {
    pub case: usize,
    pub state: State,
    pub mem: u64,
    pub next: Option<u64>,
}

pub const HARNESS: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <relift/arm64/abi.h>
#include <relift/arm64/fp_slow.h>
#include <relift/arm64/pac.h>

extern const rl_fn cases[];
extern const unsigned case_count;

static uint64_t miss;
static int missed;
static uint64_t events;
static sigjmp_buf jb;

static void on_fault(int sig) { (void)sig; siglongjmp(jb, 1); }
static uint8_t buf[WINDOW] __attribute__((aligned(64)));

uint64_t rl_dispatch_miss(rl_ctx *c, uint8_t *mem) { (void)mem; if (!missed) { missed = 1; miss = c->pc; } return RL_OK; }
uint64_t rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem) { return rl_dispatch_miss(c, mem); }
int rl_svc(rl_ctx *c, uint8_t *mem, uint32_t imm) { (void)mem; (void)imm; events |= 4; c->pend.kind = RL_PEND_EXIT_PROCESS; return RL_PENDING; }
int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn) { (void)mem; (void)pc; (void)insn; events |= 1; c->pend.kind = RL_PEND_EXIT_PROCESS; return RL_PENDING; }
int rl_fp_slow(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn) { (void)mem; (void)pc; (void)insn; events |= 2; c->pend.kind = RL_PEND_EXIT_PROCESS; return RL_PENDING; }
uint64_t rl_mrs_cntpct(rl_ctx *c) { (void)c; events |= 8; return 0; }
void rl_msr_fpcr(rl_ctx *c, uint64_t v) { events |= 8; c->fpcr = (uint32_t)v; }
void rl_wfe(rl_ctx *c) { (void)c; events |= 8; }
void rl_preempt(rl_ctx *c) { c->time.budget = INT64_MAX; }
int rl_arm64_fp_flags(void) { return 0; }
int rl_arm64_fp_flags_on;
uint64_t rl_arm64_ret_elsewhere(rl_ctx *c, uint64_t to, uint64_t pc) { if (to == RL_PENDING && c->pend.kind != RL_PEND_NONE) return RL_PENDING; c->pc = pc; c->pend.pc = to; c->pend.sp = RL_SP_NONE; c->pend.kind = RL_PEND_REDIRECT; return RL_PENDING; }
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
    memcpy(rl_pac_keys, &r[105], sizeof rl_pac_keys);
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
    memcpy(&r[105], rl_pac_keys, sizeof rl_pac_keys);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out)
        return 2;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    rl_pac_mode = RL_PAC_QARMA5;
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
        if (sigsetjmp(jb, 1) == 0) {
            uint64_t r = cases[rec[0]](&ctx, mem);
            int pending = r == RL_PENDING && ctx.pend.kind != RL_PEND_NONE && ctx.pend.kind != RL_PEND_REDIRECT;
            res[0] = pending ? RL_PENDING : RL_OK;
        } else {
            res[0] = UINT64_MAX;
            events |= 16;
        }
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

pub fn write(path: &Path, bytes: &[u8]) {
    relift_cache::write_if_changed(path, bytes).expect("the scratch file is writable");
}

pub fn words(bytes: &[u64]) -> Vec<u8> {
    bytes.iter().flat_map(|w| w.to_le_bytes()).collect()
}

pub fn read_words(path: &Path) -> Vec<u64> {
    std::fs::read(path)
        .expect("the harness wrote its output")
        .as_chunks::<8>()
        .0
        .iter()
        .map(|c| u64::from_le_bytes(*c))
        .collect()
}

pub fn clang(cc: &Path, args: &[String]) {
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

/// Compiles `jobs` (source, object, extra flags) on every core.
fn compile_all(cc: &Path, flags: &[String], jobs: &[(PathBuf, PathBuf)]) {
    let next = std::sync::atomic::AtomicUsize::new(0);
    let threads = std::thread::available_parallelism().map_or(4, usize::from);
    std::thread::scope(|s| {
        for _ in 0..threads.min(jobs.len()) {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some((src, obj)) = jobs.get(i) else { break };
                    let mut a = flags.to_vec();
                    a.extend([
                        "-c".into(),
                        src.display().to_string(),
                        "-o".into(),
                        obj.display().to_string(),
                    ]);
                    clang(cc, &a);
                }
            });
        }
    });
}

/// The compiler, the lifted-code flags and the runtime objects that every
/// emitted program links with, built once.
pub struct Harness {
    cc: PathBuf,
    flags: Vec<String>,
    base: Vec<PathBuf>,
}

impl Harness {
    pub fn new(cc: PathBuf, root: &Path, dir: &Path) -> Harness {
        std::fs::create_dir_all(dir).expect("the scratch dir is creatable");
        let rt = root.join("runtime");
        let flags: Vec<String> = [
            "-std=c11",
            "-O2",
            "-ffp-contract=off",
            "-w",
            "-D_GNU_SOURCE",
            &format!("-DWINDOW={WINDOW}"),
            &format!("-DBASE=UINT64_C({BASE:#x})"),
            &format!("-DCTX={CTX}"),
            &format!("-I{}", rt.join("core/include").display()),
            &format!("-I{}", rt.join("cpus/arm64/include").display()),
            &format!("-I{}", rt.join("cpus/arm64").display()),
            &format!("-I{}", rt.join("core").display()),
        ]
        .iter()
        .map(|s| s.to_string())
        .chain(cfg!(target_arch = "x86_64").then(|| "-mcx16".to_string()))
        .collect();
        let harness = dir.join("harness.c");
        write(&harness, HARNESS.as_bytes());
        // The FP exact layer, the binary16 helpers, the cipher steps and the
        // pointer authentication keys and cipher the emitted code calls.
        let mut jobs = vec![(harness, dir.join("harness.o"))];
        for c in ["fp.c", "fp16.c", "crypto.c", "pac.c"] {
            jobs.push((rt.join("cpus/arm64").join(c), dir.join(format!("{c}.o"))));
        }
        compile_all(&cc, &flags, &jobs);
        Harness {
            cc,
            flags,
            base: jobs.into_iter().map(|(_, o)| o).collect(),
        }
    }

    /// Compiles the emitted `files` and the table of the functions `names`
    /// into a harness in `dir`, runs it over the records `input` and returns
    /// its output words.
    pub fn run(
        &self,
        dir: &Path,
        files: &[(String, String)],
        names: &[String],
        input: &[u64],
    ) -> Vec<u64> {
        let _ = std::fs::remove_dir_all(dir);
        std::fs::create_dir_all(dir).expect("the scratch dir is creatable");
        let mut table = String::from("#include <relift/arm64/abi.h>\n");
        for n in names {
            table.push_str(&format!("RL_LIFTED({n});\n"));
        }
        table.push_str("const rl_fn cases[] = {\n");
        for n in names {
            table.push_str(&format!("    {n},\n"));
        }
        table.push_str(&format!(
            "}};\nconst unsigned case_count = {}u;\n",
            names.len()
        ));
        let cases = dir.join("cases.c");
        write(&cases, table.as_bytes());
        let mut jobs = vec![(cases, dir.join("cases.o"))];
        for (name, text) in files {
            write(&dir.join(name), text.as_bytes());
            if name.starts_with("shard") {
                jobs.push((dir.join(name), dir.join(format!("{name}.o"))));
            }
        }
        compile_all(&self.cc, &self.flags, &jobs);
        let exe = dir.join("harness");
        let mut a: Vec<String> = jobs
            .iter()
            .map(|(_, o)| o)
            .chain(&self.base)
            .map(|o| o.display().to_string())
            .collect();
        a.extend(["-lm".into(), "-o".into(), exe.display().to_string()]);
        clang(&self.cc, &a);
        write(&dir.join("in.bin"), &words(input));
        let run = relift_proc::command(&exe)
            .arg(dir.join("in.bin"))
            .arg(dir.join("out.bin"))
            .output()
            .expect("the harness runs");
        assert!(
            run.status.success(),
            "harness {}: {}",
            run.status,
            String::from_utf8_lossy(&run.stderr)
        );
        read_words(&dir.join("out.bin"))
    }
}

/// How the harness's record for one run differs from the interpreter's.
pub fn diffs(fe: &Arm64, slots: &[Slot], e: &Expect, rec: &[u64]) -> Vec<String> {
    let ctx = &rec[4..];
    let mut diffs = Vec::new();
    if rec[2] & FAULT != 0 {
        diffs.push("host fault".to_string());
    }
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
    diffs
}
