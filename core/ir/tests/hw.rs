//! Hardware-register guest support: the guest clock, I/O accesses, sync
//! points and banked code.

use relift_ir::interp::{
    self, Devices, Error, Fault, FlatMemory, GuestMemory, NoIntrinsics, Outcome, State,
};
use relift_ir::{
    AddrClass, Block, Builder, CmpOp, Endian, IoKind, IoMap, IoRange, Op, Order, Reg, RegDecl,
    RegFile, Width, classify_io, dce, opt, parse,
};

static DECLS: [RegDecl; 3] = [
    RegDecl {
        name: "a",
        width: Width::W8,
    },
    RegDecl {
        name: "hl",
        width: Width::W16,
    },
    RegDecl {
        name: "z",
        width: Width::W1,
    },
];

fn file() -> RegFile {
    RegFile::new("hw", &DECLS)
}

#[derive(Debug, PartialEq, Eq)]
enum Event {
    Tick(u64),
    Read(u64, usize),
    Write(u64, Vec<u8>),
    Sync { next: u64, a: u128 },
}

fn gb_map() -> IoMap {
    IoMap {
        ranges: vec![
            IoRange {
                first: 0x0000,
                last: 0x7fff,
                read: AddrClass::Memory,
                write: AddrClass::Device,
            },
            IoRange {
                first: 0xff00,
                last: 0xff7f,
                read: AddrClass::Device,
                write: AddrClass::Device,
            },
            IoRange {
                first: 0xffff,
                last: 0xffff,
                read: AddrClass::Device,
                write: AddrClass::Device,
            },
        ],
        default: AddrClass::Memory,
    }
}

#[derive(Debug)]
struct Log {
    map: IoMap,
    events: Vec<Event>,
    irq: Option<u64>,
}

impl Default for Log {
    fn default() -> Log {
        Log {
            map: gb_map(),
            events: Vec::new(),
            irq: None,
        }
    }
}

impl Devices for Log {
    fn tick(&mut self, cycles: u64) {
        self.events.push(Event::Tick(cycles));
    }

    fn map(&self) -> &IoMap {
        &self.map
    }

    fn io_read(
        &mut self,
        _mem: &mut dyn GuestMemory,
        addr: u64,
        buf: &mut [u8],
    ) -> Result<(), Fault> {
        self.events.push(Event::Read(addr, buf.len()));
        for (i, b) in buf.iter_mut().enumerate() {
            *b = 0xa0 + i as u8;
        }
        Ok(())
    }

    fn io_write(
        &mut self,
        _mem: &mut dyn GuestMemory,
        addr: u64,
        data: &[u8],
    ) -> Result<(), Fault> {
        self.events.push(Event::Write(addr, data.to_vec()));
        Ok(())
    }

    fn sync(
        &mut self,
        state: &mut State,
        mem: &mut dyn GuestMemory,
        next: u64,
    ) -> Result<Option<u64>, Error> {
        self.events.push(Event::Sync {
            next,
            a: state.regs[0],
        });
        let Some(entry) = self.irq.take() else {
            return Ok(None);
        };
        let sp = (state.regs[1] as u64).wrapping_sub(2) & 0xffff;
        mem.write(sp, &(next as u16).to_le_bytes())
            .map_err(Error::Fault)?;
        state.regs[1] = u128::from(sp);
        self.tick(20);
        Ok(Some(entry))
    }
}

fn mem() -> FlatMemory {
    FlatMemory {
        base: 0,
        bytes: vec![0; 0x10000],
    }
}

fn run(b: &Block, regs: &[u128]) -> (Result<Outcome, Error>, State, FlatMemory, Log) {
    run_irq(b, regs, None)
}

fn run_irq(
    b: &Block,
    regs: &[u128],
    irq: Option<u64>,
) -> (Result<Outcome, Error>, State, FlatMemory, Log) {
    let mut s = State::new(DECLS.len());
    s.regs = regs.to_vec();
    let mut m = mem();
    let mut d = Log {
        irq,
        ..Log::default()
    };
    let r = interp::run_with(b, &mut s, &mut m, &mut NoIntrinsics, &mut d);
    (r, s, m, d)
}

fn roundtrip(b: &Block) {
    let text = b.to_string();
    let back = parse(&text, &[file()]).unwrap_or_else(|e| panic!("{e}\n{text}"));
    assert_eq!(&back, b, "{text}");
}

