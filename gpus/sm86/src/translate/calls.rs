//! Calls and returns for the control flow builder: which `CALL`, `RET`,
//! `JMP` and `BPT` forms a program may use, where a call goes and whether
//! a callee returns to the instruction after its call.
//!
//! Absolute addresses (`CALL.ABS`, `JMP`, `LEPC`, the registers `RET.ABS`
//! jumps through) count from the program's first instruction: discovery
//! lays a CUDA kernel out at address 0 with its callees after it and
//! applies the cubin's relocations to that layout. An absolute target of
//! 0 is one no relocation filled (a call to code outside the program, such
//! as the driver's) and is rejected.
//!
//! An indirect `CALL.ABS` (through a register) may go to any function in
//! the program: each direct call target and each instruction that starts a
//! function after another one's `BRA self` and reaches a `RET`. The call
//! becomes a switch over their addresses on the register's low word; an
//! address that names none of them ends the invocation, as a BRX address
//! outside its cases does. Function pointers a
//! program reads from memory hold the addresses the loader gave, so they
//! match only when the kernel was loaded in discovery's layout.

use super::{Code, Res, bad};
use crate::Operand;

/// How far before a CALL the return address `MOV` may be.
const MOVE_WINDOW: usize = 64;

/// Where a CALL goes.
pub(super) enum Callee {
    /// To this instruction.
    To(usize),
    /// Through this register (the low word of a 64-bit address).
    Reg(u8),
}

fn target_of(i: &crate::Inst, end: usize) -> Res<usize> {
    let Some(Operand::Target(t)) = i.ops.last() else {
        return bad(format!("{} without a target", i.mnemonic));
    };
    if *t == 0 && i.has("ABS") {
        return bad(format!("{} to an unrelocated address", i.mnemonic));
    }
    usize::try_from(*t / 16)
        .ok()
        .filter(|k| *k < end && *t % 16 == 0)
        .map_or_else(|| bad("branch target outside the program"), Ok)
}

/// The callee of `CALL` `i` in a program of `end` instructions.
pub(super) fn callee(i: &crate::Inst, w: crate::Word, end: usize) -> Res<Callee> {
    match i.ops.last() {
        Some(Operand::Target(_)) if i.has("REL") || i.has("ABS") => {
            target_of(i, end).map(Callee::To)
        }
        Some(Operand::Reg(..)) if i.has("ABS") && w.get(24, 8) != 255 => {
            Ok(Callee::Reg(w.get(24, 8) as u8))
        }
        _ => bad(format!("CALL form {i}")),
    }
}

/// The target of `JMP` `i`: an absolute address in the program.
pub(super) fn jmp_target(i: &crate::Inst, end: usize) -> Res<usize> {
    if i.mods.iter().any(|m| *m != "U") || !matches!(i.ops.last(), Some(Operand::Target(_))) {
        return bad(format!("JMP form {i}"));
    }
    let Some(Operand::Target(t)) = i.ops.last() else {
        return bad("JMP without a target");
    };
    if *t == 0 {
        return bad("JMP to an unrelocated address");
    }
    usize::try_from(*t / 16)
        .ok()
        .filter(|k| *k < end && *t % 16 == 0)
        .map_or_else(|| bad("branch target outside the program"), Ok)
}

/// Whether `i` is `BPT.TRAP`, which ends the invocation (a SPIR-V program
/// cannot abort the dispatch the way the trap aborts the grid).
pub(super) fn is_trap(i: &crate::Inst) -> bool {
    i.mnemonic == "BPT" && i.has("TRAP")
}

/// Rejects a RET or BPT form the builder does not take.
pub(super) fn check(i: &crate::Inst) -> Res<()> {
    match i.mnemonic {
        "RET" if !i.has("REL") && !i.has("ABS") => bad(format!("RET form {i}")),
        "BPT" if !is_trap(i) => bad(format!("control BPT.{}", i.mods.join("."))),
        _ => Ok(()),
    }
}

/// Signed 48-bit offset field of a control word, in bytes.
fn raw_offset(w: crate::Word) -> i64 {
    ((w.get64(34, 48) as i64) << 16 >> 16) * 4
}

