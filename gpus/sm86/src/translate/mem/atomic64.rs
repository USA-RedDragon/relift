//! 64-bit global atomics on hosts without them.
//!
//! Each 64-bit read-modify-write takes a spin lock from a table of
//! [`LOCK_WORDS`] words at set [`GUEST_MEMORY_SET`] binding
//! [`LOCK_BINDING`] (zeroed once by the runtime), picked by hashing the
//! address, then reads the two words, writes the new value and releases the
//! lock. The words are read and written with 32-bit atomics so that no
//! cache between the lanes keeps a stale copy.
//!
//! The lock is taken and released in the same loop iteration, with no
//! branch between, so lanes of one subgroup that contend for a lock never
//! wait on a lane of their own subgroup that holds it: the holder finishes
//! in the iteration it won. A subgroup that spins on a lock another
//! subgroup holds relies on the device to go on running the holder, which
//! Vulkan does not promise; that is part of what [`Atomic64Path::Locked`]
//! reports.
//!
//! All 64-bit atomics of all programs on the device must go through the
//! same table for the accesses to be atomic with each other. Weak 64-bit
//! loads and stores are not locked: on the guest a weak access racing an
//! atomic one is a data race with no value guaranteed either. A 32-bit
//! atomic on one half is not locked: it overlaps the 64-bit atomic only in
//! part, so the two are not morally strong and the guest guarantees no
//! atomicity between them. Strong 64-bit loads and stores are single-copy
//! atomic on the guest but two 32-bit atomics here, which a locked
//! read-modify-write can split; they are listed inexact (see
//! [`super::order`]).

use super::spv;
use crate::spirv::op;
use crate::translate::{Atomic64Path, Ctx, GUEST_MEMORY_SET, LOCK_BINDING, LOCK_WORDS};

const STORAGE_BUFFER: u32 = 12;
const SCOPE_DEVICE: u32 = 1;
const SEM_ACQUIRE: u32 = 0x2;
const SEM_RELEASE: u32 = 0x4;
const SEM_ACQUIRE_RELEASE: u32 = 0x8;
const SEM_UNIFORM: u32 = 0x40;
const ATOMIC_STORE: u16 = 228;
const LOCK_KEY: u32 = 0x3_1000;
const DONE_KEY: u32 = 0x3_1001;

