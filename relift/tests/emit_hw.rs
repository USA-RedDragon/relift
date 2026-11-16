//! The emitted C means what the IR means for the section 13 operations
//! (docs/abi.md): ticks, `tick.if`, I/O loads and stores of both kinds,
//! endians and odd sizes, and sync points, on random blocks over a Game
//! Boy-like map. Each block is a one-instruction function compiled with
//! clang next to a mock scheduler (a device model that the IR interpreter
//! runs through its `Devices` trait too), and registers, memory, the clock,
//! the device events and the exit target must match the interpreter on the
//! same states. A mutation control breaks one tick or one byte order in the
//! IR and must be seen. Skipped with a note when clang is not on `PATH`.

mod mockabi;

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_emit::{Function, Insn, Options, emit};
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::interp::{
    Devices, Error, Fault, FlatMemory, GuestMemory, NoIntrinsics, State, run_with,
};
use relift_ir::{
    AddrClass, BinOp, Block, Builder, CmpOp, Endian, Exit, Flow, FpOp, IoKind, IoMap, IoRange,
    Next, Op, Reg, RegDecl, RegFile, Value, Width,
};

const WINDOW: usize = 0x1_0000;
const EXTRA: usize = 64;
const IRQ_VECTOR: u64 = 0x40;

static DECLS: [RegDecl; 6] = [
    RegDecl {
        name: "a",
        width: Width::W8,
    },
    RegDecl {
        name: "b",
        width: Width::W8,
    },
    RegDecl {
        name: "hl",
        width: Width::W16,
    },
    RegDecl {
        name: "sp",
        width: Width::W16,
    },
    RegDecl {
        name: "z",
        width: Width::W1,
    },
    RegDecl {
        name: "ime",
        width: Width::W1,
    },
];

const NAMES: [&str; 6] = ["a", "b", "hl", "sp", "z", "ime"];

fn file() -> RegFile {
    RegFile::new("hw", &DECLS)
}

/// A frontend for a made-up 16-bit machine with a clock; it only names
/// where its registers and clock live.
struct Hw;

impl Frontend for Hw {
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
        &["hwabi.h"]
    }
    fn home(&self, reg: Reg) -> Home {
        Home::Field(format!("c->{}", NAMES[reg.index()]))
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
    fn clock(&self) -> Option<&'static str> {
        Some("c->time")
    }
}

const ABI_H: &str = r#"#ifndef HWABI_H
#define HWABI_H
#include <relift/abi_core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct rl_ctx {
    uint8_t a, b;
    uint16_t hl, sp;
    uint8_t z, ime;
    uint64_t pc;
    rl_pending pend;
    rl_time time;
};

static inline uint8_t rl_ld8(const uint8_t *mem, uint64_t a) { return mem[a]; }
static inline uint16_t rl_ld16(const uint8_t *mem, uint64_t a) { uint16_t v; memcpy(&v, mem + a, 2); return v; }
static inline uint32_t rl_ld32(const uint8_t *mem, uint64_t a) { uint32_t v; memcpy(&v, mem + a, 4); return v; }
static inline uint64_t rl_ld64(const uint8_t *mem, uint64_t a) { uint64_t v; memcpy(&v, mem + a, 8); return v; }
static inline rl_u128 rl_ld128(const uint8_t *mem, uint64_t a) { rl_u128 v; memcpy(&v, mem + a, 16); return v; }
static inline void rl_st8(uint8_t *mem, uint64_t a, uint8_t v) { mem[a] = v; }
static inline void rl_st16(uint8_t *mem, uint64_t a, uint16_t v) { memcpy(mem + a, &v, 2); }
static inline void rl_st32(uint8_t *mem, uint64_t a, uint32_t v) { memcpy(mem + a, &v, 4); }
static inline void rl_st64(uint8_t *mem, uint64_t a, uint64_t v) { memcpy(mem + a, &v, 8); }
static inline void rl_st128(uint8_t *mem, uint64_t a, rl_u128 v) { memcpy(mem + a, &v, 16); }

"#;

