//! Shared memory (LDS, STS) and local memory (LDL, STL).
//!
//! Shared memory is a Workgroup array of 32-bit words and local memory a
//! Private one; the sizes are not in the code, so both lengths are
//! specialization constants the runtime sets from the launch description:
//! [`SHARED_SPEC`] gives the shared words and [`LOCAL_SPEC`] the words of
//! local memory per invocation. The defaults are what a device must always
//! provide. Local addresses are relative to the stack pointer the program
//! loads from `c[0][0x28]`, so the runtime must set that word to the local
//! window in bytes (four times [`LOCAL_SPEC`]'s value). Sub-word stores are atomic AND/OR on the containing word so that
//! lanes writing neighbouring bytes do not lose each other's updates.

use super::{Addr, Size, addr_operand, mods_of, reg_at, spv, store_words, tuple_reg};
use crate::Inst;
use crate::spirv::op;
use crate::translate::{Ctx, Res, bad};

pub const SHARED_SPEC: u32 = 3;
pub const LOCAL_SPEC: u32 = 4;
pub const SHARED_DEFAULT_WORDS: u32 = 4096;
pub const LOCAL_DEFAULT_WORDS: u32 = 1024;

const WORKGROUP: u32 = 4;
const PRIVATE: u32 = 6;
const SCOPE_WORKGROUP: u32 = 2;
const SHARED_KEY: u32 = 0x2_0001;
const LOCAL_KEY: u32 = 0x2_0002;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Space {
    Shared,
    Local,
}

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "LDS" => load(c, i, Space::Shared),
        "LDL" => load(c, i, Space::Local),
        "STS" => store(c, i, Space::Shared),
        "STL" => store(c, i, Space::Local),
        "LDSM" => ldsm(c, i),
        _ => return None,
    })
}

impl Ctx {
    fn scratch_var(&mut self, sp: Space) -> u32 {
        let key = if sp == Space::Shared {
            SHARED_KEY
        } else {
            LOCAL_KEY
        };
        if let Some(&v) = self.builtins.get(&key) {
            return v;
        }
        let (class, spec, default) = if sp == Space::Shared {
            (WORKGROUP, SHARED_SPEC, SHARED_DEFAULT_WORDS)
        } else {
            (PRIVATE, LOCAL_SPEC, LOCAL_DEFAULT_WORDS)
        };
        let u = self.u32t();
        let len = self.m.spec_u32(default, spec);
        let arr = self.m.t_array_of(u, len);
        let p = self.m.t_ptr(class, arr);
        let var = self.m.global(p, class);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    /// The byte address of a shared or local operand.
    pub(crate) fn scratch_address(&mut self, a: &Addr, bytes: u32, sp: Space) -> Res<u32> {
        let u = self.u32t();
        if a.reg.is_some_and(|(_, wide)| wide) {
            return bad("64-bit shared or local address");
        }
        let mut acc = self.m.c_u32(a.offset as u32);
        if let Some((r, _)) = a.reg {
            let mut v = self.load_reg(r);
            if a.shift != 0 {
                let s = self.m.c_u32(a.shift);
                v = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[v, s]);
            }
            acc = self.m.emit(op::I_ADD, u, &[acc, v]);
        }
        if let Some(r) = a.ureg {
            let v = self.load_ureg(r);
            acc = self.m.emit(op::I_ADD, u, &[acc, v]);
        }
        if a.offset >= 0 {
            let hi = (a.offset as u32).saturating_add(bytes).next_multiple_of(4);
            let slot = if sp == Space::Shared {
                &mut self.iface.shared_bytes
            } else {
                &mut self.iface.local_bytes
            };
            *slot = (*slot).max(hi);
        }
        Ok(acc)
    }

    /// A pointer to word `k` past the word that holds byte address `addr`.
    pub(crate) fn scratch_word(&mut self, sp: Space, addr: u32, k: u32) -> u32 {
        let class = if sp == Space::Shared {
            WORKGROUP
        } else {
            PRIVATE
        };
        let var = self.scratch_var(sp);
        let u = self.u32t();
        let two = self.m.c_u32(2);
        let mut idx = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[addr, two]);
        if k != 0 {
            let kk = self.m.c_u32(k);
            idx = self.m.emit(op::I_ADD, u, &[idx, kk]);
        }
        let p = self.m.t_ptr(class, u);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, idx])
    }
}