fn branch() -> Block {
    let mut b = Builder::new(file(), Width::W16, 0x150);
    let z = b.get(Reg(2));
    b.tick(8);
    b.tick_if(z, 4);
    b.finish(
        2,
        relift_ir::Exit {
            next: relift_ir::Next::Select {
                cond: z,
                taken: 0x160,
                not_taken: 0x152,
            },
            flow: relift_ir::Flow::Jump,
        },
    )
}

#[test]
fn ticks_reach_the_devices_in_order_and_only_when_taken() {
    let b = branch();
    roundtrip(&b);
    let (r, _, _, d) = run(&b, &[0, 0, 1]);
    assert_eq!(r.unwrap().next, 0x160);
    assert_eq!(d.events, [Event::Tick(8), Event::Tick(4)]);
    let (r, _, _, d) = run(&b, &[0, 0, 0]);
    assert_eq!(r.unwrap().next, 0x152);
    assert_eq!(d.events, [Event::Tick(8)]);
}

#[test]
fn plain_run_ignores_the_clock() {
    let b = branch();
    let mut s = State::new(DECLS.len());
    s.regs[2] = 1;
    let o = interp::run(&b, &mut s, &mut mem(), &mut NoIntrinsics).unwrap();
    assert_eq!(o.next, 0x160);
}

#[test]
fn dce_keeps_ticks() {
    let mut b = Builder::new(file(), Width::W16, 0x100);
    let a = b.get(Reg(0));
    let k = b.konst(Width::W8, 0);
    let c = b.cmp(CmpOp::Eq, a, k);
    let _dead = b.add(a, k);
    b.tick(4);
    b.tick_if(c, 4);
    let block = b.jump(1, 0x101);
    let d = dce(&block);
    assert_eq!(d.insts.len(), block.insts.len() - 1);
    roundtrip(&d);
    for regs in [[0, 0, 0], [3, 0, 0]] {
        let (_, _, _, la) = run(&block, &regs);
        let (_, _, _, lb) = run(&d, &regs);
        assert_eq!(la.events, lb.events);
    }
}

#[test]
fn the_map_classifies_by_direction_and_straddling_goes_to_the_device() {
    let m = gb_map();
    let w = Width::W16;
    assert_eq!(m.classify(0x4000, 1, false, w), AddrClass::Memory);
    assert_eq!(m.classify(0x2000, 1, true, w), AddrClass::Device);
    assert_eq!(m.classify(0xc000, 1, true, w), AddrClass::Memory);
    assert_eq!(m.classify(0xff44, 1, false, w), AddrClass::Device);
    assert_eq!(m.classify(0xfeff, 2, false, w), AddrClass::Device);
    assert_eq!(m.classify(0xfffe, 1, false, w), AddrClass::Memory);
    assert_eq!(m.classify(0x1_ff44, 1, false, w), AddrClass::Device);
    assert_eq!(
        IoMap::default().classify(0xff44, 1, false, w),
        AddrClass::Memory
    );
}

fn io_block(addr: u128, kind: IoKind) -> Block {
    let mut b = Builder::new(file(), Width::W16, 0x200);
    let a = b.konst(Width::W16, addr);
    b.tick(4);
    let v = b.io_load(a, Width::W8, Endian::Little, kind);
    b.tick(4);
    b.io_store(a, v, Endian::Little, kind);
    let wide = b.io_load(a, Width::W16, Endian::Big, kind);
    let lo = b.trunc(wide, Width::W8);
    b.set(Reg(0), lo);
    b.jump(2, 0x202)
}

#[test]
fn io_accesses_route_by_the_map_and_interleave_with_ticks() {
    let b = io_block(0xff44, IoKind::Any);
    roundtrip(&b);
    let (r, s, m, d) = run(&b, &[0, 0, 0]);
    r.unwrap();
    assert_eq!(
        d.events,
        [
            Event::Tick(4),
            Event::Read(0xff44, 1),
            Event::Tick(4),
            Event::Write(0xff44, vec![0xa0]),
            Event::Read(0xff44, 2),
        ]
    );
    assert_eq!(s.regs[0], 0xa1);
    assert_eq!(m.bytes[0xff44], 0);

    let (r, s, m, d) = run(&io_block(0xc000, IoKind::Any), &[0, 0, 0]);
    r.unwrap();
    assert_eq!(d.events, [Event::Tick(4), Event::Tick(4)]);
    assert_eq!(s.regs[0], 0);
    assert_eq!(m.bytes[0xc000], 0);

    let (r, _, _, d) = run(&io_block(0xc000, IoKind::Device), &[0, 0, 0]);
    r.unwrap();
    assert_eq!(d.events.len(), 5);
}