/// The mock scheduler: the device model of the interpreter, in C.
const HARNESS: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hwabi.h>

#define MAX_EV 4096

static uint8_t buf[WINDOW + EXTRA] __attribute__((aligned(64)));
static uint64_t ev[MAX_EV][4];
static unsigned nev;
static uint64_t miss;
static int missed;
static int taken;

uint64_t rl_dispatch_miss(rl_ctx *c, uint8_t *mem) { (void)mem; if (!missed) { missed = 1; miss = c->pc; } return RL_OK; }

static void log4(uint64_t k, uint64_t a, uint64_t b, uint64_t t)
{
    if (nev < MAX_EV) { ev[nev][0] = k; ev[nev][1] = a; ev[nev][2] = b; ev[nev][3] = t; nev++; }
}

static uint64_t now(rl_ctx *c) { return c->time.deadline - (uint64_t)c->time.budget; }

static uint8_t dev_byte(uint64_t addr, uint64_t t)
{
    return (uint8_t)((addr * 31 + t * 7) >> 1);
}

uint64_t rl_io_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    uint64_t t = now(c), v = 0;
    for (uint32_t i = 0; i < size; i++) {
        v |= (uint64_t)dev_byte(addr + i, t) << (8 * i);
        log4(0, addr + i, t, 0);
    }
    return v;
}

void rl_io_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    uint64_t t = now(c);
    for (uint32_t i = 0; i < size; i++)
        log4(1, addr + i, (value >> (8 * i)) & 0xff, t);
}

int rl_sync(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    uint64_t t = now(c);
    log4(2, next, t, 0);
    if (t % 2 == 1 && c->ime == 1) {
        uint16_t sp = (uint16_t)(c->sp - 2);
        uint16_t n = (uint16_t)next;
        memcpy(mem + sp, &n, 2);
        c->sp = sp;
        c->ime = 0;
        c->time.budget -= 20;
        taken = 1;
    }
    return RL_OK;
}

static void fill(uint64_t seed)
{
    uint64_t x = seed | 1;
    for (size_t i = 0; i < WINDOW; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        buf[i] = (uint8_t)((x * UINT64_C(0x2545f4914f6cdd1d)) >> 56);
    }
    memset(buf + WINDOW, 0, EXTRA);
}

static uint64_t fnv(void)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < WINDOW; i++)
        h = (h ^ buf[i]) * UINT64_C(0x100000001b3);
    return h;
}

extern const rl_fn cases[];
extern const unsigned case_count;

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out)
        return 2;
    uint64_t rec[10];
    static rl_ctx ctx;
    while (fread(rec, sizeof rec, 1, in) == 1) {
        if (rec[0] >= case_count)
            return 3;
        memset(&ctx, 0, sizeof ctx);
        ctx.a = (uint8_t)rec[4];
        ctx.b = (uint8_t)rec[5];
        ctx.hl = (uint16_t)rec[6];
        ctx.sp = (uint16_t)rec[7];
        ctx.z = (uint8_t)rec[8];
        ctx.ime = (uint8_t)rec[9];
        ctx.time.deadline = rec[3] + rec[2];
        ctx.time.budget = (int64_t)rec[2];
        fill(rec[1]);
        nev = 0;
        missed = 0;
        miss = 0;
        taken = 0;
        uint64_t rc = (uint64_t)cases[rec[0]](&ctx, buf);
        uint64_t head[13] = {
            rc, missed ? miss : UINT64_MAX, (uint64_t)taken, fnv(), now(&ctx),
            ctx.a, ctx.b, ctx.hl, ctx.sp, ctx.z, ctx.ime, nev, 0,
        };
        fwrite(head, 8, 12, out);
        fwrite(ev, 32, nev, out);
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
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n.max(1)
    }
    fn chance(&mut self, p: f64) -> bool {
        (self.next() >> 11) as f64 / (1u64 << 53) as f64 <= p
    }
    fn pick<T: Copy>(&mut self, xs: &[T]) -> T {
        xs[self.below(xs.len() as u64) as usize]
    }
}

