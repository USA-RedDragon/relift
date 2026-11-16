//! The machine around the lifted Z80: a flat 64 KiB of RAM, a clock in
//! T-states, a log of every bus access and a list of port reads. Each
//! instruction is lifted with `relift_cpu_z80::lift_bytes` when it is reached
//! and run on the IR interpreter.

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

use relift_cpu_z80::{Entry, HALF, HALT, Options, PORT_IN, PORT_OUT, lift_bytes, regs};
use relift_ir::interp::{self, Devices, Error, Fault, GuestMemory, Intrinsics, State};
use relift_ir::{AddrClass, Block, IoMap, Reg};

use super::json::J;
use super::mutate::Mutation;

/// One bus access: the clock at the start of its machine cycle, the address,
/// the byte and the kind (`f` fetch, `r` read, `w` write, `i` in, `o` out).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Acc {
    pub t: u64,
    pub addr: u16,
    pub val: u8,
    pub kind: u8,
    /// An M1 (opcode) fetch.
    pub m1: bool,
}

pub struct Bus {
    pub mem: Vec<u8>,
    pub clock: u64,
    pub log: Vec<Acc>,
    pub logging: bool,
    /// Port reads to give: port, byte, used.
    pub ports_in: Vec<(u16, u8, bool)>,
}

impl Bus {
    fn new() -> Bus {
        Bus {
            mem: vec![0; 0x10000],
            clock: 0,
            log: Vec::new(),
            logging: true,
            ports_in: Vec::new(),
        }
    }

    fn push(&mut self, addr: u16, val: u8, kind: u8) {
        if self.logging {
            let t = self.clock;
            self.log.push(Acc {
                t,
                addr,
                val,
                kind,
                m1: false,
            });
        }
    }
}

struct Dev<'a> {
    bus: &'a RefCell<Bus>,
    map: &'a IoMap,
}

struct Intr<'a>(&'a RefCell<Bus>);

struct NoMem;

impl GuestMemory for NoMem {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        Err(Fault {
            addr,
            size: buf.len() as u32,
            write: false,
        })
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        Err(Fault {
            addr,
            size: data.len() as u32,
            write: true,
        })
    }
}

impl Devices for Dev<'_> {
    fn tick(&mut self, cycles: u64) {
        self.bus.borrow_mut().clock += cycles;
    }

    fn map(&self) -> &IoMap {
        self.map
    }

    fn io_read(
        &mut self,
        _m: &mut dyn GuestMemory,
        addr: u64,
        buf: &mut [u8],
    ) -> Result<(), Fault> {
        let mut b = self.bus.borrow_mut();
        let a = addr as u16;
        buf[0] = b.mem[usize::from(a)];
        let v = buf[0];
        b.push(a, v, b'r');
        Ok(())
    }

    fn io_write(&mut self, _m: &mut dyn GuestMemory, addr: u64, data: &[u8]) -> Result<(), Fault> {
        let mut b = self.bus.borrow_mut();
        let a = addr as u16;
        b.mem[usize::from(a)] = data[0];
        b.push(a, data[0], b'w');
        Ok(())
    }

    fn sync(
        &mut self,
        _s: &mut State,
        _m: &mut dyn GuestMemory,
        _n: u64,
    ) -> Result<Option<u64>, Error> {
        Ok(None)
    }
}

impl Intrinsics for Intr<'_> {
    fn call(&mut self, name: &'static str, args: &[u128]) -> Result<u128, Error> {
        let mut b = self.0.borrow_mut();
        match name {
            PORT_IN => {
                let port = args[0] as u16;
                let mut v = 0xFF;
                if let Some(p) = b.ports_in.iter_mut().find(|p| !p.2 && p.0 == port) {
                    p.2 = true;
                    v = p.1;
                }
                b.push(port, v, b'i');
                Ok(u128::from(v))
            }
            PORT_OUT => {
                b.push(args[0] as u16, args[1] as u8, b'o');
                Ok(0)
            }
            HALT => Ok(0),
            _ => Err(Error::UnknownIntrinsic(name)),
        }
    }
}

/// A lifted block with the fetches its template recorded.
pub struct Unit {
    block: Block,
    fetches: Vec<(u16, u32)>,
}

pub struct Cpu {
    pub st: State,
    pub pc: u16,
    pub bus: RefCell<Bus>,
    map: IoMap,
    opts: Options,
    cache: HashMap<(u64, [u8; 4]), Rc<Unit>>,
    pub use_cache: bool,
    pub mutation: Option<Mutation>,
}

/// The F bit slots, bit 0 first.
const F_LOW_FIRST: [Reg; 8] = [
    regs::FC,
    regs::FN,
    regs::FP,
    regs::FX,
    regs::FH,
    regs::FY,
    regs::FZ,
    regs::FS,
];

/// SST's names of the plain registers and the slots that hold them.
const PLAIN: [(&str, Reg); 22] = [
    ("a", regs::A),
    ("b", regs::B),
    ("c", regs::C),
    ("d", regs::D),
    ("e", regs::E),
    ("h", regs::H),
    ("l", regs::L),
    ("i", regs::I),
    ("r", regs::R),
    ("sp", regs::SP),
    ("wz", regs::WZ),
    ("ix", regs::IX),
    ("iy", regs::IY),
    ("af_", regs::AF_),
    ("bc_", regs::BC_),
    ("de_", regs::DE_),
    ("hl_", regs::HL_),
    ("im", regs::IM),
    ("iff1", regs::IFF1),
    ("iff2", regs::IFF2),
    ("ei", regs::EIDELAY),
    ("p", regs::LDAIR),
];

