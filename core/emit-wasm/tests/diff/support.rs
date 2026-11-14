//! The wasmtime side of the differential tests: one engine, one linear
//! memory and one funcref table shared by every module a test loads, and the
//! runtime imports, with a device model the IR interpreter uses too.

use std::collections::BTreeMap;

use relift_emit_wasm::{Output, RC_CHAIN, ctx};
use relift_ir::interp::{Devices, Error, Fault, GuestMemory, State};
use relift_ir::{AddrClass, IoMap, IoRange, Reg, RegFile, Width};
use wasmtime::{
    AsContext, AsContextMut, Caller, Config, Engine, Func, Instance, Linker, Memory, MemoryType,
    Module, Ref, RefType, Store, Table, TableType, TypedFunc, Val,
};

/// A memory image whose `Debug` names its first bytes and a hash, so a
/// failure message stays short.
#[derive(Clone, PartialEq, Eq)]
pub struct Bytes(pub Vec<u8>);

impl std::fmt::Debug for Bytes {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let h = self.0.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, b| {
            (h ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3)
        });
        write!(f, "{} bytes, hash {h:016x}", self.0.len())
    }
}

/// The first offsets where `a` and `b` differ, for failure messages.
pub fn diff_bytes(a: &Bytes, b: &Bytes) -> String {
    let d: Vec<String> =
        a.0.iter()
            .zip(&b.0)
            .enumerate()
            .filter(|(_, (x, y))| x != y)
            .take(8)
            .map(|(i, (x, y))| format!("{i:#x}: {x:02x} vs {y:02x}"))
            .collect();
    d.join(", ")
}

/// How many cases a test runs: a small sample by default, the full run
/// when `RELIFT_WASM_CHECK=full`.
pub fn scale(ci: u64, full: u64) -> u64 {
    match std::env::var("RELIFT_WASM_CHECK") {
        Ok(v) if v == "full" => full,
        _ => ci,
    }
}

/// The seed: `RELIFT_WASM_SEED`, else 1.
pub fn seed() -> u64 {
    std::env::var("RELIFT_WASM_SEED")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(1)
}

/// A small deterministic generator.
pub struct Rng(u64);

impl Rng {
    pub fn new(label: &str, seed: u64) -> Rng {
        let mut h = 0xcbf2_9ce4_8422_2325u64 ^ seed.wrapping_mul(0x9e37_79b9_7f4a_7c15);
        for b in label.bytes() {
            h = (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
        }
        Rng(h | 1)
    }
    pub fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n.max(1)
    }
    pub fn chance(&mut self, p: f64) -> bool {
        (self.next() >> 11) as f64 / (1u64 << 53) as f64 <= p
    }
    pub fn pick<T: Copy>(&mut self, xs: &[T]) -> T {
        xs[self.below(xs.len() as u64) as usize]
    }
    pub fn wide(&mut self) -> u128 {
        (u128::from(self.next()) << 64) | u128::from(self.next())
    }
    /// A value of width `w` biased to edge cases.
    pub fn value(&mut self, w: Width) -> u128 {
        let m = w.mask();
        let v = match self.below(8) {
            0 => 0,
            1 => m,
            2 => 1u128 << (w.bits() - 1),
            3 => (1u128 << (w.bits() - 1)).wrapping_sub(1),
            4 => self.below(17).into(),
            _ => self.wide(),
        };
        v & m
    }
}

/// The context address tests use.
pub const CTX: i32 = 0x100;
/// The guest memory bias: guest address 0 sits at this offset.
pub const BIAS: i32 = 0x1_0000;
/// The guest memory window a test may touch, from guest address 0.
pub const WINDOW: usize = 0x1_0000;
/// Pages of linear memory.
pub const PAGES: u64 = 3;
/// The table slot of the miss entry.
pub const MISS_SLOT: u32 = 0;
/// The first table slot a module may use.
pub const FIRST_SLOT: u32 = 1;

/// An event of the device model, as both sides log it, one per byte.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Ev {
    Read(u64, u64),
    Write(u64, u8, u64),
    Sync(u64, u64),
}

/// The interrupt rule of the device model: a sync point at an odd cycle
/// count with `ime` set pushes the return address on a 16-bit stack at `sp`,
/// clears `ime`, charges 20 cycles and enters `vector`.
#[derive(Clone, Copy, Debug)]
pub struct Irq {
    pub sp: Reg,
    pub ime: Reg,
    pub vector: u64,
}