fn gb_map() -> IoMap {
    let r = |first, last, read, write| IoRange {
        first,
        last,
        read,
        write,
    };
    IoMap {
        ranges: vec![
            r(0x0000, 0x7fff, AddrClass::Memory, AddrClass::Device),
            r(0xff00, 0xff7f, AddrClass::Device, AddrClass::Device),
            r(0xffff, 0xffff, AddrClass::Device, AddrClass::Device),
        ],
        default: AddrClass::Memory,
    }
}

const CONSTS_R: [u64; 10] = [
    0xff00, 0xff44, 0xff7e, 0xff80, 0xc000, 0x4000, 0x7fff, 0xfffe, 0xffff, 0x9000,
];
const CONSTS_W: [u64; 8] = [
    0xff01, 0xff40, 0xc123, 0x2000, 0xfffe, 0xffff, 0xa000, 0x7ffe,
];

fn kind(r: &mut Rng) -> IoKind {
    if r.chance(0.2) {
        IoKind::Device
    } else {
        IoKind::Any
    }
}

fn endian(r: &mut Rng) -> Endian {
    if r.chance(0.2) {
        Endian::Big
    } else {
        Endian::Little
    }
}

fn add_ops(
    b: &mut Builder,
    r: &mut Rng,
    vals8: &mut Vec<Value>,
    addrs: &mut Vec<Value>,
    bits: &mut Vec<Value>,
    n: u64,
) {
    for _ in 0..n {
        match r.below(7) {
            0 => b.tick(4 * (1 + r.below(3) as u32)),
            1 => {
                let c = r.pick(bits);
                b.tick_if(c, 4);
            }
            2 | 3 => {
                let a = if r.chance(0.5) {
                    r.pick(addrs)
                } else {
                    b.konst(Width::W16, u128::from(r.pick(&CONSTS_R)))
                };
                let w = Width::new(8 * r.pick(&[1u32, 1, 2, 3, 4, 5, 8, 9, 16]));
                let (k, e) = (kind(r), endian(r));
                let v = b.io_load(a, w, e, k);
                if w == Width::W8 {
                    vals8.push(v);
                } else {
                    let lo = b.extract(v, 0, Width::W8);
                    vals8.push(lo);
                    let a = b.extract(v, 0, Width::W16);
                    addrs.push(a);
                }
            }
            4 => {
                let a = if r.chance(0.5) {
                    r.pick(addrs)
                } else {
                    b.konst(Width::W16, u128::from(r.pick(&CONSTS_W)))
                };
                let mut v = r.pick(vals8);
                for _ in 0..r.pick(&[0u32, 0, 1, 2, 3, 7, 8, 15]) {
                    let h = r.pick(vals8);
                    v = b.concat(h, v);
                }
                let (k, e) = (kind(r), endian(r));
                b.io_store(a, v, e, k);
            }
            5 => {
                let x = r.pick(vals8);
                let y = r.pick(vals8);
                let s = b.bin(r.pick(&[BinOp::Add, BinOp::Xor, BinOp::Sub]), x, y);
                vals8.push(s);
                let z = b.konst(Width::W8, 0);
                let c = b.cmp(CmpOp::Eq, s, z);
                bits.push(c);
            }
            _ => {
                let x = r.pick(addrs);
                let one = b.konst(Width::W16, 1);
                let y = b.bin(BinOp::Add, x, one);
                addrs.push(y);
            }
        }
    }
}

fn make_block(r: &mut Rng, pc: u64) -> Block {
    let mut b = Builder::new(file(), Width::W16, pc);
    let mut vals8 = vec![b.get(Reg(0)), b.get(Reg(1))];
    let mut addrs = vec![b.get(Reg(2))];
    let mut bits = vec![b.get(Reg(4))];
    let n = 3 + r.below(8);
    add_ops(&mut b, r, &mut vals8, &mut addrs, &mut bits, n);
    let a = r.pick(&vals8);
    b.set(Reg(0), a);
    let h = r.pick(&addrs);
    b.set(Reg(2), h);
    let c = r.pick(&bits);
    b.set(Reg(4), c);
    if r.chance(0.6) {
        b.sync();
    }
    let next = if r.chance(0.5) {
        Next::Const(pc + 1)
    } else {
        Next::Select {
            cond: c,
            taken: 0x200,
            not_taken: pc + 1,
        }
    };
    b.finish(
        1,
        Exit {
            next,
            flow: Flow::Jump,
        },
    )
}

