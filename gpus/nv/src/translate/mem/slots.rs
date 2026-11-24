//! Global accesses through the driver's storage buffer slots.
//!
//! NVN's driver constant bank c\[0\] holds, for each storage buffer slot `k`
//! of a graphics stage, the slot's 64-bit GPU address and size at byte
//! `16 * (`[`slot_row`]`(stage) + k)`. A program that loads, stores or
//! does an atomic through such a base (a uniform register pair loaded
//! from those words) plus a 32-bit register and a constant offset reaches
//! the slot's buffer. On NVIDIA's driver 615 for Ampere, some programs
//! whose results depend on a value loaded through a buffer device address
//! never finish (the device is lost); the same accesses through a storage
//! buffer descriptor run. So the translation reaches such an address
//! through a storage buffer descriptor over the slot, at set
//! [`DRIVER_CBUF_SET`] binding [`slot_binding`]`(k)`, and records the slot
//! in the interface; every other global access keeps the buffer device
//! address path.
//!
//! `Ctx::global_address` notes the slot of the access it builds the
//! address of, and `Ctx::global_ptr` then turns every pointer the access
//! makes (each word, each half of a 64-bit lock pair) into an element of
//! the slot's buffer at the address minus the slot's base. The offset is
//! the low word of that difference: the register is 32 bits and the
//! constant not negative, so the access lies in the 4 GiB above the base.

use super::super::{Ctx, DRIVER_CBUF_SET, Stage, op};
use super::Addr;

const STORAGE_BUFFER: u32 = 12;
const DECO_BLOCK: u32 = 2;
const DECO_ARRAY_STRIDE: u32 = 6;
const DECO_ALIASED: u32 = 20;
const DECO_BINDING: u32 = 33;
const DECO_SET: u32 = 34;
const DECO_OFFSET: u32 = 35;
const VIEW_KEY: u32 = 0x4_0000;
const DECO_NON_UNIFORM: u32 = 5300;
const FUNCTION_INLINE: u32 = 1;
const FUNCTION_DONT_INLINE: u32 = 2;
const CAP_SHADER_NON_UNIFORM: u32 = 5301;
const CAP_STORAGE_BUFFER_NON_UNIFORM: u32 = 5308;

/// The c\[0\] row (16 bytes each) of vertex storage buffer slot 0.
pub const VERTEX_SLOT_ROW: u32 = 0x11;
/// Storage buffer slots per stage.
pub const STORAGE_SLOTS: u32 = 16;
/// First binding of set [`DRIVER_CBUF_SET`] that holds a storage buffer
/// slot.
pub const SLOT_BINDING_BASE: u32 = 64;

/// The c\[0\] row of storage buffer slot 0 of `stage`, from the driver's
/// per-stage table that nvnCommandBufferBindStorageBuffer reads; `None`
/// for compute, whose slots follow its pushed constants and uniform
/// buffers at a per-program place.
pub fn slot_row(stage: Stage) -> Option<u32> {
    match stage {
        Stage::Vertex => Some(VERTEX_SLOT_ROW),
        Stage::TessControl => Some(0x21),
        Stage::TessEval => Some(0x31),
        Stage::Geometry => Some(0x41),
        Stage::Fragment => Some(0x51),
        Stage::Task => Some(0x67),
        Stage::Mesh => Some(0x77),
        Stage::Compute => None,
    }
}

/// The binding of set [`DRIVER_CBUF_SET`] that holds storage buffer slot
/// `k` of the program's stage.
pub fn slot_binding(k: u32) -> u32 {
    SLOT_BINDING_BASE + k
}

/// First binding of set [`DRIVER_CBUF_SET`] that a compute program reaches
/// a base address in its c\[0\] through: binding
/// `COMPUTE_BASE_BINDING + offset / 8` is a storage buffer over the
/// memory from the address in the c\[0\] words at byte `offset` (see
/// [`compute_base_binding`]).
pub const COMPUTE_BASE_BINDING: u32 = 128;
/// The c\[0\] bytes a compute base can be at: the dispatch table ends
/// before 0x800.
pub const COMPUTE_BASE_BYTES: u32 = 0x800;
/// The binding of set [`DRIVER_CBUF_SET`] of a compute program's base
/// table: a uniform buffer whose word `offset / 8` is the low word of the
/// GPU address where the descriptor of the base at c\[0\] byte `offset`
/// starts. The runtime starts it below the base where the base's pool
/// has bytes there, so an access at a negative offset from the base reads
/// what the console reads.
pub const COMPUTE_BASE_TABLE_BINDING: u32 = 80;
/// Most bases a compute program reaches through descriptors; accesses
/// through more keep buffer device addresses.
pub const COMPUTE_BASES: usize = 16;