/// Whether the return address of the CALL at `k` is set for the RET at
/// `ret`, shortly before the call: by a `MOV` of the address (the RET adds
/// its own offset, relative to itself for `RET.REL`), or for `RET.ABS` by
/// `LEPC` (its own address), optionally plus an `IADD3` immediate. The
/// register set is the one the RET reads, or one the callee copies into it
/// with unguarded register moves (`MOV`, `IMAD.MOV`).
pub(super) fn return_address_set(code: &Code, k: usize, ret: usize) -> bool {
    let rw = code.words[ret];
    if rw.bit(91) {
        return false;
    }
    let mut regs = vec![rw.get(24, 8)];
    if let Ok(Callee::To(callee)) = callee(&code.insts[k], code.words[k], code.insts.len()) {
        for j in (callee..ret).rev() {
            let w = code.words[j];
            let copy = match w.opcode() {
                0x202 if w.get(72, 4) == 0xf => Some(w.get(32, 8)),
                0x224
                    if code.insts[j].has("MOV")
                        && !code.insts[j].has("X")
                        && w.get(24, 8) == 255
                        && w.get(32, 8) == 255 =>
                {
                    Some(w.get(64, 8))
                }
                _ => None,
            };
            if let Some(src) = copy
                && code.insts[j].guard.is_none()
                && regs.contains(&w.get(16, 8))
            {
                regs.retain(|r| *r != w.get(16, 8));
                regs.push(src);
            }
        }
    }
    let abs = rw.bit(85);
    let off = if abs {
        raw_offset(rw)
    } else {
        16 * (ret as i64 + 1) + raw_offset(rw)
    };
    let want = 16 * (k as i64 + 1) - off;
    let window = k.saturating_sub(16)..k;
    let moved = moved_before(code, k, &regs, want);
    if moved || !abs {
        return moved;
    }
    window.clone().any(|j| {
        let i = &code.insts[j];
        if i.mnemonic != "LEPC" || i.guard.is_some() {
            return false;
        }
        let r = code.words[j].get(16, 8);
        if !regs.contains(&r) {
            return false;
        }
        let add = (j + 1..k)
            .filter(|&m| {
                let w = code.words[m];
                w.opcode() == 0x810
                    && w.get(16, 8) == r
                    && w.get(24, 8) == r
                    && w.get(64, 8) == 255
                    && code.insts[m].guard.is_none()
            })
            .map(|m| i64::from(code.words[m].get(32, 32) as i32))
            .next_back()
            .unwrap_or(0);
        16 * j as i64 + add == want
    })
}

/// Whether one of `regs` holds `want` at the CALL at `k` through an
/// unguarded `MOV` of it in the straight-line code before the call: no
/// transfer and no branch target between, and no other write of that
/// register after it.
fn moved_before(code: &Code, k: usize, regs: &[u32], want: i64) -> bool {
    let entered = |j: usize| {
        code.insts.iter().any(|i| {
            i.mnemonic == "BRA"
                && matches!(i.ops.last(), Some(Operand::Target(t)) if *t == 16 * j as i64)
        })
    };
    let mut live = regs.to_vec();
    for j in (k.saturating_sub(MOVE_WINDOW)..k).rev() {
        let i = &code.insts[j];
        let w = code.words[j];
        if matches!(
            i.mnemonic,
            "BRA" | "JMP" | "EXIT" | "CALL" | "RET" | "BRX" | "BRXU"
        ) || entered(j + 1)
        {
            return false;
        }
        if w.opcode() == 0x802
            && i.guard.is_none()
            && live.contains(&w.get(16, 8))
            && i64::from(w.get(32, 32)) == want
        {
            return true;
        }
        let written = super::uniform::written_regs(i);
        live.retain(|r| !written.iter().any(|x| u32::from(*x) == *r));
        if live.is_empty() {
            return false;
        }
    }
    false
}

/// Whether the line at `k` is `BRA` to itself.
fn bra_self(code: &Code, k: usize) -> bool {
    code.insts[k].mnemonic == "BRA"
        && matches!(code.insts[k].ops.as_slice(), [Operand::Target(t)] if *t == 16 * k as i64)
}

/// The functions an indirect call may reach: every direct call target and
/// every instruction that follows a `BRA self` (after its NOP padding),
/// that `returns` says reaches a RET.
pub(super) fn functions(code: &Code, end: usize, returns: impl Fn(usize) -> bool) -> Vec<usize> {
    let mut v: Vec<usize> = Vec::new();
    for (k, i) in code.insts[..end].iter().enumerate() {
        if i.mnemonic == "CALL"
            && let Ok(Callee::To(t)) = callee(i, code.words[k], end)
        {
            v.push(t);
        }
    }
    for m in 1..end {
        let mut p = m - 1;
        while p > 0 && code.insts[p].mnemonic == "NOP" {
            p -= 1;
        }
        if code.insts[m].mnemonic != "NOP" && bra_self(code, p) {
            v.push(m);
        }
    }
    v.sort_unstable();
    v.dedup();
    v.retain(|&t| returns(t));
    v
}