/// Breaks one tick, else one I/O load's byte order; false when there is
/// neither.
fn mutate(b: &mut Block) -> bool {
    for i in &mut b.insts {
        match i.op {
            Op::Tick(n) => {
                i.op = Op::Tick(n + 4);
                return true;
            }
            Op::TickIf(c, n) => {
                i.op = Op::TickIf(c, n + 4);
                return true;
            }
            _ => {}
        }
    }
    for i in &mut b.insts {
        if let Op::IoLoad { addr, endian, kind } = i.op {
            i.op = Op::IoLoad {
                addr,
                endian: if endian == Endian::Big {
                    Endian::Little
                } else {
                    Endian::Big
                },
                kind,
            };
            return true;
        }
    }
    false
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Ev {
    Read(u64, u64),
    Write(u64, u8, u64),
    Sync(u64, u64),
}

/// The interpreter's device model, with the rules of the mock scheduler: a
/// sync point only calls in when the budget is spent.
struct Model {
    map: IoMap,
    time: u64,
    start: u64,
    budget: i64,
    log: Vec<Ev>,
    taken: bool,
}

fn dev_byte(addr: u64, time: u64) -> u8 {
    (addr.wrapping_mul(31).wrapping_add(time.wrapping_mul(7)) >> 1) as u8
}

impl Devices for Model {
    fn tick(&mut self, cycles: u64) {
        self.time += cycles;
    }
    fn map(&self) -> &IoMap {
        &self.map
    }
    fn io_read(
        &mut self,
        _m: &mut dyn GuestMemory,
        addr: u64,
        buf: &mut [u8],
    ) -> Result<(), Fault> {
        for (i, b) in buf.iter_mut().enumerate() {
            let a = addr + i as u64;
            *b = dev_byte(a, self.time);
            self.log.push(Ev::Read(a, self.time));
        }
        Ok(())
    }
    fn io_write(&mut self, _m: &mut dyn GuestMemory, addr: u64, data: &[u8]) -> Result<(), Fault> {
        for (i, b) in data.iter().enumerate() {
            self.log.push(Ev::Write(addr + i as u64, *b, self.time));
        }
        Ok(())
    }
    fn sync(
        &mut self,
        st: &mut State,
        mem: &mut dyn GuestMemory,
        next: u64,
    ) -> Result<Option<u64>, Error> {
        let spent = self.budget - (self.time - self.start) as i64;
        if spent > 0 {
            return Ok(None);
        }
        self.log.push(Ev::Sync(next, self.time));
        if self.time % 2 == 1 && st.regs[5] == 1 {
            let sp = (st.regs[3] as u64).wrapping_sub(2) & 0xffff;
            mem.write(sp, &(next as u16).to_le_bytes())
                .map_err(Error::Fault)?;
            st.regs[3] = u128::from(sp);
            st.regs[5] = 0;
            self.time += 20;
            self.taken = true;
            return Ok(Some(IRQ_VECTOR));
        }
        Ok(None)
    }
}

fn fnv(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| {
        (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3)
    })
}

fn fill(seed: u64) -> Vec<u8> {
    let mut x = seed | 1;
    let mut out: Vec<u8> = (0..WINDOW)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x.wrapping_mul(0x2545_f491_4f6c_dd1d) >> 56) as u8
        })
        .collect();
    out.resize(WINDOW + EXTRA, 0);
    out
}

/// One run: the state it starts in.
#[derive(Clone, Debug)]
struct Start {
    seed: u64,
    budget: i64,
    t0: u64,
    regs: Vec<u128>,
}

