//! The Z80 lifter on the IR interpreter: every template lifts to a block
//! that verifies, and known instructions of each kind (load, ALU and flags,
//! block, conditional call and return, ports, prefixes, interrupt entries,
//! `HALT`) give the state, clock, bus accesses and exit that the Zilog
//! manual and the undocumented-behavior notes give.

use std::cell::{Cell, RefCell};

use relift_cpu_z80::{
    ENTRY, Entry, Group, HALF, HALT, IFF, Lifted, Options, PORT_IN, PORT_OUT, SyncPolicy, decode,
    lift_bytes, regs,
};
use relift_ir::interp::{self, Devices, Error, Fault, FlatMemory, GuestMemory, Intrinsics, State};
use relift_ir::{Flow, IoMap, Next, Op, Reg};

fn opts() -> Options {
    Options::new(IoMap::memory())
}

/// A port access: T-state, `i` or `o`, port and byte.
type Port = (u64, char, u16, u8);

/// The machine around the CPU: a clock, and the port accesses and
/// intrinsics seen. Memory is all RAM.
#[derive(Default)]
struct Machine {
    clock: Cell<u64>,
    ports: RefCell<Vec<Port>>,
    calls: RefCell<Vec<&'static str>>,
}

struct Dev<'a>(&'a Machine);

impl Devices for Dev<'_> {
    fn tick(&mut self, cycles: u64) {
        self.0.clock.set(self.0.clock.get() + cycles);
    }

    fn map(&self) -> &IoMap {
        static MAP: IoMap = IoMap::memory();
        &MAP
    }

    fn io_read(&mut self, _m: &mut dyn GuestMemory, addr: u64, _b: &mut [u8]) -> Result<(), Fault> {
        Err(Fault {
            addr,
            size: 1,
            write: false,
        })
    }

    fn io_write(&mut self, _m: &mut dyn GuestMemory, addr: u64, _d: &[u8]) -> Result<(), Fault> {
        Err(Fault {
            addr,
            size: 1,
            write: true,
        })
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

/// The byte every port read gives.
const PORT_VALUE: u8 = 0x5A;

struct Calls<'a>(&'a Machine);

impl Intrinsics for Calls<'_> {
    fn call(&mut self, name: &'static str, args: &[u128]) -> Result<u128, Error> {
        let m = self.0;
        m.calls.borrow_mut().push(name);
        let t = m.clock.get();
        match name {
            PORT_IN => {
                m.ports
                    .borrow_mut()
                    .push((t, 'i', args[0] as u16, PORT_VALUE));
                Ok(u128::from(PORT_VALUE))
            }
            PORT_OUT => {
                m.ports
                    .borrow_mut()
                    .push((t, 'o', args[0] as u16, args[1] as u8));
                Ok(0)
            }
            HALT => Ok(args[1]),
            _ => Ok(0),
        }
    }
}

struct Cpu {
    st: State,
    mem: FlatMemory,
    m: Machine,
}

impl Cpu {
    fn new(code: &[u8]) -> Cpu {
        let mut bytes = vec![0u8; 0x10000];
        bytes[..code.len()].copy_from_slice(code);
        Cpu {
            st: State::new(regs::DECLS.len()),
            mem: FlatMemory { base: 0, bytes },
            m: Machine::default(),
        }
    }

    fn set(&mut self, r: Reg, v: u128) {
        self.st.regs[r.index()] = v;
    }

    fn get(&self, r: Reg) -> u128 {
        self.st.regs[r.index()]
    }

    fn clock(&self) -> u64 {
        self.m.clock.get()
    }

    /// Runs the block `l` and gives its exit.
    fn run(&mut self, l: &Lifted) -> u64 {
        interp::run_verified(
            &l.block,
            &mut self.st,
            &mut self.mem,
            &mut Calls(&self.m),
            &mut Dev(&self.m),
        )
        .expect("runs")
        .next
    }

    /// Lifts and runs the instruction at `pc` (with its taken continuation)
    /// and gives the next pc.
    fn step(&mut self, pc: u16) -> u64 {
        let mut at = u64::from(pc);
        loop {
            let a = usize::from(pc);
            let bytes: Vec<u8> = (0..4).map(|i| self.mem.bytes[(a + i) & 0xFFFF]).collect();
            let l = lift_bytes(&bytes, at, &opts()).expect("lifts");
            let next = self.run(&l);
            if next == HALF | u64::from(pc) && at != next {
                at = next;
                continue;
            }
            return next;
        }
    }