#[test]
fn without_devices_io_is_plain_memory() {
    for kind in [IoKind::Any, IoKind::Device] {
        let b = io_block(0xff44, kind);
        let mut s = State::new(DECLS.len());
        let mut m = mem();
        m.bytes[0xff44] = 0x12;
        m.bytes[0xff45] = 0x34;
        interp::run(&b, &mut s, &mut m, &mut NoIntrinsics).unwrap();
        assert_eq!(s.regs[0], 0x34);
    }
}

#[test]
fn dce_keeps_unused_io_loads() {
    let mut b = Builder::new(file(), Width::W16, 0x100);
    let hl = b.get(Reg(1));
    let _unused = b.io_load(hl, Width::W8, Endian::Little, IoKind::Any);
    let block = b.jump(1, 0x101);
    assert_eq!(dce(&block), block);
}

fn ldh_block() -> Block {
    let mut b = Builder::new(file(), Width::W16, 0x300);
    let hi = b.konst(Width::W8, 0xff);
    let n = b.konst(Width::W8, 0x44);
    let ly = b.concat(hi, n);
    let v = b.io_load(ly, Width::W8, Endian::Little, IoKind::Any);
    let base = b.konst(Width::W16, 0xc000);
    let one = b.konst(Width::W16, 1);
    let wram = b.add(base, one);
    b.io_store(wram, v, Endian::Little, IoKind::Any);
    let rom = b.konst(Width::W16, 0x2000);
    b.io_store(rom, v, Endian::Little, IoKind::Any);
    let rd = b.io_load(rom, Width::W8, Endian::Little, IoKind::Any);
    let hl = b.get(Reg(1));
    let w = b.io_load(hl, Width::W8, Endian::Little, IoKind::Any);
    let edge = b.konst(Width::W16, 0xfeff);
    let e = b.io_load(edge, Width::W16, Endian::Little, IoKind::Any);
    let lo = b.trunc(e, Width::W8);
    let x = b.xor(w, rd);
    let x = b.xor(x, lo);
    b.set(Reg(0), x);
    b.jump(2, 0x302)
}

#[test]
fn constants_fold_pure_operations_only() {
    let b = ldh_block();
    let k = opt::constants(&b);
    assert_eq!(k[2], Some(0xff44));
    assert_eq!(k[6], Some(0xc001));
    assert_eq!(k[3], None);
    assert_eq!(k[11], None);
}

#[test]
fn classify_io_routes_constant_addresses_and_keeps_behavior() {
    let b = ldh_block();
    let c = classify_io(&b, &gb_map());
    c.verify().unwrap();
    roundtrip(&c);
    assert_eq!(c.insts.len(), b.insts.len());
    let kinds: Vec<&Op> = c
        .insts
        .iter()
        .map(|i| &i.op)
        .filter(|o| {
            matches!(
                o,
                Op::Load { .. } | Op::Store { .. } | Op::IoLoad { .. } | Op::IoStore { .. }
            )
        })
        .collect();
    assert!(matches!(
        kinds[0],
        Op::IoLoad {
            kind: IoKind::Device,
            ..
        }
    ));
    assert!(matches!(
        kinds[1],
        Op::Store {
            order: Order::Plain,
            ..
        }
    ));
    assert!(matches!(
        kinds[2],
        Op::IoStore {
            kind: IoKind::Device,
            ..
        }
    ));
    assert!(matches!(kinds[3], Op::Load { .. }));
    assert!(matches!(
        kinds[4],
        Op::IoLoad {
            kind: IoKind::Any,
            ..
        }
    ));
    assert!(matches!(
        kinds[5],
        Op::IoLoad {
            kind: IoKind::Device,
            ..
        }
    ));
    assert_eq!(classify_io(&c, &gb_map()), c);
    for hl in [0xff44, 0xc001, 0x2000, 0x8000] {
        let (ra, sa, ma, la) = run(&b, &[0, hl, 0]);
        let (rb, sb, mb, lb) = run(&c, &[0, hl, 0]);
        assert_eq!(ra, rb);
        assert_eq!(sa, sb);
        assert_eq!(ma.bytes, mb.bytes);
        assert_eq!(la.events, lb.events);
    }
    let all_memory = classify_io(&b, &IoMap::memory());
    assert!(
        !all_memory
            .insts
            .iter()
            .any(|i| matches!(i.op, Op::IoStore { .. }))
    );
}