/// A Game Boy-like device model: reads give a hash of the address and the
/// cycle, writes are logged, and `irq` decides interrupts.
#[derive(Clone, Debug)]
pub struct Model {
    pub map: IoMap,
    pub time: u64,
    pub log: Vec<Ev>,
    pub irq: Option<Irq>,
    /// How many interrupts were taken.
    pub taken: u64,
}

pub fn gb_map() -> IoMap {
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

fn dev_byte(addr: u64, time: u64) -> u8 {
    (addr.wrapping_mul(31).wrapping_add(time.wrapping_mul(7)) >> 1) as u8
}

impl Model {
    pub fn new(map: IoMap, time: u64, irq: Option<Irq>) -> Model {
        Model {
            map,
            time,
            log: Vec::new(),
            irq,
            taken: 0,
        }
    }

    fn read(&mut self, addr: u64, buf: &mut [u8]) {
        for (i, b) in buf.iter_mut().enumerate() {
            let a = addr + i as u64;
            *b = dev_byte(a, self.time);
            self.log.push(Ev::Read(a, self.time));
        }
    }

    fn write(&mut self, addr: u64, data: &[u8]) {
        for (i, b) in data.iter().enumerate() {
            self.log.push(Ev::Write(addr + i as u64, *b, self.time));
        }
    }

    fn enter(
        &mut self,
        st: &mut State,
        mem: &mut dyn GuestMemory,
        next: u64,
    ) -> Result<Option<u64>, Error> {
        self.log.push(Ev::Sync(next, self.time));
        let Some(irq) = self.irq else {
            return Ok(None);
        };
        if self.time % 2 == 1 && st.regs[irq.ime.index()] == 1 {
            let sp = (st.regs[irq.sp.index()] as u64).wrapping_sub(2) & 0xffff;
            mem.write(sp, &(next as u16).to_le_bytes())
                .map_err(Error::Fault)?;
            st.regs[irq.sp.index()] = u128::from(sp);
            st.regs[irq.ime.index()] = 0;
            self.time += 20;
            self.taken += 1;
            return Ok(Some(irq.vector));
        }
        Ok(None)
    }
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
        self.read(addr, buf);
        Ok(())
    }
    fn io_write(&mut self, _m: &mut dyn GuestMemory, addr: u64, data: &[u8]) -> Result<(), Fault> {
        self.write(addr, data);
        Ok(())
    }
    fn sync(
        &mut self,
        st: &mut State,
        mem: &mut dyn GuestMemory,
        next: u64,
    ) -> Result<Option<u64>, Error> {
        self.enter(st, mem, next)
    }
}

/// Guest memory over a slice of the linear memory, from guest address 0.
pub struct Slice<'a>(pub &'a mut [u8]);

impl GuestMemory for Slice<'_> {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        let fault = Fault {
            addr,
            size: buf.len() as u32,
            write: false,
        };
        let a = usize::try_from(addr).map_err(|_| fault)?;
        let src = self.0.get(a..a + buf.len()).ok_or(fault)?;
        buf.copy_from_slice(src);
        Ok(())
    }
    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        let fault = Fault {
            addr,
            size: data.len() as u32,
            write: true,
        };
        let a = usize::try_from(addr).map_err(|_| fault)?;
        self.0
            .get_mut(a..a + data.len())
            .ok_or(fault)?
            .copy_from_slice(data);
        Ok(())
    }
}

/// What the runtime imports saw.
#[derive(Default)]
pub struct Host {
    pub memory: Option<Memory>,
    pub table: Option<Table>,
    pub regs: Option<RegFile>,
    /// Guest entry to table slot.
    pub slots: BTreeMap<u64, u32>,
    pub slow: Vec<(u32, u64, u64)>,
    pub traps: Vec<(u32, u64)>,
    pub trap_rc: i32,
    pub undefined: Vec<(u64, u64)>,
    pub misses: Vec<u64>,
    pub model: Option<Model>,
    /// Whether `rt.sync` runs a taken interrupt (chain mode) or redirects
    /// `ctx::PC` to it (step mode).
    pub nested_irq: bool,
    pub intrinsic_calls: Vec<(String, Vec<i64>)>,
}

fn rd<const N: usize>(m: &[u8], at: usize) -> [u8; N] {
    m[at..at + N].try_into().expect("in range")
}

/// The context word at `off` of context `c`.
pub fn ctx_u64(m: &[u8], c: i32, off: u32) -> u64 {
    u64::from_le_bytes(rd(m, c as usize + off as usize))
}

