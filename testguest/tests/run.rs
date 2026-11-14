//! Runs small test-guest programs through `relift-ir`'s interpreter: a
//! 32-bit big-endian guest with no ARM concepts.

use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{Flow, Width};
use relift_testguest::{encode, lift, regs};

/// Runs from `pc` until a trap, returning the trap code.
fn run(mem: &mut FlatMemory, state: &mut State, mut pc: u32) -> u32 {
    for _ in 0..10_000 {
        let off = (pc - mem.base as u32) as usize;
        let word = u16::from_be_bytes([mem.bytes[off], mem.bytes[off + 1]]);
        let block = lift(word, pc).expect("defined");
        assert_eq!(block.addr_width, Width::W32);
        let out = interp::run(&block, state, mem, &mut NoIntrinsics).expect("runs");
        if let Flow::Trap(code) = out.flow {
            return code;
        }
        pc = out.next as u32;
    }
    panic!("no trap");
}

fn load(prog: &[u16], data_at: usize, data: &[u8]) -> FlatMemory {
    let mut bytes = vec![0u8; 0x500];
    for (i, w) in prog.iter().enumerate() {
        bytes[2 * i..2 * i + 2].copy_from_slice(&w.to_be_bytes());
    }
    bytes[data_at..data_at + data.len()].copy_from_slice(data);
    FlatMemory { base: 0x400, bytes }
}

#[test]
fn a_counting_loop_sums_and_stores_big_endian() {
    let prog = [
        encode(1, 0, 0, 0),
        encode(1, 1, 0, 5),
        encode(1, 2, 0, 1),
        encode(2, 0, 1, 0),
        encode(3, 1, 2, 0),
        encode(6, 0, 0, (-3i8) as u8),
        encode(1, 3, 0, 0x40),
        encode(2, 3, 3, 0),
        encode(2, 3, 3, 0),
        encode(2, 3, 3, 0),
        encode(2, 3, 3, 0),
        encode(5, 0, 3, 0),
        encode(8, 0, 0, 7),
    ];
    let mut mem = load(&prog, 0, &[]);
    mem.base = 0;
    let mut s = State::new(regs().len());
    let code = run(&mut mem, &mut s, 0);
    assert_eq!(code, 7);
    assert_eq!(s.regs[0], 15);
    assert_eq!(s.regs[3], 0x400);
    assert_eq!(s.regs[4], 0);
    assert_eq!(&mem.bytes[0x400..0x404], &[0, 0, 0, 15]);
}

#[test]
fn loads_are_big_endian_and_jr_is_dynamic() {
    let prog = [
        encode(1, 1, 0, 0x48),
        encode(2, 1, 1, 0),
        encode(4, 0, 1, 0),
        encode(1, 3, 0, 0x5a),
        encode(2, 3, 3, 0),
        encode(7, 0, 3, 0),
        encode(8, 0, 0, 9),
    ];
    let mut mem = load(&prog, 0x90, &[0xde, 0xad, 0xbe, 0xef]);
    mem.base = 0;
    mem.bytes[0xb4..0xb6].copy_from_slice(&encode(8, 0, 0, 3).to_be_bytes());
    let mut s = State::new(regs().len());
    let code = run(&mut mem, &mut s, 0);
    assert_eq!(s.regs[1], 0x90);
    assert_eq!(s.regs[0], 0xdead_beef);
    assert_eq!(code, 3);
}

#[test]
fn undefined_opcodes_do_not_lift() {
    assert!(lift(0x9000, 0).is_none());
    assert!(lift(0xf123, 0).is_none());
}