fn synced() -> Block {
    let mut b = Builder::new(file(), Width::W16, 0x400);
    let k = b.konst(Width::W8, 0x5a);
    b.set(Reg(0), k);
    b.tick(4);
    b.sync();
    b.jump(1, 0x401)
}

#[test]
fn a_sync_without_an_interrupt_sees_committed_state_and_continues() {
    let b = synced();
    roundtrip(&b);
    let (r, s, _, d) = run(&b, &[0, 0xd000, 0]);
    let o = r.unwrap();
    assert_eq!((o.next, o.resume), (0x401, None));
    assert_eq!(s.regs[0], 0x5a);
    assert_eq!(
        d.events,
        [
            Event::Tick(4),
            Event::Sync {
                next: 0x401,
                a: 0x5a
            }
        ]
    );
}

#[test]
fn a_taken_interrupt_redirects_to_the_handler() {
    let (r, s, m, d) = run_irq(&synced(), &[0, 0xd000, 0], Some(0x40));
    let o = r.unwrap();
    assert_eq!((o.next, o.resume), (0x40, Some(0x401)));
    assert_eq!(s.regs[1], 0xcffe);
    assert_eq!(&m.bytes[0xcffe..0xd000], &[0x01, 0x04]);
    assert_eq!(d.events.last(), Some(&Event::Tick(20)));

    let mut s = State::new(DECLS.len());
    let o = interp::run(&synced(), &mut s, &mut mem(), &mut NoIntrinsics).unwrap();
    assert_eq!((o.next, o.resume), (0x401, None));
}

#[test]
fn a_block_without_sync_never_calls_the_hook() {
    let (r, _, _, d) = run_irq(&branch(), &[0, 0, 0], Some(0x40));
    assert_eq!(r.unwrap().resume, None);
    assert!(!d.events.iter().any(|e| matches!(e, Event::Sync { .. })));
}

#[test]
fn sync_must_be_last_and_dce_keeps_it() {
    let text = "block 0x100 len 1 regs hw addr i16\n  sync\n  tick 4\n  exit jump 0x101\n";
    let e = parse(text, &[file()]).expect_err("must not parse");
    assert_eq!((e.line, e.col), (2, 3));
    assert!(e.message.contains("sync"), "{}", e.message);
    let text = "block 0x100 len 1 regs hw addr i16\n  sync\n  sync\n  exit jump 0x101\n";
    assert!(parse(text, &[file()]).is_err());
    let b = synced();
    assert_eq!(dce(&b), b);
}

fn banked(bank: Option<u64>) -> Block {
    let mut b = Builder::new(file(), Width::W16, 0x4000);
    if let Some(n) = bank {
        b.set_bank(n);
    }
    assert_eq!(b.bank(), bank);
    let a = b.get(Reg(0));
    let _dead = b.not(a);
    b.tick(4);
    b.jump(1, 0x4001)
}

#[test]
fn banks_print_parse_and_key_blocks() {
    let one = banked(Some(1));
    let two = banked(Some(0x1ff));
    let none = banked(None);
    for b in [&one, &two, &none] {
        roundtrip(b);
        assert_eq!(dce(b).bank, b.bank);
    }
    assert!(
        one.to_string()
            .starts_with("block 0x4000 len 1 regs hw addr i16 bank 0x1\n")
    );
    assert!(!none.to_string().contains("bank"));
    assert_ne!(one.key(), two.key());
    assert_eq!(one.key(), (0x4000, Some(1)));
    assert_eq!(none.key(), (0x4000, None));

    let text = "block 0x4000 len 1 regs hw bank 0x2\n  exit jump 0x4001\n";
    let b = parse(text, &[file()]).unwrap();
    assert_eq!((b.addr_width, b.bank), (Width::W64, Some(2)));
    let e = parse(
        "block 0x4000 len 1 regs hw bank 2\n  exit jump 0x4001\n",
        &[file()],
    )
    .expect_err("must not parse");
    assert_eq!((e.line, e.col), (1, 33));
    let e = parse(
        "block 0x4000 len 1 regs hw bank 0x2 addr i16\n  exit jump 0x4001\n",
        &[file()],
    )
    .expect_err("must not parse");
    assert_eq!((e.line, e.col), (1, 37));
}

#[test]
fn a_guard_after_a_tick_does_not_verify() {
    let text = "block 0x100 len 1 regs hw addr i16\n  tick 4\n  v1:i1 = get z\n  guard v1, 0x1\n  exit jump 0x101\n";
    let e = parse(text, &[file()]).expect_err("must not parse");
    assert_eq!(e.line, 4);
    assert!(e.message.contains("guard"), "{}", e.message);
}