fn load(c: &mut Ctx, i: &Inst, sp: Space) -> Res<()> {
    let m = mods_of(i)?;
    let d = reg_at(i, 2, 0)?;
    let a = addr_operand(i, 1)?;
    let addr = c.scratch_address(&a, m.size.bytes(), sp)?;
    if d == 255 {
        return Ok(());
    }
    let u = c.u32t();
    if m.size.sub() {
        let p = c.scratch_word(sp, addr, 0);
        let w = c.m.emit(op::LOAD, u, &[p]);
        let v = c.extract_sub(w, addr, m.size);
        return store_words(c, d, &[v]);
    }
    let mut vals = Vec::new();
    for k in 0..m.size.words() {
        let p = c.scratch_word(sp, addr, k);
        vals.push(c.m.emit(op::LOAD, u, &[p]));
    }
    store_words(c, d, &vals)
}

fn store(c: &mut Ctx, i: &Inst, sp: Space) -> Res<()> {
    let m = mods_of(i)?;
    let s = reg_at(i, 2, 1)?;
    let a = addr_operand(i, 0)?;
    let addr = c.scratch_address(&a, m.size.bytes(), sp)?;
    let u = c.u32t();
    if m.size.sub() {
        let v = c.load_reg(s);
        let p = c.scratch_word(sp, addr, 0);
        if sp == Space::Local {
            let old = c.m.emit(op::LOAD, u, &[p]);
            let (keep, bits) = c.insert_sub(v, addr, m.size);
            let kept = c.m.emit(op::BITWISE_AND, u, &[old, keep]);
            let n = c.m.emit(op::BITWISE_OR, u, &[kept, bits]);
            c.m.emit_void(op::STORE, &[p, n]);
        } else {
            let (keep, bits) = c.insert_sub(v, addr, m.size);
            let scope = c.m.c_u32(SCOPE_WORKGROUP);
            let sem = c.m.c_u32(0);
            c.m.emit(spv::ATOMIC_AND, u, &[p, scope, sem, keep]);
            c.m.emit(spv::ATOMIC_OR, u, &[p, scope, sem, bits]);
        }
        return Ok(());
    }
    for k in 0..m.size.words() {
        let v = c.load_reg(tuple_reg(s, k)?);
        let p = c.scratch_word(sp, addr, k);
        c.m.emit_void(op::STORE, &[p, v]);
    }
    Ok(())
}

/// LDSM: each of up to four 8x8 matrices of 16-bit elements has its eight
/// row addresses in eight consecutive lanes (matrix `m` in lanes `8m` to
/// `8m + 7`); lane `t` gets the pair of elements in row `t / 4` at columns
/// `2 * (t % 4)` and the next, or with `MT88` the pair in rows `2 * (t % 4)`
/// and the next at column `t / 4`.
fn ldsm(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut count = 1;
    let mut trans = false;
    for m in &i.mods {
        match *m {
            "16" | "M88" => {}
            "MT88" => trans = true,
            "2" => count = 2,
            "4" => count = 4,
            _ => return bad(format!("LDSM.{m}")),
        }
    }
    let d = reg_at(i, 2, 0)?;
    let a = addr_operand(i, 1)?;
    let addr = c.scratch_address(&a, 16, Space::Shared)?;
    let u = c.u32t();
    let lane = c.lane_id();
    let two = c.m.c_u32(2);
    let three = c.m.c_u32(3);
    let one = c.m.c_u32(1);
    let sixteen = c.m.c_u32(16);
    let row = c.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[lane, two]);
    let quad = c.m.emit(op::BITWISE_AND, u, &[lane, three]);
    let mut vals = Vec::new();
    for m in 0..count {
        let base = c.m.c_u32(8 * m);
        let v = if trans {
            let pair = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[quad, one]);
            let mut half = [0; 2];
            for (k, h) in half.iter_mut().enumerate() {
                let first = c.m.emit(op::I_ADD, u, &[base, pair]);
                let kk = c.m.c_u32(k as u32);
                let src = c.m.emit(op::I_ADD, u, &[first, kk]);
                let ra = c.shuffle(addr, src);
                let col = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[row, one]);
                let at = c.m.emit(op::I_ADD, u, &[ra, col]);
                let p = c.scratch_word(Space::Shared, at, 0);
                let w = c.m.emit(op::LOAD, u, &[p]);
                *h = c.extract_sub(w, at, Size::U16);
            }
            let hi = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[half[1], sixteen]);
            c.m.emit(op::BITWISE_OR, u, &[half[0], hi])
        } else {
            let src = c.m.emit(op::I_ADD, u, &[base, row]);
            let ra = c.shuffle(addr, src);
            let col = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[quad, two]);
            let at = c.m.emit(op::I_ADD, u, &[ra, col]);
            let p = c.scratch_word(Space::Shared, at, 0);
            c.m.emit(op::LOAD, u, &[p])
        };
        vals.push(v);
    }
    store_words(c, d, &vals)
}