pub fn set_ctx_u64(m: &mut [u8], c: i32, off: u32, v: u64) {
    let at = c as usize + off as usize;
    m[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

pub fn get_reg(m: &[u8], c: i32, r: Reg, w: Width) -> u128 {
    let at = c as usize + ctx::slot(r) as usize;
    let lo = u128::from(u64::from_le_bytes(rd(m, at)));
    let hi = if w.bits() > 64 {
        u128::from(u64::from_le_bytes(rd(m, at + 8)))
    } else {
        0
    };
    (lo | (hi << 64)) & w.mask()
}

pub fn put_reg(m: &mut [u8], c: i32, r: Reg, v: u128) {
    let at = c as usize + ctx::slot(r) as usize;
    m[at..at + 16].copy_from_slice(&v.to_le_bytes());
}

fn guest(m: &mut [u8]) -> &mut [u8] {
    &mut m[BIAS as usize..BIAS as usize + WINDOW + 64]
}

/// The clock the context holds: `deadline - budget`.
fn clock(m: &[u8], c: i32) -> u64 {
    ctx_u64(m, c, ctx::DEADLINE).wrapping_sub(ctx_u64(m, c, ctx::BUDGET))
}

/// Sets the clock to `t` with a spent budget, so every sync point calls in.
pub fn set_clock(m: &mut [u8], c: i32, t: u64) {
    set_ctx_u64(m, c, ctx::DEADLINE, t);
    set_ctx_u64(m, c, ctx::BUDGET, 0);
}

fn state_of(m: &[u8], c: i32, regs: &RegFile) -> State {
    let mut st = State::new(regs.len());
    for (i, d) in regs.decls().iter().enumerate() {
        st.regs[i] = get_reg(m, c, Reg(i as u16), d.width);
    }
    st
}

fn put_state(m: &mut [u8], c: i32, st: &State) {
    for (i, v) in st.regs.iter().enumerate() {
        put_reg(m, c, Reg(i as u16), *v);
    }
}

fn div128(alo: i64, ahi: i64, blo: i64, bhi: i64, op: i32, bits: i32) -> (i64, i64) {
    let w = Width::new(bits as u32);
    let a = (u128::from(ahi as u64) << 64) | u128::from(alo as u64);
    let b = (u128::from(bhi as u64) << 64) | u128::from(blo as u64);
    let o = match op {
        0 => relift_ir::BinOp::UDiv,
        1 => relift_ir::BinOp::SDiv,
        2 => relift_ir::BinOp::URem,
        _ => relift_ir::BinOp::SRem,
    };
    let r = relift_ir::interp::eval_bin(o, a, b, w);
    (r as u64 as i64, (r >> 64) as u64 as i64)
}

/// The result of the test intrinsic `t.hash` on `args`.
pub fn hash_intrinsic(args: &[u128]) -> u128 {
    args.iter()
        .fold(0x1234_5678_9abc_def0_1122_3344_5566_7788u128, |h, a| {
            (h ^ a).wrapping_mul(0x0000_0100_0000_0000_0000_0000_0000_013b)
        })
}

/// `rt.sync`: runs the device model's sync, then either enters a taken
/// interrupt as a nested call (chain mode) or redirects `ctx::PC` to it
/// (step mode).
fn sync(mut cx: Caller<'_, Host>, c: i32, mem: i32, next: i64) -> i32 {
    let memory = cx.data().memory.expect("memory");
    let regs = cx.data().regs.expect("register file");
    let mut model = cx.data_mut().model.take().expect("a device model");
    let m = memory.data_mut(&mut cx);
    model.time = clock(m, c);
    let mut st = state_of(m, c, &regs);
    let taken = model
        .enter(&mut st, &mut Slice(guest(m)), next as u64)
        .expect("the entry stays in guest memory");
    put_state(m, c, &st);
    set_clock(m, c, model.time);
    cx.data_mut().model = Some(model);
    let Some(entry) = taken else {
        return 0;
    };
    set_ctx_u64(memory.data_mut(&mut cx), c, ctx::PC, entry);
    if !cx.data().nested_irq {
        return 0;
    }
    drive(&mut cx, c, mem, RC_CHAIN).expect("the handler runs")
}

/// The runtime's half of the fallback without tail calls: while a call
/// returns [`RC_CHAIN`], looks up `ctx::PC` and calls that function.
pub fn drive(
    cx: &mut impl AsContextMut<Data = Host>,
    c: i32,
    mem: i32,
    mut rc: i32,
) -> Result<i32, String> {
    while rc == RC_CHAIN {
        let memory = cx.as_context().data().memory.expect("memory");
        let table = cx.as_context().data().table.expect("table");
        let pc = ctx_u64(memory.data(&*cx), c, ctx::PC);
        let slot = cx
            .as_context()
            .data()
            .slots
            .get(&pc)
            .copied()
            .unwrap_or(MISS_SLOT);
        let f = table
            .get(&mut *cx, u64::from(slot))
            .and_then(|r| r.as_func().flatten().copied())
            .ok_or("an empty slot")?;
        let mut out = [Val::I32(0)];
        f.call(&mut *cx, &[Val::I32(c), Val::I32(mem)], &mut out)
            .map_err(|e| format!("trap: {e:#}"))?;
        rc = out[0].unwrap_i32();
    }
    Ok(rc)
}

/// The linker with every runtime import.
pub fn linker(engine: &Engine, intrinsics: &[&str]) -> Linker<Host> {
    let mut l: Linker<Host> = Linker::new(engine);
    l.func_wrap(
        "rt",
        "slow",
        |mut c: Caller<'_, Host>, _: i32, _: i32, code: i32, pc: i64, insn: i64| -> i32 {
            c.data_mut()
                .slow
                .push((code as u32, pc as u64, insn as u64));
            1
        },
    )
    .expect("slow");
    l.func_wrap(
        "rt",
        "lookup",
        |c: Caller<'_, Host>, _: i32, target: i64| -> i32 {
            c.data()
                .slots
                .get(&(target as u64))
                .map_or(MISS_SLOT as i32, |s| *s as i32)
        },
    )
    .expect("lookup");
    l.func_wrap(
        "rt",
        "trap",
        |mut c: Caller<'_, Host>, _: i32, _: i32, code: i32, pc: i64| -> i32 {
            c.data_mut().traps.push((code as u32, pc as u64));
            c.data().trap_rc
        },
    )
    .expect("trap");
    l.func_wrap(
        "rt",
        "undefined",
        |mut c: Caller<'_, Host>, _: i32, _: i32, pc: i64, insn: i64| -> i32 {
            c.data_mut().undefined.push((pc as u64, insn as u64));
            1
        },
    )
    .expect("undefined");
    l.func_wrap("rt", "fma32", |a: f32, b: f32, c: f32| a.mul_add(b, c))
        .expect("fma32");
    l.func_wrap("rt", "fma64", |a: f64, b: f64, c: f64| a.mul_add(b, c))
        .expect("fma64");
    l.func_wrap("rt", "div128", div128).expect("div128");
    l.func_wrap(
        "rt",
        "io_read",
        |mut cx: Caller<'_, Host>, c: i32, _: i32, a: i64, n: i32| -> i64 {
            let memory = cx.data().memory.expect("memory");
            let t = clock(memory.data(&cx), c);
            let model = cx.data_mut().model.as_mut().expect("a device model");
            model.time = t;
            let mut buf = vec![0u8; n as usize];
            model.read(a as u64, &mut buf);
            buf.iter().rev().fold(0u64, |a, b| (a << 8) | u64::from(*b)) as i64
        },
    )
    .expect("io_read");
    l.func_wrap(
        "rt",
        "io_write",
        |mut cx: Caller<'_, Host>, c: i32, _: i32, a: i64, n: i32, v: i64| {
            let memory = cx.data().memory.expect("memory");
            let t = clock(memory.data(&cx), c);
            let model = cx.data_mut().model.as_mut().expect("a device model");
            model.time = t;
            let data: Vec<u8> = (0..n).map(|i| (v as u64 >> (8 * i)) as u8).collect();
            model.write(a as u64, &data);
        },
    )
    .expect("io_write");
    l.func_wrap("rt", "sync", sync).expect("sync");
    for name in intrinsics {
        let n = (*name).to_string();
        match *name {
            "t.hash1" => l
                .func_wrap(
                    "intrinsic",
                    name,
                    move |mut cx: Caller<'_, Host>, _: i32, _: i32, a: i64| -> i64 {
                        cx.data_mut().intrinsic_calls.push((n.clone(), vec![a]));
                        hash_intrinsic(&[u128::from(a as u64)]) as u64 as i64
                    },
                )
                .map(|_| ()),
            "t.hash2" => l
                .func_wrap(
                    "intrinsic",
                    name,
                    move |mut cx: Caller<'_, Host>, _: i32, _: i32, a: i64, b: i64| -> i64 {
                        cx.data_mut().intrinsic_calls.push((n.clone(), vec![a, b]));
                        hash_intrinsic(&[u128::from(a as u64), u128::from(b as u64)]) as u64 as i64
                    },
                )
                .map(|_| ()),
            "t.wide" => l
                .func_wrap(
                    "intrinsic",
                    name,
                    move |mut cx: Caller<'_, Host>,
                          _: i32,
                          _: i32,
                          lo: i64,
                          hi: i64|
                          -> (i64, i64) {
                        cx.data_mut()
                            .intrinsic_calls
                            .push((n.clone(), vec![lo, hi]));
                        let r =
                            hash_intrinsic(
                                &[(u128::from(hi as u64) << 64) | u128::from(lo as u64)],
                            );
                        (r as u64 as i64, (r >> 64) as u64 as i64)
                    },
                )
                .map(|_| ()),
            "t.effect" => l
                .func_wrap(
                    "intrinsic",
                    name,
                    move |mut cx: Caller<'_, Host>, _: i32, _: i32| {
                        cx.data_mut().intrinsic_calls.push((n.clone(), vec![]));
                    },
                )
                .map(|_| ()),
            other => panic!("no test intrinsic {other}"),
        }
        .expect("intrinsic");
    }
    l
}

