//! The ARM9's internal cycles per instruction, from the console's
//! measurements (DS mode on a 3DS and on a DSi, which agree): what an
//! instruction takes beyond its own cycle and its memory accesses.
//!
//! - MUL and MLA take 2 cycles, MULS and MLAS 4, the long multiplies 3 and
//!   their S forms 5, SMLALxy 2, the other halfword multiplies 1, Thumb MULS
//!   4, MRS 2; no operand value changes them;
//! - LDR and STR take their own cycle only (the memory side charges the
//!   access); a single-register LDM or STM takes 2, and a load of the PC 2
//!   more than the branch refill;
//! - a load or multiply whose result the next instruction reads stalls it:
//!   1 cycle after LDR, LDM (its last register) and the multiplies, 2 after
//!   LDRB, LDRH, LDRSB and LDRSH;
//! - a load right after a store waits a cycle (STR then LDR in the data TCM
//!   or the data cache, Thumb PUSH then POP).
//!
//! Other instructions keep the lifter's ARMv5TE internal cycles.

use relift_cpu_arm32::decode::Profile;
use relift_cpu_arm32::lift;
use relift_cpu_arm32::regs;
use relift_ir::Op;

/// A result the instruction leaves for the next one: its register and the
/// cycles a read of it right after stalls.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Result {
    reg: u32,
    stall: u32,
}

/// Cycles and the stalling result of an ARM instruction; `None` keeps the
/// lifter's cycles.
fn arm(w: u32) -> Option<(u32, Option<Result>)> {
    let rd = (w >> 12) & 15;
    let rn = (w >> 16) & 15;
    if w >> 28 == 15 {
        return None;
    }
    if w & 0x0FC0_00F0 == 0x0000_0090 {
        let s = w & 1 << 20 != 0;
        return Some((if s { 3 } else { 1 }, Some(Result { reg: rn, stall: 1 })));
    }
    if w & 0x0F80_00F0 == 0x0080_0090 {
        let s = w & 1 << 20 != 0;
        return Some((if s { 4 } else { 2 }, Some(Result { reg: rn, stall: 1 })));
    }
    if w & 0x0FB0_0FF0 == 0x0100_0090 {
        return Some((1, None));
    }
    if w & 0x0F90_0090 == 0x0100_0080 {
        let op = (w >> 21) & 3;
        let dst = if op == 2 { rd } else { rn };
        return Some((u32::from(op == 2), Some(Result { reg: dst, stall: 1 })));
    }
    if w & 0x0FBF_0FFF == 0x010F_0000 {
        return Some((1, None));
    }
    if w & 0x0C00_0000 == 0x0400_0000 {
        let load = w & 1 << 20 != 0;
        if !load {
            return Some((0, None));
        }
        if rd == 15 {
            return Some((2, None));
        }
        let stall = if w & 1 << 22 != 0 { 2 } else { 1 };
        return Some((0, Some(Result { reg: rd, stall })));
    }
    if w & 0x0E00_0090 == 0x0000_0090 && w & 0x60 != 0 {
        let narrow = w & 1 << 20 != 0;
        return Some((0, narrow.then_some(Result { reg: rd, stall: 2 })));
    }
    if w & 0x0E00_0000 == 0x0800_0000 {
        let list = w & 0xFFFF;
        let n = list.count_ones();
        let load = w & 1 << 20 != 0;
        if load && list & 1 << 15 != 0 {
            return Some((if n == 1 { 3 } else { 2 }, None));
        }
        let last = (load && list != 0).then(|| Result {
            reg: 31 - list.leading_zeros(),
            stall: 1,
        });
        return Some((u32::from(n == 1), last));
    }
    None
}

/// Cycles and the stalling result of a Thumb instruction; `None` keeps the
/// lifter's cycles.
fn thumb(h: u32) -> Option<(u32, Option<Result>)> {
    let low = h & 7;
    let high = (h >> 8) & 7;
    let word = |reg| Some((0, Some(Result { reg, stall: 1 })));
    let narrow = |reg| Some((0, Some(Result { reg, stall: 2 })));
    match h >> 11 {
        0b01001 | 0b10011 => return word(high),
        0b01101 => return word(low),
        0b01111 | 0b10001 => return narrow(low),
        0b01100 | 0b01110 | 0b10000 | 0b10010 => return Some((0, None)),
        0b01010 => return Some((0, None)),
        0b01011 => {
            return if (h >> 9) & 3 == 0 {
                word(low)
            } else {
                narrow(low)
            };
        }
        0b11000 => return Some((u32::from((h & 0xFF).count_ones() == 1), None)),
        0b11001 => {
            let list = h & 0xFF;
            let last = (list != 0).then(|| Result {
                reg: 31 - list.leading_zeros(),
                stall: 1,
            });
            return Some((u32::from(list.count_ones() == 1), last));
        }
        _ => {}
    }
    if h & 0xFFC0 == 0x4340 {
        return Some((3, Some(Result { reg: low, stall: 1 })));
    }
    if h & 0xFE00 == 0xBC00 {
        let list = h & 0xFF;
        if h & 0x100 != 0 {
            return Some((if list == 0 { 3 } else { 2 }, None));
        }
        let last = (list != 0).then(|| Result {
            reg: 31 - list.leading_zeros(),
            stall: 1,
        });
        return Some((u32::from(list.count_ones() == 1), last));
    }
    if h & 0xFE00 == 0xB400 {
        let n = (h & 0xFF).count_ones() + (h >> 8 & 1);
        return Some((u32::from(n == 1), None));
    }
    None
}