    /// Takes the interrupt `e` returning to `ret` and gives the handler's
    /// address.
    fn take(&mut self, e: Entry, ret: u16) -> u64 {
        self.set(regs::PC, u128::from(ret));
        let l = lift_bytes(&[], e.addr(), &opts()).expect("lifts the entry");
        assert!(l.block.insts.iter().any(|i| matches!(i.op, Op::Sync)));
        self.run(&l)
    }

    fn f(&self) -> u8 {
        regs::F_BITS
            .iter()
            .fold(0u8, |acc, r| (acc << 1) | self.get(*r) as u8)
    }
}

#[test]
fn every_template_lifts() {
    let mut n = 0;
    let mut check = |bytes: &[u8]| {
        let l = lift_bytes(bytes, 0x4000, &opts()).expect("lifts");
        l.block.verify().expect("verifies");
        assert!((1..=4).contains(&l.len), "{bytes:02x?}");
        if let Next::Select { taken, .. } = l.block.exit.next
            && taken == HALF | 0x4000
        {
            let t = lift_bytes(bytes, taken, &opts()).expect("lifts the taken half");
            t.block.verify().expect("verifies");
        }
        n += 1;
    };
    for op in 0..=255u8 {
        let rest = [op, 0x12, 0x34, 0x56];
        check(&rest);
        for p in [0xCB, 0xED, 0xDD, 0xFD] {
            check(&[p, op, 0x12, 0x34]);
            if p != 0xCB && p != 0xED {
                check(&[p, 0xCB, 0x05, op]);
            }
        }
    }
    for e in [Entry::Nmi, Entry::Im0, Entry::Im1, Entry::Im2] {
        let l = lift_bytes(&[], e.addr(), &opts()).expect("lifts the entry");
        l.block.verify().expect("verifies");
        assert_eq!(l.len, 0);
        assert_eq!(l.block.exit.flow, Flow::Jump);
    }
    assert_eq!(n, 256 * 7);
    assert!(lift_bytes(&[], ENTRY + 4, &opts()).is_err());
    assert!(lift_bytes(&[0x00], HALF, &opts()).is_err());
}

#[test]
fn lone_prefix() {
    assert_eq!(decode(&[0xDD, 0xDD, 0x21]), Ok((Group::Lone, 0xDD)));
    let mut cpu = Cpu::new(&[0xDD, 0xFD, 0x36, 0xFE, 0x42]);
    cpu.set(regs::IY, 0x8002);
    assert_eq!(cpu.step(0), 1);
    assert_eq!(cpu.clock(), 4);
    assert_eq!(cpu.get(regs::R), 1);
    assert_eq!(cpu.step(1), 5);
    assert_eq!(cpu.mem.bytes[0x8000], 0x42);
    assert_eq!(cpu.clock(), 4 + 19);
    assert_eq!(cpu.get(regs::R), 3);
}

#[test]
fn load_and_add() {
    let mut cpu = Cpu::new(&[0x3E, 0x7F, 0x80]);
    cpu.set(regs::B, 1);
    assert_eq!(cpu.step(0), 2);
    assert_eq!(cpu.get(regs::A), 0x7F);
    assert_eq!(cpu.clock(), 7);
    assert_eq!(cpu.step(2), 3);
    assert_eq!(cpu.get(regs::A), 0x80);
    assert_eq!(cpu.f(), 0b1001_0100, "S, H and P/V (overflow) set");
    assert_eq!(cpu.clock(), 11);
    assert_eq!(cpu.get(regs::R), 2);
    assert_eq!(cpu.get(regs::Q), 1);
}

#[test]
fn ldir_repeats() {
    let mut cpu = Cpu::new(&[0xED, 0xB0]);
    cpu.mem.bytes[0x8000] = 0xAA;
    cpu.mem.bytes[0x8001] = 0xBB;
    cpu.set(regs::H, 0x80);
    cpu.set(regs::D, 0x90);
    cpu.set(regs::C, 2);
    assert_eq!(cpu.step(0), 0, "repeats while BC is not zero");
    assert_eq!(cpu.clock(), 21);
    assert_eq!(cpu.step(0), 2);
    assert_eq!(cpu.clock(), 37);
    assert_eq!(&cpu.mem.bytes[0x9000..0x9002], &[0xAA, 0xBB]);
    assert_eq!((cpu.get(regs::B), cpu.get(regs::C)), (0, 0));
    assert_eq!(cpu.get(regs::FP), 0);
}