/// The shared program state: one store, memory and table.
pub struct Rig {
    pub engine: Engine,
    pub store: Store<Host>,
    pub memory: Memory,
    pub table: Table,
    pub linker: Linker<Host>,
}

pub fn engine(tail_calls: bool) -> Engine {
    let mut c = Config::new();
    c.wasm_tail_call(tail_calls);
    Engine::new(&c).expect("engine")
}

impl Rig {
    /// A rig with an engine that has tail calls or not, and `slots` table
    /// slots (the miss entry in slot 0).
    pub fn new(engine: &Engine, slots: u32, regs: RegFile, intrinsics: &[&str]) -> Rig {
        let mut store = Store::new(engine, Host::default());
        let memory = Memory::new(&mut store, MemoryType::new(PAGES as u32, None)).expect("memory");
        let table = Table::new(
            &mut store,
            TableType::new(RefType::FUNCREF, slots.max(FIRST_SLOT), None),
            Ref::Func(None),
        )
        .expect("table");
        let miss = Func::wrap(
            &mut store,
            |mut c: Caller<'_, Host>, cb: i32, _: i32| -> i32 {
                let memory = c.data().memory.expect("memory");
                let pc = ctx_u64(memory.data(&c), cb, ctx::PC);
                c.data_mut().misses.push(pc);
                1
            },
        );
        table
            .set(&mut store, u64::from(MISS_SLOT), Ref::Func(Some(miss)))
            .expect("miss slot");
        store.data_mut().memory = Some(memory);
        store.data_mut().table = Some(table);
        store.data_mut().regs = Some(regs);
        let mut linker = linker(engine, intrinsics);
        linker
            .define(&store, "env", "memory", memory)
            .expect("memory");
        linker.define(&store, "env", "table", table).expect("table");
        Rig {
            engine: engine.clone(),
            store,
            memory,
            table,
            linker,
        }
    }

    /// Compiles and instantiates `out`, registering its slots.
    pub fn load(&mut self, out: &Output) -> Result<Instance, String> {
        let module =
            Module::new(&self.engine, &out.bytes).map_err(|e| format!("compile: {e:#}"))?;
        let inst = self
            .linker
            .instantiate(&mut self.store, &module)
            .map_err(|e| format!("instantiate: {e:#}"))?;
        for (entry, _, slot) in &out.slots {
            self.store.data_mut().slots.insert(*entry, *slot);
        }
        Ok(inst)
    }

    pub fn func(&mut self, inst: &Instance, name: &str) -> TypedFunc<(i32, i32), i32> {
        inst.get_typed_func(&mut self.store, name)
            .expect("an exported lifted function")
    }

    pub fn mem(&mut self) -> &mut [u8] {
        self.memory.data_mut(&mut self.store)
    }

    pub fn guest(&mut self) -> &mut [u8] {
        guest(self.memory.data_mut(&mut self.store))
    }

    /// Calls `f` on context `c`; a trap becomes `Err`.
    pub fn call(&mut self, f: &TypedFunc<(i32, i32), i32>, c: i32) -> Result<i32, String> {
        let rc = f
            .call(&mut self.store, (c, BIAS))
            .map_err(|e| format!("trap: {e:#}"))?;
        drive(&mut self.store, c, BIAS, rc)
    }

    pub fn grow_table(&mut self, to: u32) {
        let have = self.table.size(&self.store);
        if u64::from(to) > have {
            self.table
                .grow(&mut self.store, u64::from(to) - have, Ref::Func(None))
                .expect("grow");
        }
    }
}