/// Whether `w` (ARM or Thumb) stores to memory.
fn stores(w: u32, is_thumb: bool) -> bool {
    if is_thumb {
        return matches!(w >> 11, 0b01100 | 0b01110 | 0b10000 | 0b10010 | 0b11000)
            || (w >> 12 == 0b0101 && (w >> 9) & 7 < 3)
            || w & 0xFE00 == 0xB400;
    }
    w >> 28 != 15
        && (w & 0x0C10_0000 == 0x0400_0000
            || w & 0x0E10_0000 == 0x0800_0000
            || (w & 0x0E10_00F0 == 0x0000_00B0)
            || w & 0x0E10_00F0 == 0x0000_00F0)
}

/// Whether `w` (ARM or Thumb) loads from memory.
fn loads(w: u32, is_thumb: bool) -> bool {
    if is_thumb {
        return matches!(
            w >> 11,
            0b01001 | 0b01101 | 0b01111 | 0b10001 | 0b10011 | 0b11001
        ) || (w >> 12 == 0b0101 && (w >> 9) & 7 >= 3)
            || w & 0xFE00 == 0xBC00;
    }
    w >> 28 != 15
        && (w & 0x0C10_0000 == 0x0410_0000
            || w & 0x0E10_0000 == 0x0810_0000
            || (w & 0x0E10_0090 == 0x0010_0090 && w & 0x60 != 0)
            || w & 0x0E10_00F0 == 0x0000_00D0)
}

/// Whether the instruction `next` (ARM or Thumb) reads register `reg`.
fn reads(next: u32, pc: u32, is_thumb: bool, reg: u32) -> bool {
    let Ok(l) = lift::lift_for(next, pc, is_thumb, Profile::V5te) else {
        return false;
    };
    let r = regs::r(reg);
    l.block
        .insts
        .iter()
        .any(|i| matches!(i.op, Op::Get(g) if g == r))
}

/// The ARM9 internal cycles of the instruction `word` at `pc`, whose
/// fall-through successor is `next` (`None` when it does not fall
/// through), where the lifter counts `lifted`.
#[must_use]
pub fn arm9_internal(word: u32, pc: u32, is_thumb: bool, next: Option<u32>, lifted: u32) -> u32 {
    let class = if is_thumb { thumb(word) } else { arm(word) };
    let Some((cycles, result)) = class else {
        return lifted;
    };
    let size = if is_thumb { 2 } else { 4 };
    let stall = match (result, next) {
        (Some(r), Some(n)) if r.reg != 15 && reads(n, pc.wrapping_add(size), is_thumb, r.reg) => {
            r.stall
        }
        _ => 0,
    };
    let after_store = next.is_some_and(|n| stores(word, is_thumb) && loads(n, is_thumb));
    cycles + stall + u32::from(after_store)
}

#[cfg(test)]
mod tests {
    use super::arm9_internal;

    const ADD_R7_R6_1: u32 = 0xE286_7001;
    const ADD_R8_R8_1: u32 = 0xE288_8001;
    const NOP: u32 = 0xE1A0_0000;

    #[test]
    fn multiplies_take_their_measured_cycles() {
        let mul = 0xE006_0995;
        let muls = 0xE016_0995;
        let umull = 0xE088_6995;
        let smulls = 0xE0D8_6995;
        let smlalbb = 0xE148_6985;
        let smulbb = 0xE166_0985;
        for (w, c) in [
            (mul, 1),
            (muls, 3),
            (umull, 2),
            (smulls, 4),
            (smlalbb, 1),
            (smulbb, 0),
        ] {
            assert_eq!(arm9_internal(w, 0, false, Some(NOP), 9), c, "{w:#x}");
        }
        assert_eq!(arm9_internal(0x4375, 0, true, None, 9), 3, "thumb muls");
    }

    #[test]
    fn a_result_read_next_stalls() {
        let ldr_r6 = 0xE594_6000;
        let ldrb_r6 = 0xE5D4_6000;
        let mul_r6 = 0xE006_0795;
        assert_eq!(arm9_internal(ldr_r6, 0, false, Some(ADD_R7_R6_1), 1), 1);
        assert_eq!(arm9_internal(ldr_r6, 0, false, Some(ADD_R8_R8_1), 1), 0);
        assert_eq!(arm9_internal(ldrb_r6, 0, false, Some(ADD_R7_R6_1), 1), 2);
        assert_eq!(arm9_internal(mul_r6, 0, false, Some(ADD_R7_R6_1), 0), 2);
    }

    #[test]
    fn single_register_transfers_and_pc_loads() {
        let ldm1 = 0xE894_0040;
        let ldm2 = 0xE894_00C0;
        let ldr_pc = 0xE59A_F000;
        let ldm_pc = 0xE89A_8100;
        assert_eq!(arm9_internal(ldm1, 0, false, None, 1), 1);
        assert_eq!(arm9_internal(ldm2, 0, false, None, 1), 0);
        assert_eq!(arm9_internal(ldr_pc, 0, false, None, 1), 2);
        assert_eq!(arm9_internal(ldm_pc, 0, false, None, 1), 2);
        assert_eq!(arm9_internal(0xBD00, 0, true, None, 1), 3, "pop {{pc}}");
        assert_eq!(
            arm9_internal(0xE584_6000, 0, false, Some(0xE594_7004), 0),
            1,
            "str, ldr"
        );
        assert_eq!(
            arm9_internal(0xB4C0, 0, true, Some(0xBCC0), 0),
            1,
            "push, pop"
        );
    }
}