/// The binding of set [`DRIVER_CBUF_SET`] of the compute base at c\[0\]
/// byte `offset`. A compute program's c\[0\] puts its storage buffers after
/// its pushed constants and uniform buffers, at a place only the dispatch
/// knows, so the binding names the bank offset and the runtime binds the
/// memory from the address there.
pub fn compute_base_binding(offset: u32) -> u32 {
    COMPUTE_BASE_BINDING + offset / 8
}

/// The descriptor an access goes through: element `k` of the stage's
/// slot array (binding [`SLOT_BINDING_BASE`]), or a compute base's own
/// binding.
#[derive(Clone, Copy, Debug)]
enum View {
    Slot(u32),
    Binding(u32),
}

/// The descriptor an access goes through and the low word of the address
/// it starts at.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Route {
    view: View,
    base_lo: u32,
}

impl Ctx {
    /// The low word of the address the descriptor of the compute base at
    /// c\[0\] byte `off` starts at, from the base table.
    fn compute_start(&mut self, off: u32) -> u32 {
        const KEY: u32 = 0x100;
        let var = match self.cbufs.get(&KEY) {
            Some(&v) => v,
            None => {
                let st = match self.cbuf_block {
                    Some(st) => st,
                    None => {
                        let _ = self.cbuf_var(0);
                        self.cbuf_block.expect("cbuf_var makes the block type")
                    }
                };
                let p = self.m.t_ptr(super::super::UNIFORM, st);
                let var = self.m.global(p, super::super::UNIFORM);
                self.m.deco(var, DECO_SET, &[DRIVER_CBUF_SET]);
                self.m
                    .deco(var, DECO_BINDING, &[COMPUTE_BASE_TABLE_BINDING]);
                self.cbufs.insert(KEY, var);
                self.globals.push(var);
                var
            }
        };
        let word = off / 8;
        let u = self.u32t();
        let zero = self.m.c_u32(0);
        let vi = self.m.c_u32(word / 4);
        let ci = self.m.c_u32(word % 4);
        let p = self.m.t_ptr(super::super::UNIFORM, u);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, zero, vi, ci]);
        self.m.emit(op::LOAD, u, &[ptr])
    }

    /// The low word an access's offset into the descriptor of the base at
    /// c\[0\] byte `off` is counted from: the base's own low word `lo` in a
    /// graphics stage, the table's start word in compute.
    fn start_lo(&mut self, off: u32, lo: u32) -> u32 {
        if self.stage == Stage::Compute {
            self.compute_start(off)
        } else {
            lo
        }
    }

    /// The binding, c\[0\] byte and added constant of the storage buffer
    /// slot whose base a global address with uniform register `u` adds is,
    /// when every write that reaches this instruction gives `u` and `u + 1`
    /// the slot's address words (see `tex::Prep::ubase`).
    fn base_slot(&mut self, u: u8) -> Option<(View, u32, i64)> {
        let (lo, add) = self.tex.ubase((self.pc / 16) as usize, u)?;
        Some((self.slot_at(lo)?, lo, add))
    }

    /// The descriptor of the slot whose base address is the c\[0\] word at
    /// byte `lo`, which the interface then records: a graphics stage's slot
    /// rows, or any doubleword past compute's group counts.
    fn slot_at(&mut self, lo: u32) -> Option<View> {
        if self.stage == Stage::Compute {
            let fresh = !self.iface.compute_bases.contains(&lo);
            if !lo.is_multiple_of(8)
                || !(0x10..COMPUTE_BASE_BYTES).contains(&lo)
                || (fresh && self.iface.compute_bases.len() >= COMPUTE_BASES)
            {
                return None;
            }
            self.iface.compute_bases.insert(lo);
            return Some(View::Binding(compute_base_binding(lo)));
        }
        let first = slot_row(self.stage)?;
        let row = lo / 16;
        if !lo.is_multiple_of(16) || !(first..first + STORAGE_SLOTS).contains(&row) {
            return None;
        }
        self.iface.storage_slots.insert(row - first);
        Some(View::Slot(row - first))
    }

    /// The slot route of an access at `a`, or `None` when `a` is not a
    /// slot base plus a 32-bit offset and a constant at or above zero:
    /// either a uniform register pair loaded from the slot's base plus a
    /// 32-bit register, or a register pair made from the slot's base and a
    /// 32-bit value with a carry (see `tex::Prep::pair_base`).
    pub(crate) fn slot_route(&mut self, a: &Addr) -> Option<Route> {
        let below_ok = self.stage == Stage::Compute;
        if a.offset < 0 && !below_ok {
            return None;
        }
        match (a.ureg, a.reg) {
            (Some(u), None | Some((_, false))) => {
                let (view, off, add) = self.base_slot(u)?;
                if add + a.offset < 0 && !below_ok {
                    return None;
                }
                let base_lo = if below_ok {
                    self.compute_start(off)
                } else {
                    let w = self.load_cbuf(0, i64::from(off)).ok()?;
                    self.start_lo(off, w)
                };
                Some(Route { view, base_lo })
            }
            (None, Some((r, true))) if a.shift == 0 => {
                let lo = self.tex.pair_base((self.pc / 16) as usize, r)?;
                let view = self.slot_at(lo)?;
                let word = self.load_cbuf(0, i64::from(lo)).ok()?;
                let base_lo = self.start_lo(lo, word);
                Some(Route { view, base_lo })
            }
            _ => None,
        }
    }

    /// A storage buffer block of a runtime array of `ty` (`size` bytes
    /// each).
    pub(crate) fn view_block(&mut self, ty: u32, size: u32) -> u32 {
        let arr = self.m.id();
        crate::spirv::inst(&mut self.m.types, op::TYPE_RUNTIME_ARRAY, &[arr, ty]);
        self.m.deco(arr, DECO_ARRAY_STRIDE, &[size]);
        let st = self.m.t_struct_unique(&[arr]);
        self.m.deco(st, DECO_BLOCK, &[]);
        self.m.member_deco(st, 0, DECO_OFFSET, &[0]);
        st
    }

    /// The variable of `ty` accesses through `view`: the stage's slot
    /// array, or a compute base's binding.
    fn view_var(&mut self, view: View, ty: u32, size: u32) -> u32 {
        let (key, binding, array) = match view {
            View::Slot(_) => (VIEW_KEY + ty, SLOT_BINDING_BASE, true),
            View::Binding(b) => (VIEW_KEY + (b << 20) + ty, b, false),
        };
        if let Some(&v) = self.builtins.get(&key) {
            return v;
        }
        let st = self.view_block(ty, size);
        let t = if array {
            self.m.t_array(st, STORAGE_SLOTS)
        } else {
            st
        };
        let p = self.m.t_ptr(STORAGE_BUFFER, t);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, DECO_SET, &[DRIVER_CBUF_SET]);
        self.m.deco(var, DECO_BINDING, &[binding]);
        self.m.deco(var, DECO_ALIASED, &[]);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    /// A pointer to `ty` at 64-bit address `addr` through `r`, or `None`
    /// for a type whose size is not a power of two.
    pub(crate) fn routed_ptr(&mut self, r: Route, addr: u32, ty: u32) -> Option<u32> {
        let k = match r.view {
            View::Slot(k) => Some(self.m.c_u32(k)),
            View::Binding(_) => None,
        };
        self.view_ptr(r.view, k, r.base_lo, addr, ty, false)
    }

    /// A pointer to `ty` at `addr` through `view` (slot array element `k`
    /// when it is the slot array) whose descriptor starts at the address
    /// with low word `base_lo`; `nonuniform` when `k` may differ between
    /// invocations.
    fn view_ptr(
        &mut self,
        view: View,
        k: Option<u32>,
        base_lo: u32,
        addr: u32,
        ty: u32,
        nonuniform: bool,
    ) -> Option<u32> {
        let bytes = self.access_bytes(ty);
        if !bytes.is_power_of_two() {
            return None;
        }
        let u = self.u32t();
        let lo = self.addr_low(addr);
        let off = self.m.emit(op::I_SUB, u, &[lo, base_lo]);
        let sh = self.m.c_u32(bytes.trailing_zeros());
        let elem = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[off, sh]);
        let var = self.view_var(view, ty, bytes);
        let z = self.m.c_u32(0);
        let p = self.m.t_ptr(STORAGE_BUFFER, ty);
        let ptr = match k {
            Some(k) => self.m.emit(op::ACCESS_CHAIN, p, &[var, k, z, elem]),
            None => self.m.emit(op::ACCESS_CHAIN, p, &[var, z, elem]),
        };
        if nonuniform {
            self.nonuniform_caps();
            self.m.deco(ptr, DECO_NON_UNIFORM, &[]);
        }
        Some(ptr)
    }

    pub(crate) fn nonuniform_caps(&mut self) {
        self.cap(CAP_SHADER_NON_UNIFORM);
        self.cap(CAP_STORAGE_BUFFER_NON_UNIFORM);
    }

    /// Whether global accesses that no slot route takes search the
    /// stage's slots at run time (see [`Ctx::global_op`]): graphics
    /// stages, where the slots' bases and sizes are in c\[0\].
    fn searches(&self) -> bool {
        self.host.bda && slot_row(self.stage).is_some()
    }

    /// The run-time search of the stage's storage buffer slots for
    /// `addr`: whether one holds it (its base at or below `addr` and the
    /// offset below its size), the last such slot, and the low word of its
    /// base. Every slot is then recorded, so the runtime binds them all.
    /// The search is one function per program, as the other recompilers
    /// keep theirs: kept a call (`DontInline`) where NVIDIA's compiler
    /// took tens of seconds on programs with every access's search
    /// inlined, and inlined in task and mesh programs, where NVIDIA's
    /// compiler gave a call's result to the wrong load.
    fn slot_search(&mut self, addr: u32) -> Option<(u32, u32, u32)> {
        let first = slot_row(self.stage)?;
        let u = self.u32t();
        let b = self.boolt();
        let (lo, hi) = self.addr_words(addr);
        let control = if matches!(self.stage, Stage::Task | Stage::Mesh) {
            FUNCTION_INLINE
        } else {
            FUNCTION_DONT_INLINE
        };
        let r = self
            .shared_fn::<super::super::Unsupported>(
                format!("slot_search_{first}"),
                &[lo, hi],
                control,
                |c, p| {
                    let (lo, hi) = (p[0], p[1]);
                    let z = c.m.c_u32(0);
                    let one = c.m.c_u32(1);
                    let mut r = z;
                    for k in 0..STORAGE_SLOTS {
                        let row = i64::from(16 * (first + k));
                        let blo = c.load_cbuf(0, row)?;
                        let bhi = c.load_cbuf(0, row + 4)?;
                        let size = c.load_cbuf(0, row + 8)?;
                        let dlo = c.m.emit(op::I_SUB, u, &[lo, blo]);
                        let borrow = c.m.emit(op::U_LESS_THAN, b, &[lo, blo]);
                        let bv = c.select(u, borrow, one, z);
                        let dhi = c.m.emit(op::I_SUB, u, &[hi, bhi]);
                        let dhi = c.m.emit(op::I_SUB, u, &[dhi, bv]);
                        let high_ok = c.m.emit(op::I_EQUAL, b, &[dhi, z]);
                        let low_ok = c.m.emit(op::U_LESS_THAN, b, &[dlo, size]);
                        let hit = c.m.emit(op::LOGICAL_AND, b, &[high_ok, low_ok]);
                        let kv = c.m.c_u32(k + 1);
                        r = c.select(u, hit, kv, r);
                    }
                    Ok(r)
                },
            )
            .ok()?;
        for k in 0..STORAGE_SLOTS {
            self.iface.storage_slots.insert(k);
        }
        let z = self.m.c_u32(0);
        let one = self.m.c_u32(1);
        let found = self.m.emit(op::I_NOT_EQUAL, b, &[r, z]);
        let idx = self.m.emit(op::I_SUB, u, &[r, one]);
        let idx = self.select(u, found, idx, z);
        let sixteen = self.m.c_u32(16);
        let at = self.m.emit(op::I_MUL, u, &[idx, sixteen]);
        let row0 = self.m.c_u32(16 * first);
        let at = self.m.emit(op::I_ADD, u, &[at, row0]);
        let base = self.load_cbuf_dyn(0, at).ok()?;
        Some((found, idx, base))
    }

    /// Marks the page of a store or atomic at `addr` (and of its last
    /// byte) written, where guest memory is mirrored, as a store through
    /// the address does (see `Ctx::global_ptr_w`): the runtime copies back
    /// only the pages a program marks, so a store through a slot or pool
    /// descriptor that marked none would be lost.
    fn mark_pool_write(&mut self, addr: u32) {
        self.iface.global_store = true;
        if self.host.mirror_writes && self.host.bda {
            self.iface.marks_written_pages = true;
            self.mark_written(addr);
            let last = self.addr_add_const(addr, 15);
            self.mark_written(last);
        }
    }

    /// Does `op` on the pointer to `ty` at global address `addr` (a store
    /// or atomic when `write`) and gives back the words it returns. An
    /// access no slot route takes searches the stage's slots at run time,
    /// as the other recompilers do, and on a miss the table of every guest
    /// memory pool (see [`super::pools`]): the slot or pool that holds the
    /// address is accessed through its descriptor. Only an address in no
    /// pool, which faults on the console, goes through the buffer device
    /// address.
    pub(crate) fn global_op(
        &mut self,
        addr: u32,
        ty: u32,
        write: bool,
        op_fn: &mut dyn FnMut(&mut Ctx, u32) -> Vec<u32>,
    ) -> Vec<u32> {
        let direct = |c: &mut Ctx, op_fn: &mut dyn FnMut(&mut Ctx, u32) -> Vec<u32>| {
            let p = if write {
                c.global_ptr_w(addr, ty)
            } else {
                c.global_ptr(addr, ty)
            };
            op_fn(c, p)
        };
        if self.route.is_some() || !self.host.bda || !self.access_bytes(ty).is_power_of_two() {
            return direct(self, op_fn);
        }
        // compute programs keep the buffer device address (see `mem::pools`)
        if self.stage == Stage::Compute {
            return direct(self, op_fn);
        }
        let mut vars: Vec<u32> = Vec::new();
        let keep = |c: &mut Ctx, words: Vec<u32>, vars: &mut Vec<u32>| {
            if vars.is_empty() {
                *vars = words
                    .iter()
                    .map(|_| {
                        let v = c.m.id();
                        c.locals.push(v);
                        v
                    })
                    .collect();
            }
            for (&v, &w) in vars.iter().zip(&words) {
                c.m.emit_void(op::STORE, &[v, w]);
            }
        };
        let slots = if self.searches() {
            self.slot_search(addr)
        } else {
            None
        };
        let outer = self.m.id();
        if let Some((found, idx, base)) = slots {
            let hit = self.m.id();
            let miss = self.m.id();
            self.m.emit_void(op::SELECTION_MERGE, &[outer, 0]);
            self.m
                .emit_void(op::BRANCH_CONDITIONAL, &[found, hit, miss]);
            self.m.emit_void(op::LABEL, &[hit]);
            let p = self
                .view_ptr(View::Slot(0), Some(idx), base, addr, ty, true)
                .expect("the size is a power of two");
            if write {
                self.mark_pool_write(addr);
            }
            let words = op_fn(self, p);
            keep(self, words, &mut vars);
            self.m.emit_void(op::BRANCH, &[outer]);
            self.m.emit_void(op::LABEL, &[miss]);
        }
        let (found, idx, base) = self.pool_search(addr);
        let hit = self.m.id();
        let miss = self.m.id();
        let inner = if slots.is_some() { self.m.id() } else { outer };
        self.m.emit_void(op::SELECTION_MERGE, &[inner, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[found, hit, miss]);
        self.m.emit_void(op::LABEL, &[hit]);
        let p = self
            .pool_ptr(idx, base, addr, ty)
            .expect("the size is a power of two");
        if write {
            self.mark_pool_write(addr);
        }
        let words = op_fn(self, p);
        keep(self, words, &mut vars);
        self.m.emit_void(op::BRANCH, &[inner]);
        self.m.emit_void(op::LABEL, &[miss]);
        self.trace_miss(addr);
        let words = direct(self, op_fn);
        keep(self, words, &mut vars);
        self.m.emit_void(op::BRANCH, &[inner]);
        self.m.emit_void(op::LABEL, &[inner]);
        if slots.is_some() {
            self.m.emit_void(op::BRANCH, &[outer]);
            self.m.emit_void(op::LABEL, &[outer]);
        }
        let u = self.u32t();
        vars.iter()
            .map(|&v| self.m.emit(op::LOAD, u, &[v]))
            .collect()
    }
}