#[test]
fn conditional_call_and_return() {
    let mut cpu = Cpu::new(&[0xCC, 0x00, 0x30]);
    cpu.mem.bytes[0x3000] = 0xC8;
    cpu.set(regs::SP, 0xFFF0);
    let l = lift_bytes(&cpu.mem.bytes[0..4], 0, &opts()).expect("lifts");
    assert!(matches!(l.block.exit.next, Next::Select { taken, not_taken: 3, .. } if taken == HALF));
    assert_eq!(cpu.step(0), 3, "Z clear: not taken");
    assert_eq!(cpu.clock(), 10);
    cpu.set(regs::FZ, 1);
    assert_eq!(cpu.step(0), 0x3000);
    assert_eq!(cpu.clock(), 10 + 17);
    assert_eq!(cpu.get(regs::SP), 0xFFEE);
    assert_eq!(&cpu.mem.bytes[0xFFEE..0xFFF0], &[0x03, 0x00]);
    assert_eq!(cpu.step(0x3000), 3, "RET Z taken");
    assert_eq!(cpu.clock(), 27 + 11);
    assert_eq!(cpu.get(regs::SP), 0xFFF0);
    let half = lift_bytes(&[0xC8], HALF | 0x3000, &opts()).expect("lifts");
    assert_eq!(half.block.exit.flow, Flow::Return);
}

#[test]
fn ports_at_their_t_state() {
    let mut cpu = Cpu::new(&[0xDB, 0xBF, 0xD3, 0x7F]);
    cpu.set(regs::A, 0x12);
    cpu.step(0);
    cpu.step(2);
    assert_eq!(
        *cpu.m.ports.borrow(),
        vec![(7, 'i', 0x12BF, PORT_VALUE), (18, 'o', 0x5A7F, PORT_VALUE)]
    );
    assert_eq!(cpu.get(regs::A), u128::from(PORT_VALUE));
    assert_eq!(cpu.clock(), 22);
}

#[test]
fn interrupt_entries() {
    let mut cpu = Cpu::new(&[]);
    cpu.set(regs::SP, 0xD000);
    cpu.set(regs::IFF1, 1);
    cpu.set(regs::IFF2, 1);
    assert_eq!(cpu.take(Entry::Im1, 0x1234), 0x38);
    assert_eq!(cpu.clock(), 13);
    assert_eq!(cpu.get(regs::SP), 0xCFFE);
    assert_eq!(&cpu.mem.bytes[0xCFFE..0xD000], &[0x34, 0x12]);
    assert_eq!((cpu.get(regs::IFF1), cpu.get(regs::IFF2)), (0, 0));
    assert_eq!(cpu.get(regs::R), 1);

    cpu.set(regs::I, 0x40);
    cpu.mem.bytes[0x40FF] = 0x00;
    cpu.mem.bytes[0x4100] = 0x80;
    assert_eq!(cpu.take(Entry::Im2, 0x0038), 0x8000);
    assert_eq!(cpu.clock(), 13 + 19);

    cpu.set(regs::IFF1, 1);
    cpu.set(regs::IFF2, 1);
    assert_eq!(cpu.take(Entry::Nmi, 0x0100), 0x66);
    assert_eq!(cpu.clock(), 32 + 11);
    assert_eq!((cpu.get(regs::IFF1), cpu.get(regs::IFF2)), (0, 1));
}

#[test]
fn halt_calls_the_cpu_layer() {
    let mut cpu = Cpu::new(&[0x76]);
    assert_eq!(cpu.step(0), 1);
    assert_eq!(*cpu.m.calls.borrow(), vec![HALT]);
    assert_eq!(cpu.clock(), 4);
    assert_eq!(cpu.get(regs::R), 1);
    let l = lift_bytes(&[0x76], 0, &opts()).expect("lifts");
    assert!(matches!(
        l.block.insts.last().map(|i| &i.op),
        Some(Op::Sync)
    ));
}

#[test]
fn iff1_changes_tell_the_cpu_layer_before_the_sync() {
    for (bytes, value) in [(&[0xFB][..], 1), (&[0xF3][..], 0)] {
        let mut cpu = Cpu::new(bytes);
        cpu.step(0);
        assert_eq!(*cpu.m.calls.borrow(), vec![IFF]);
        assert_eq!(cpu.get(regs::IFF1), value);
        let l = lift_bytes(bytes, 0, &opts()).expect("lifts");
        let n = l.block.insts.len();
        assert!(matches!(l.block.insts[n - 1].op, Op::Sync));
        assert!(matches!(&l.block.insts[n - 2].op, Op::Intrinsic(c) if c.name == IFF));
    }
}