impl Cpu {
    pub fn new() -> Cpu {
        let map = IoMap {
            ranges: Vec::new(),
            default: AddrClass::Device,
        };
        Cpu {
            st: State::new(regs::DECLS.len()),
            pc: 0,
            bus: RefCell::new(Bus::new()),
            opts: Options::new(map.clone()),
            map,
            cache: HashMap::new(),
            use_cache: false,
            mutation: None,
        }
    }

    pub fn distinct_blocks(&self) -> usize {
        self.cache.len()
    }

    fn unit(&mut self, bytes: [u8; 4], addr: u64) -> Result<Rc<Unit>, String> {
        let key = (addr, bytes);
        if self.use_cache
            && let Some(u) = self.cache.get(&key)
        {
            return Ok(u.clone());
        }
        let l = lift_bytes(&bytes, addr, &self.opts).map_err(|e| e.to_string())?;
        let mut block = l.block;
        if let Some(m) = self.mutation {
            m.apply(&mut block);
        }
        let u = Rc::new(Unit {
            block,
            fetches: l.fetches,
        });
        if self.use_cache {
            self.cache.insert(key, u.clone());
        }
        Ok(u)
    }

    fn run(&mut self, u: &Unit) -> Result<u64, String> {
        let mut dev = Dev {
            bus: &self.bus,
            map: &self.map,
        };
        let mut intr = Intr(&self.bus);
        interp::run_verified(&u.block, &mut self.st, &mut NoMem, &mut intr, &mut dev)
            .map(|o| o.next)
            .map_err(|e| e.to_string())
    }

    fn log_fetches(&self, u: &Unit, t0: u64, m1_bytes: u16) {
        let mut b = self.bus.borrow_mut();
        if !b.logging {
            return;
        }
        for &(off, t) in &u.fetches {
            let addr = self.pc.wrapping_add(off);
            let val = b.mem[usize::from(addr)];
            b.log.push(Acc {
                t: t0 + u64::from(t),
                addr,
                val,
                kind: b'f',
                m1: off < m1_bytes,
            });
        }
    }

    /// Runs one instruction.
    pub fn step(&mut self) -> Result<(), String> {
        let (bytes, t0) = {
            let b = self.bus.borrow();
            let at = |o: u16| b.mem[usize::from(self.pc.wrapping_add(o))];
            ([at(0), at(1), at(2), at(3)], b.clock)
        };
        let m1_bytes = if matches!(bytes[0], 0xCB | 0xED | 0xDD | 0xFD) {
            2
        } else {
            1
        };
        let u = self.unit(bytes, u64::from(self.pc))?;
        self.log_fetches(&u, t0, m1_bytes);
        let mut next = self.run(&u)?;
        if next & HALF != 0 {
            let u2 = self.unit(bytes, next)?;
            self.log_fetches(&u2, t0, m1_bytes);
            next = self.run(&u2)?;
        }
        self.pc = next as u16;
        Ok(())
    }

    /// Takes an interrupt after an instruction: NMI, IM 0, IM 1 or IM 2,
    /// each with 0xFF on the data bus.
    pub fn accept(&mut self, entry: Entry) -> Result<(), String> {
        self.set(regs::PC, u64::from(self.pc));
        let u = self.unit([0; 4], entry.addr())?;
        self.pc = self.run(&u)? as u16;
        Ok(())
    }

    pub fn r(&self, r: Reg) -> u64 {
        self.st.regs[r.index()] as u64
    }

    pub fn set(&mut self, r: Reg, v: u64) {
        let w = regs::DECLS[r.index()].width;
        self.st.regs[r.index()] = u128::from(v) & w.mask();
    }

    /// F as the eight flag slots build it.
    pub fn f(&self) -> u64 {
        F_LOW_FIRST
            .iter()
            .enumerate()
            .fold(0, |f, (i, r)| f | (self.r(*r) & 1) << i)
    }

    fn set_f(&mut self, f: u64) {
        for (i, r) in F_LOW_FIRST.iter().enumerate() {
            self.set(*r, f >> i & 1);
        }
    }

    /// Loads an SST-style state object: registers and RAM (all else zero).
    pub fn load(&mut self, s: &J) {
        let g = |k: &str| s.get(k).map_or(0, J::num);
        self.pc = g("pc") as u16;
        for (k, r) in PLAIN {
            self.set(r, g(k));
        }
        self.set_f(g("f"));
        self.set(regs::Q, u64::from(g("q") != 0));
        let mut b = self.bus.borrow_mut();
        b.mem.fill(0);
        if let Some(J::A(ram)) = s.get("ram") {
            for e in ram {
                b.mem[e.idx(0).num() as usize & 0xFFFF] = e.idx(1).num() as u8;
            }
        }
    }

    /// Clears the log, the clock and the port reads.
    pub fn reset_bus(&self, ports: &[(u16, u8, bool)]) {
        let mut b = self.bus.borrow_mut();
        b.log.clear();
        b.clock = 0;
        b.ports_in = ports.to_vec();
    }

    /// The state as SST names it.
    pub fn dump(&self) -> Vec<(&'static str, u64)> {
        let mut v = vec![("pc", u64::from(self.pc)), ("f", self.f())];
        for (k, r) in PLAIN {
            v.push((k, self.r(r)));
        }
        v.push(("q", if self.r(regs::Q) != 0 { self.f() } else { 0 }));
        v
    }
}