impl Ctx {
    fn lock_table(&mut self) -> u32 {
        if let Some(&v) = self.builtins.get(&LOCK_KEY) {
            return v;
        }
        let u = self.u32t();
        let arr = self.m.id();
        crate::spirv::inst(&mut self.m.types, op::TYPE_RUNTIME_ARRAY, &[arr, u]);
        self.m.deco(arr, 6, &[4]);
        let st = self.m.t_struct_unique(&[arr]);
        self.m.deco(st, 2, &[]);
        self.m.member_deco(st, 0, 35, &[0]);
        let p = self.m.t_ptr(STORAGE_BUFFER, st);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, 34, &[GUEST_MEMORY_SET]);
        self.m.deco(var, 33, &[LOCK_BINDING]);
        self.builtins.insert(LOCK_KEY, var);
        self.globals.push(var);
        var
    }

    fn lock_done_var(&mut self) -> u32 {
        if let Some(&v) = self.builtins.get(&DONE_KEY) {
            return v;
        }
        let v = self.m.id();
        self.locals.push(v);
        self.builtins.insert(DONE_KEY, v);
        v
    }

    /// The 64-bit atomic `f(old) -> new` at guest address `addr` under the
    /// address's lock; gives the old value.
    pub(crate) fn locked_rmw64(&mut self, addr: u32, f: impl FnOnce(&mut Ctx, u32) -> u32) -> u32 {
        self.iface.host.atomics64 = Atomic64Path::Locked;
        let u = self.u32t();
        let t = self.m.t_u64();
        let b = self.boolt();
        let table = self.lock_table();
        let k3 = self.m.c_u64(3);
        let k15 = self.m.c_u64(15);
        let a3 = self.m.emit(op::SHIFT_RIGHT_LOGICAL, t, &[addr, k3]);
        let a15 = self.m.emit(op::SHIFT_RIGHT_LOGICAL, t, &[addr, k15]);
        let h = self.m.emit(op::BITWISE_XOR, t, &[a3, a15]);
        let h = self.m.emit(op::U_CONVERT, u, &[h]);
        let mask = self.m.c_u32(LOCK_WORDS - 1);
        let idx = self.m.emit(op::BITWISE_AND, u, &[h, mask]);
        let z = self.m.c_u32(0);
        let pu = self.m.t_ptr(STORAGE_BUFFER, u);
        let lock = self.m.emit(op::ACCESS_CHAIN, pu, &[table, z, idx]);
        let done = self.lock_done_var();
        self.m.emit_void(op::STORE, &[done, z]);
        let k4 = self.m.c_u64(4);
        let hi_addr = self.m.emit(op::I_ADD, t, &[addr, k4]);
        let header = self.m.id();
        let body = self.m.id();
        let crit = self.m.id();
        let skip = self.m.id();
        let cont = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::BRANCH, &[header]);
        self.m.emit_void(op::LABEL, &[header]);
        self.m.emit_void(spv::LOOP_MERGE, &[merge, cont, 0]);
        self.m.emit_void(op::BRANCH, &[body]);
        self.m.emit_void(op::LABEL, &[body]);
        let scope = self.m.c_u32(SCOPE_DEVICE);
        let acq = self.m.c_u32(SEM_ACQUIRE | SEM_UNIFORM);
        let relaxed = self.m.c_u32(0);
        let one = self.m.c_u32(1);
        let prev = self.m.emit(
            spv::ATOMIC_COMPARE_EXCHANGE,
            u,
            &[lock, scope, acq, relaxed, one, z],
        );
        let got = self.m.emit(op::I_EQUAL, b, &[prev, z]);
        self.m.emit_void(op::SELECTION_MERGE, &[skip, 0]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[got, crit, skip]);
        self.m.emit_void(op::LABEL, &[crit]);
        let fence = self.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_UNIFORM);
        self.m.emit_void(op::MEMORY_BARRIER, &[scope, fence]);
        let plo = self.global_ptr(addr, u);
        let lo = self.m.emit(spv::ATOMIC_LOAD, u, &[plo, scope, relaxed]);
        let phi = self.global_ptr(hi_addr, u);
        let hi = self.m.emit(spv::ATOMIC_LOAD, u, &[phi, scope, relaxed]);
        let v2 = self.m.t_vec(u, 2);
        let pair = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, &[lo, hi]);
        let old = self.m.emit(op::BITCAST, t, &[pair]);
        let new = f(self, old);
        let np = self.m.emit(op::BITCAST, v2, &[new]);
        let nlo = self.m.emit(op::COMPOSITE_EXTRACT, u, &[np, 0]);
        let nhi = self.m.emit(op::COMPOSITE_EXTRACT, u, &[np, 1]);
        let plo = self.global_ptr(addr, u);
        self.m.emit_void(ATOMIC_STORE, &[plo, scope, relaxed, nlo]);
        let phi = self.global_ptr(hi_addr, u);
        self.m.emit_void(ATOMIC_STORE, &[phi, scope, relaxed, nhi]);
        self.m.emit_void(op::MEMORY_BARRIER, &[scope, fence]);
        let rel = self.m.c_u32(SEM_RELEASE | SEM_UNIFORM);
        self.m.emit_void(ATOMIC_STORE, &[lock, scope, rel, z]);
        let oldv = self.old64_vars();
        self.m.emit_void(op::STORE, &[oldv.0, lo]);
        self.m.emit_void(op::STORE, &[oldv.1, hi]);
        self.m.emit_void(op::STORE, &[done, one]);
        self.m.emit_void(op::BRANCH, &[skip]);
        self.m.emit_void(op::LABEL, &[skip]);
        let d = self.m.emit(op::LOAD, u, &[done]);
        let fin = self.m.emit(op::I_NOT_EQUAL, b, &[d, z]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[fin, merge, cont]);
        self.m.emit_void(op::LABEL, &[cont]);
        self.m.emit_void(op::BRANCH, &[header]);
        self.m.emit_void(op::LABEL, &[merge]);
        let lo = self.m.emit(op::LOAD, u, &[oldv.0]);
        let hi = self.m.emit(op::LOAD, u, &[oldv.1]);
        let pair = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, &[lo, hi]);
        self.m.emit(op::BITCAST, t, &[pair])
    }

    fn old64_vars(&mut self) -> (u32, u32) {
        if let (Some(&lo), Some(&hi)) = (
            self.builtins.get(&(DONE_KEY + 1)),
            self.builtins.get(&(DONE_KEY + 2)),
        ) {
            return (lo, hi);
        }
        let lo = self.m.id();
        let hi = self.m.id();
        self.locals.push(lo);
        self.locals.push(hi);
        self.builtins.insert(DONE_KEY + 1, lo);
        self.builtins.insert(DONE_KEY + 2, hi);
        (lo, hi)
    }
}