#[test]
fn sync_policy() {
    let has_sync = |bytes: &[u8], sync| {
        let o = Options {
            map: IoMap::memory(),
            sync,
            live_operands: false,
        };
        lift_bytes(bytes, 0, &o)
            .expect("lifts")
            .block
            .insts
            .iter()
            .any(|i| matches!(i.op, Op::Sync))
    };
    assert!(!has_sync(&[0x3C], SyncPolicy::IoStores));
    assert!(has_sync(&[0x3C], SyncPolicy::Every));
    assert!(
        has_sync(&[0xD3, 0x7F], SyncPolicy::IoStores),
        "a port write"
    );
    assert!(!has_sync(&[0xD3, 0x7F], SyncPolicy::Blocks));
    assert!(has_sync(&[0xFB], SyncPolicy::Blocks), "EI sets IFF1");
    assert!(has_sync(&[0xC3, 0, 0], SyncPolicy::Blocks), "a jump");
    assert!(
        !has_sync(&[0xC8], SyncPolicy::Every),
        "the decision half of RET Z"
    );
}

/// Runs every opcode of every page once with operands taken from the bytes
/// lifted and once lifted with live operands, from the same random state
/// and memory: registers, memory, clock, port accesses and exits agree.
/// Then the live form of each instruction with operands runs with other
/// operand bytes in memory and must agree with the fixed form lifted from
/// those bytes.
#[test]
fn live_operands_read_the_bytes_in_memory() {
    let live = Options {
        live_operands: true,
        ..opts()
    };
    let mut seed = 0x1234_5678_9ABC_DEF0u64;
    let mut rnd = move || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };
    let mut forms = Vec::new();
    for op in 0..=255u8 {
        forms.push(vec![op]);
        for p in [0xCB, 0xED, 0xDD, 0xFD] {
            forms.push(vec![p, op]);
        }
        for p in [0xDD, 0xFD] {
            forms.push(vec![p, 0xCB, 0, op]);
        }
    }
    let pc = 0x8100u16;
    let mut compared = 0;
    let mut differs = 0;
    for form in forms {
        for round in 0..2 {
            let mut lifted = [0u8; 4];
            let mut later = [0u8; 4];
            for k in 0..4 {
                lifted[k] = rnd() as u8;
                later[k] = rnd() as u8;
            }
            for (k, b) in form.iter().enumerate() {
                let at = if form.len() == 4 && k == 3 { 3 } else { k };
                lifted[at] = *b;
                later[at] = *b;
            }
            let Ok(mask) = relift_cpu_z80::opcode_mask(&lifted) else {
                continue;
            };
            for k in 0..4 {
                if mask & (1 << k) != 0 {
                    later[k] = lifted[k];
                }
            }
            let in_mem = if round == 0 { lifted } else { later };
            let fixed = lift_bytes(&in_mem, u64::from(pc), &opts()).expect("lifts");
            let l = lift_bytes(&lifted, u64::from(pc), &live).expect("lifts live");
            if fixed.len != l.len {
                continue;
            }
            let mut regs = vec![0u128; regs::DECLS.len()];
            for (i, d) in regs::DECLS.iter().enumerate() {
                regs[i] = u128::from(rnd()) & d.width.mask();
            }
            let mut mem = vec![0u8; 0x10000];
            for b in mem.iter_mut() {
                *b = rnd() as u8;
            }
            mem[usize::from(pc)..usize::from(pc) + 4].copy_from_slice(&in_mem);
            let run = |l: &Lifted| {
                let mut c = Cpu::new(&[]);
                c.st.regs = regs.clone();
                c.mem.bytes = mem.clone();
                let next = c.run(l);
                let ports = c.m.ports.borrow().clone();
                let clock = c.clock();
                (next, c.st.regs.clone(), c.mem.bytes, clock, ports)
            };
            let a = run(&fixed);
            let b = run(&l);
            assert!(a == b, "{in_mem:02x?} lifted from {lifted:02x?}");
            compared += 1;
            if in_mem != lifted {
                differs += 1;
            }
        }
    }
    assert!(compared > 3000, "{compared}");
    assert!(differs > 1000, "{differs}");
}