#[derive(Debug, PartialEq, Eq)]
struct Res {
    pc: u64,
    taken: bool,
    mem: u64,
    time: u64,
    regs: Vec<u128>,
    log: Vec<Ev>,
}

fn interp(b: &Block, s: &Start) -> Res {
    let mut st = State::new(file().len());
    st.regs = s.regs.clone();
    let mut m = FlatMemory {
        base: 0,
        bytes: fill(s.seed),
    };
    let mut d = Model {
        map: gb_map(),
        time: s.t0,
        start: s.t0,
        budget: s.budget,
        log: Vec::new(),
        taken: false,
    };
    let o = run_with(b, &mut st, &mut m, &mut NoIntrinsics, &mut d).expect("runs");
    Res {
        pc: o.resume.unwrap_or(o.next),
        taken: d.taken,
        mem: fnv(&m.bytes[..WINDOW]),
        time: d.time,
        regs: st.regs,
        log: d.log,
    }
}

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

/// Emits `blocks` as functions `F_k`, compiles them with the mock scheduler
/// into `dir` and returns the program.
fn build(cc: &Path, blocks: &[Block], dir: &Path) -> PathBuf {
    let _ = std::fs::remove_dir_all(dir);
    std::fs::create_dir_all(dir.join("inc")).expect("the scratch dir is creatable");
    write(
        &dir.join("inc/hwabi.h"),
        format!("{ABI_H}{}#endif\n", mockabi::DISPATCH_H).as_bytes(),
    );
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
        &Hw,
        &fns,
        &[],
        &Options {
            io_map: gb_map(),
            ..Options::default()
        },
    );
    assert_eq!(out.undefined, 0, "every block is expressed");
    let mut cases = String::from("#include <hwabi.h>\n");
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
    write(
        &sources[1],
        format!("{HARNESS}{}", mockabi::DISPATCH_C).as_bytes(),
    );
    for f in &out.files {
        write(&dir.join(&f.name), f.text.as_bytes());
        if f.name.starts_with("shard") {
            sources.push(dir.join(&f.name));
        }
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let flags: Vec<String> = [
        "-std=c11".to_string(),
        "-O2".into(),
        "-w".into(),
        "-D_GNU_SOURCE".into(),
        format!("-DWINDOW={WINDOW}"),
        format!("-DEXTRA={EXTRA}"),
        format!("-I{}", root.join("runtime/core/include").display()),
        format!("-I{}", root.join("runtime/core").display()),
        format!("-I{}", dir.join("inc").display()),
        format!("-I{}", dir.display()),
    ]
    .into();
    let objs: Vec<PathBuf> = (0..sources.len())
        .map(|i| dir.join(format!("o{i}.o")))
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
            s.spawn(move || clang(cc, &a));
        }
    });
    let exe = dir.join("harness");
    let mut a: Vec<String> = objs.iter().map(|o| o.display().to_string()).collect();
    a.extend(["-o".into(), exe.display().to_string()]);
    clang(cc, &a);
    exe
}

