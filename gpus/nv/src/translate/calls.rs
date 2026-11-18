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
use crate::fields::CbufIndex;
use std::collections::{BTreeMap, HashMap};

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
pub(super) fn callee(i: &crate::Inst, end: usize) -> Res<Callee> {
    match i.ops.last() {
        Some(Operand::Target(_)) if i.has("REL") || i.has("ABS") => {
            target_of(i, end).map(Callee::To)
        }
        Some(Operand::Reg(r, _)) if i.has("ABS") && *r != 255 => Ok(Callee::Reg(*r)),
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

/// Whether the return address of the CALL at `k` is set for the RET at
/// `ret`, shortly before the call: by a `MOV` of the address (the RET adds
/// its own offset, relative to itself for `RET.REL`), or for `RET.ABS` by
/// `LEPC` (its own address), optionally plus an `IADD3` immediate. The
/// register set is the one the RET reads, or one the callee copies into it
/// with unguarded register moves (`MOV`, `IMAD.MOV`).
pub(super) fn return_address_set(code: &Code, k: usize, ret: usize) -> bool {
    let rf = code.insts[ret].fields;
    if rf.uniform {
        return false;
    }
    let mut regs = vec![u32::from(rf.ra)];
    if let Ok(Callee::To(callee)) = callee(&code.insts[k], code.insts.len()) {
        for j in (callee..ret).rev() {
            if let Some((dst, src)) = register_copy(&code.insts[j])
                && regs.contains(&dst)
            {
                regs.retain(|r| *r != dst);
                regs.push(src);
            }
        }
    }
    let abs = code.insts[ret].has("ABS");
    let off = if abs {
        rf.offset
    } else {
        16 * (ret as i64 + 1) + rf.offset
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
        let Some(Operand::Reg(r, _)) = i.ops.first() else {
            return false;
        };
        let r = u32::from(*r);
        if !regs.contains(&r) {
            return false;
        }
        let add = (j + 1..k)
            .filter_map(|m| {
                let a = &code.insts[m];
                let f = a.fields;
                (a.mnemonic == "IADD3"
                    && u32::from(f.rd) == r
                    && u32::from(f.ra) == r
                    && f.rc == 255
                    && a.guard.is_none())
                .then_some(f.imm)
                .flatten()
            })
            .map(|v| i64::from(v as i32))
            .next_back()
            .unwrap_or(0);
        16 * j as i64 + add == want
    })
}

/// The destination and source of an unguarded register copy: `MOV` of a
/// register with every lane, or `IMAD.MOV` of `RZ * RZ` plus a register.
fn register_copy(i: &crate::Inst) -> Option<(u32, u32)> {
    if i.guard.is_some() {
        return None;
    }
    match (i.mnemonic, i.ops.as_slice()) {
        ("MOV", [Operand::Reg(d, _), Operand::Reg(s, _)]) => Some((u32::from(*d), u32::from(*s))),
        (
            "IMAD",
            [
                Operand::Reg(d, _),
                Operand::Reg(255, _),
                Operand::Reg(255, _),
                Operand::Reg(s, _),
            ],
        ) if i.has("MOV") && !i.has("X") => Some((u32::from(*d), u32::from(*s))),
        _ => None,
    }
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
        if matches!(
            i.mnemonic,
            "BRA" | "JMP" | "EXIT" | "CALL" | "RET" | "BRX" | "BRXU"
        ) || entered(j + 1)
        {
            return false;
        }
        if let ("MOV", [Operand::Reg(d, _), Operand::Hex(v), ..]) = (i.mnemonic, i.ops.as_slice())
            && i.guard.is_none()
            && live.contains(&u32::from(*d))
            && i64::try_from(*v).is_ok_and(|v| v == want)
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
    for i in &code.insts[..end] {
        if i.mnemonic == "CALL"
            && let Ok(Callee::To(t)) = callee(i, end)
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

/// How far before a CALL the load of its target may be.
const EXTERN_WINDOW: usize = 64;

/// The constant bank word an indirect `CALL.ABS` takes its target from,
/// when the word names a symbol of `externs`: the register comes from an
/// unguarded `LDC.64` of `c[bank][index + offset]` whose index register is
/// a constant (`RZ` or an immediate move) in straight code before the call.
fn extern_target(code: &Code, k: usize, externs: &BTreeMap<(u32, u32), String>) -> Option<String> {
    let i = &code.insts[k];
    if i.mnemonic != "CALL" || !i.has("ABS") {
        return None;
    }
    let Some(Operand::Reg(r, _)) = i.ops.last() else {
        return None;
    };
    let lo = k.saturating_sub(EXTERN_WINDOW);
    let writer = (lo..k).rev().find(|&j| writes(code, j, *r))?;
    let l = &code.insts[writer];
    let cb = l.fields.cbuf;
    if l.mnemonic != "LDC" || !l.has("64") || l.guard.is_some() || cb.bank_ureg || cb.mode != 0 {
        return None;
    }
    let (bank, offset) = (cb.bank, cb.offset);
    let base = match cb.index {
        CbufIndex::Reg(x) => constant_reg(code, lo, writer, x)?,
        _ => 0,
    };
    let at = u32::try_from(offset + base).ok()?;
    externs.get(&(bank, at)).cloned()
}

/// Whether instruction `j` ends straight code or writes register `r`.
fn writes(code: &Code, j: usize, r: u8) -> bool {
    let i = &code.insts[j];
    super::cfg::ends_straight(i)
        || matches!(i.ops.first(), Some(Operand::Reg(d, _)) if *d == r || (i.has("64") && d + 1 == r))
}

/// The value an unguarded immediate move gives register `x` before `at`.
fn constant_reg(code: &Code, lo: usize, at: usize, x: u8) -> Option<i64> {
    let j = (lo..at).rev().find(|&j| writes(code, j, x))?;
    let i = &code.insts[j];
    if i.guard.is_some() || !matches!(i.ops.first(), Some(Operand::Reg(d, _)) if *d == x) {
        return None;
    }
    let imm = match i.ops.last() {
        Some(Operand::Int(v)) => *v,
        Some(Operand::Hex(v)) => i64::try_from(*v).ok()?,
        Some(Operand::Reg(255, _)) => 0,
        _ => return None,
    };
    let plain_move = i.mnemonic == "MOV"
        || (i.mnemonic == "IMAD"
            && i.has("MOV")
            && matches!(i.ops.get(1), Some(Operand::Reg(255, _)))
            && matches!(i.ops.get(2), Some(Operand::Reg(255, _))));
    plain_move.then_some(imm)
}

/// The calls of a program to driver functions named by `externs`, by
/// instruction index.
pub(super) fn extern_calls(
    code: &Code,
    externs: &BTreeMap<(u32, u32), String>,
) -> HashMap<usize, String> {
    if externs.is_empty() {
        return HashMap::new();
    }
    (0..code.insts.len())
        .filter_map(|k| extern_target(code, k, externs).map(|n| (k, n)))
        .collect()
}

/// Binding (in [`GUEST_MEMORY_SET`](super::GUEST_MEMORY_SET)) of the
/// assert record: a storage buffer of four words the runtime zeroes before
/// a dispatch and reads after it: the number of invocations that called
/// `__assertfail`, then the line (`R8` of the CUDA call), the call's byte
/// address in the program and the low word of the message address (`R4`)
/// of one of them.
pub const ASSERT_BINDING: u32 = 2;

const STORAGE_BUFFER: u32 = 12;
const SCOPE_DEVICE: u32 = 1;
const ASSERT_KEY: u32 = 0x5_0000;

impl super::Ctx {
    fn assert_record_var(&mut self) -> u32 {
        if let Some(&v) = self.builtins.get(&ASSERT_KEY) {
            return v;
        }
        let u = self.u32t();
        let arr = self.m.id();
        crate::spirv::inst(
            &mut self.m.types,
            crate::spirv::op::TYPE_RUNTIME_ARRAY,
            &[arr, u],
        );
        self.m.deco(arr, 6, &[4]);
        let st = self.m.t_struct_unique(&[arr]);
        self.m.deco(st, 2, &[]);
        self.m.member_deco(st, 0, 35, &[0]);
        let p = self.m.t_ptr(STORAGE_BUFFER, st);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, 34, &[super::GUEST_MEMORY_SET]);
        self.m.deco(var, 33, &[ASSERT_BINDING]);
        self.builtins.insert(ASSERT_KEY, var);
        self.globals.push(var);
        var
    }

    /// What a call to `__assertfail` at instruction `k` does before the
    /// invocation ends: count it and keep its line, address and message.
    pub(super) fn assert_record(&mut self, k: usize) {
        use crate::spirv::op;
        let var = self.assert_record_var();
        self.iface.asserts.push(16 * k as u64);
        let u = self.u32t();
        let pu = self.m.t_ptr(STORAGE_BUFFER, u);
        let z = self.m.c_u32(0);
        let scope = self.m.c_u32(SCOPE_DEVICE);
        let sem = self.m.c_u32(0);
        let one = self.m.c_u32(1);
        let count = self.m.emit(op::ACCESS_CHAIN, pu, &[var, z, z]);
        self.m.emit(op::ATOMIC_I_ADD, u, &[count, scope, sem, one]);
        let line = self.load_reg(8);
        let msg = self.load_reg(4);
        let at = self.m.c_u32(16 * k as u32);
        for (n, v) in [(1, line), (2, at), (3, msg)] {
            let idx = self.m.c_u32(n);
            let p = self.m.emit(op::ACCESS_CHAIN, pu, &[var, z, idx]);
            self.m.emit_void(op::STORE, &[p, v]);
        }
    }
}
