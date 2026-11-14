//! Hardware-register guest support: the guest clock.

use relift_ir::interp::{self, Devices, Error, FlatMemory, NoIntrinsics, Outcome, State};
use relift_ir::{Block, Builder, CmpOp, Reg, RegDecl, RegFile, Width, dce, parse};

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
}

#[derive(Debug, Default)]
struct Log {
    events: Vec<Event>,
}

impl Devices for Log {
    fn tick(&mut self, cycles: u64) {
        self.events.push(Event::Tick(cycles));
    }
}

fn mem() -> FlatMemory {
    FlatMemory {
        base: 0,
        bytes: vec![0; 0x10000],
    }
}

fn run(b: &Block, regs: &[u128]) -> (Result<Outcome, Error>, State, FlatMemory, Log) {
    let mut s = State::new(DECLS.len());
    s.regs = regs.to_vec();
    let mut m = mem();
    let mut d = Log::default();
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
fn a_guard_after_a_tick_does_not_verify() {
    let text = "block 0x100 len 1 regs hw addr i16\n  tick 4\n  v1:i1 = get z\n  guard v1, 0x1\n  exit jump 0x101\n";
    let e = parse(text, &[file()]).expect_err("must not parse");
    assert_eq!(e.line, 4);
    assert!(e.message.contains("guard"), "{}", e.message);
}