/// Runs `exe` on `(case, start)` pairs and returns one result per pair.
fn run(exe: &Path, dir: &Path, runs: &[(usize, Start)]) -> Vec<Res> {
    let mut input = Vec::new();
    for (k, s) in runs {
        input.extend([*k as u64, s.seed, s.budget as u64, s.t0]);
        input.extend(s.regs.iter().map(|r| *r as u64));
    }
    let bytes: Vec<u8> = input.iter().flat_map(|w| w.to_le_bytes()).collect();
    write(&dir.join("in.bin"), &bytes);
    let out = relift_proc::command(exe)
        .arg(dir.join("in.bin"))
        .arg(dir.join("out.bin"))
        .output()
        .expect("the harness runs");
    assert!(
        out.status.success(),
        "harness: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    let words: Vec<u64> = std::fs::read(dir.join("out.bin"))
        .expect("the harness wrote its output")
        .as_chunks::<8>()
        .0
        .iter()
        .map(|c| u64::from_le_bytes(*c))
        .collect();
    let mut at = 0;
    let mut res = Vec::new();
    for _ in runs {
        let h = &words[at..at + 12];
        at += 12;
        assert_eq!(h[0], 0, "the function returned {}", h[0]);
        let n = h[11] as usize;
        let log = words[at..at + 4 * n]
            .chunks(4)
            .map(|e| match e[0] {
                0 => Ev::Read(e[1], e[2]),
                1 => Ev::Write(e[1], e[2] as u8, e[3]),
                _ => Ev::Sync(e[1], e[2]),
            })
            .collect();
        at += 4 * n;
        res.push(Res {
            pc: h[1],
            taken: h[2] != 0,
            mem: h[3],
            time: h[4],
            regs: h[5..11].iter().map(|r| u128::from(*r)).collect(),
            log,
        });
    }
    assert_eq!(at, words.len());
    res
}

fn start(r: &mut Rng) -> Start {
    let any = r.below(0x10000);
    Start {
        seed: r.next(),
        budget: r.pick(&[0, 0, 0, -3, 5, 17, 40, 100_000]),
        t0: r.below(1000),
        regs: vec![
            u128::from(r.below(256) as u8),
            u128::from(r.below(256) as u8),
            u128::from(r.pick(&[0xff40u64, 0xc000, 0xff7f, 0x8000, 0xfffe, 0x3fff, any])),
            u128::from(0xdff0 - 2 * r.below(8)),
            u128::from(r.below(2)),
            u128::from(r.below(2)),
        ],
    }
}

#[test]
fn section_13_blocks_match_the_interpreter() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let mut r = Rng(0x5eed_1313);
    let n = 600;
    let per = 6;
    let blocks: Vec<Block> = (0..n).map(|i| make_block(&mut r, 0x1000 + 2 * i)).collect();
    let mut broken = blocks.clone();
    let changed: Vec<bool> = broken.iter_mut().map(mutate).collect();
    let runs: Vec<(usize, Start)> = (0..blocks.len())
        .flat_map(|k| (0..per).map(move |_| k))
        .map(|k| (k, start(&mut r)))
        .collect();
    let base = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("emit-hw");
    let good = build(&cc, &blocks, &base.join("good"));
    let bad = build(&cc, &broken, &base.join("broken"));
    let got = run(&good, &base.join("good"), &runs);
    let got_bad = run(&bad, &base.join("broken"), &runs);
    let (mut ok, mut syncs, mut skipped, mut irqs, mut devr, mut devw) = (0, 0, 0, 0, 0, 0);
    let (mut seen, mut caught) = (0, 0);
    let mut first = None;
    for (((k, s), g), gb) in runs.iter().zip(&got).zip(&got_bad) {
        let want = interp(&blocks[*k], s);
        for e in &want.log {
            match e {
                Ev::Sync(..) => syncs += 1,
                Ev::Read(..) => devr += 1,
                Ev::Write(..) => devw += 1,
            }
        }
        irqs += usize::from(want.taken);
        skipped += usize::from(
            blocks[*k].insts.last().is_some_and(|i| i.op == Op::Sync)
                && !want.log.iter().any(|e| matches!(e, Ev::Sync(..))),
        );
        if &want == g {
            ok += 1;
        } else if first.is_none() {
            first = Some(format!("{}\nwant {want:x?}\ngot {g:x?}", blocks[*k]));
        }
        if changed[*k] && interp(&broken[*k], s) != want {
            seen += 1;
            caught += usize::from(gb != &want);
        }
    }
    println!(
        "{} runs, {ok} matched; device bytes read {devr} written {devw}, syncs {syncs} (skipped on budget {skipped}), interrupts {irqs}; mutation caught {caught}/{seen}",
        runs.len()
    );
    assert!(
        first.is_none(),
        "first difference:\n{}",
        first.unwrap_or_default()
    );
    assert!(
        syncs > 0 && skipped > 0 && irqs > 0 && devr > 0 && devw > 0,
        "control: the sample misses an event kind"
    );
    assert!(
        seen > 0 && caught == seen,
        "mutation control: caught {caught}/{seen}"
    );
}
