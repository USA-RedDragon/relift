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

/// The slot an access goes through: its binding and the low word of the
/// slot's base address.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Route {
    binding: u32,
    base_lo: u32,
}

impl Ctx {
    /// The storage buffer slot whose base a global address with uniform
    /// register `u` adds is, when every load that reaches this instruction
    /// gives `u` and `u + 1` the slot's address words.
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

    fn base_slot(&mut self, u: u8) -> Option<(u32, u32, i64)> {
        let (lo, add) = self.tex.ubase((self.pc / 16) as usize, u)?;
        Some((self.slot_at(lo)?, lo, add))
    }

    /// The binding of the slot whose base address is the c\[0\] word at byte
    /// `lo`, which the interface then records: a graphics stage's slot
    /// rows, or any doubleword past compute's group counts.
    fn slot_at(&mut self, lo: u32) -> Option<u32> {
        if self.stage == Stage::Compute {
            let fresh = !self.iface.compute_bases.contains(&lo);
            if !lo.is_multiple_of(8)
                || !(0x10..COMPUTE_BASE_BYTES).contains(&lo)
                || (fresh && self.iface.compute_bases.len() >= COMPUTE_BASES)
            {
                return None;
            }
            self.iface.compute_bases.insert(lo);
            return Some(compute_base_binding(lo));
        }
        let first = slot_row(self.stage)?;
        let row = lo / 16;
        if !lo.is_multiple_of(16) || !(first..first + STORAGE_SLOTS).contains(&row) {
            return None;
        }
        self.iface.storage_slots.insert(row - first);
        Some(slot_binding(row - first))
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
                let (binding, off, add) = self.base_slot(u)?;
                if add + a.offset < 0 && !below_ok {
                    return None;
                }
                let base_lo = if below_ok {
                    self.compute_start(off)
                } else {
                    let w = self.load_cbuf(0, i64::from(off)).ok()?;
                    self.start_lo(off, w)
                };
                Some(Route { binding, base_lo })
            }
            (None, Some((r, true))) if a.shift == 0 => {
                let lo = self.tex.pair_base((self.pc / 16) as usize, r)?;
                let binding = self.slot_at(lo)?;
                let word = self.load_cbuf(0, i64::from(lo)).ok()?;
                let base_lo = self.start_lo(lo, word);
                Some(Route { binding, base_lo })
            }
            _ => None,
        }
    }

    fn slot_view(&mut self, binding: u32, ty: u32, size: u32) -> u32 {
        let key = VIEW_KEY + (binding << 20) + ty;
        if let Some(&v) = self.builtins.get(&key) {
            return v;
        }
        let arr = self.m.id();
        crate::spirv::inst(&mut self.m.types, op::TYPE_RUNTIME_ARRAY, &[arr, ty]);
        self.m.deco(arr, DECO_ARRAY_STRIDE, &[size]);
        let st = self.m.t_struct_unique(&[arr]);
        self.m.deco(st, DECO_BLOCK, &[]);
        self.m.member_deco(st, 0, DECO_OFFSET, &[0]);
        let p = self.m.t_ptr(STORAGE_BUFFER, st);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, DECO_SET, &[DRIVER_CBUF_SET]);
        self.m.deco(var, DECO_BINDING, &[binding]);
        self.m.deco(var, DECO_ALIASED, &[]);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    /// A pointer to `ty` at 64-bit address `addr` through the slot of
    /// `r`, or `None` for a type whose size is not a power of two.
    pub(crate) fn routed_ptr(&mut self, r: Route, addr: u32, ty: u32) -> Option<u32> {
        let bytes = self.access_bytes(ty);
        if !bytes.is_power_of_two() {
            return None;
        }
        let u = self.u32t();
        let lo = self.addr_low(addr);
        let off = self.m.emit(op::I_SUB, u, &[lo, r.base_lo]);
        let k = self.m.c_u32(bytes.trailing_zeros());
        let elem = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[off, k]);
        let var = self.slot_view(r.binding, ty, bytes);
        let z = self.m.c_u32(0);
        let p = self.m.t_ptr(STORAGE_BUFFER, ty);
        Some(self.m.emit(op::ACCESS_CHAIN, p, &[var, z, elem]))
    }
}
