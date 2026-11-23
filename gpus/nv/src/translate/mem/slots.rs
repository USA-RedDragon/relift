//! Vertex program loads from the driver's storage buffer slots.
//!
//! NVN's driver constant bank c[0] holds, for each storage buffer slot `k`
//! of the vertex stage, the slot's 64-bit GPU address and size at byte
//! `16 * (`[`VERTEX_SLOT_ROW`]` + k)`. A vertex program that loads through
//! such a base (a uniform register pair loaded from those words) plus a
//! register or constant offset reads the slot's buffer. On NVIDIA's
//! driver 615 for Ampere, some vertex programs whose results depend on a
//! value loaded through a buffer device address never finish (the device
//! is lost); the same loads from a storage buffer descriptor run. So the
//! translation reads such a load from a storage buffer descriptor over
//! the slot, at set [`DRIVER_CBUF_SET`] binding [`slot_binding`]`(k)`, and
//! records the slot in the interface; every other global access keeps the
//! buffer device address path.

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

/// The c[0] row (16 bytes each) of vertex storage buffer slot 0.
pub const VERTEX_SLOT_ROW: u32 = 0x11;
/// Storage buffer slots per stage.
pub const STORAGE_SLOTS: u32 = 16;
/// First binding of set [`DRIVER_CBUF_SET`] that holds a storage buffer
/// slot.
pub const SLOT_BINDING_BASE: u32 = 64;

/// The binding of set [`DRIVER_CBUF_SET`] that holds vertex storage
/// buffer slot `k`.
pub fn slot_binding(k: u32) -> u32 {
    SLOT_BINDING_BASE + k
}

impl Ctx {
    /// The vertex storage buffer slot whose base a global address with
    /// uniform register `u` adds is, when every load that reaches this
    /// instruction gives `u` and `u + 1` the slot's address words.
    fn base_slot(&self, u: u8) -> Option<u32> {
        if self.stage != Stage::Vertex || u >= 62 {
            return None;
        }
        let k = (self.pc / 16) as usize;
        let one = |r: u8| match self.tex.uniform_source(k, r)? {
            (0, off, false) => Some(off),
            _ => None,
        };
        let lo = one(u)?;
        let hi = one(u + 1)?;
        let row = lo / 16;
        (lo % 16 == 0
            && hi == lo + 4
            && (VERTEX_SLOT_ROW..VERTEX_SLOT_ROW + STORAGE_SLOTS).contains(&row))
        .then(|| row - VERTEX_SLOT_ROW)
    }

    fn slot_view(&mut self, slot: u32, ty: u32, size: u32) -> u32 {
        let key = VIEW_KEY + slot * 0x100 + ty;
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
        self.m.deco(var, DECO_BINDING, &[slot_binding(slot)]);
        self.m.deco(var, DECO_ALIASED, &[]);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    /// A pointer to the load of `ty` (`bytes` bytes, naturally aligned) at
    /// address `a` through the vertex storage buffer slot its base names,
    /// or `None` when `a` is not such an address.
    pub(crate) fn slot_ptr(&mut self, a: &Addr, ty: u32, bytes: u32) -> Option<u32> {
        let slot = self.base_slot(a.ureg?)?;
        if matches!(a.reg, Some((_, true))) || a.shift != 0 || !bytes.is_power_of_two() {
            return None;
        }
        let u = self.u32t();
        let mut off = self.m.c_u32(a.offset as u32);
        if let Some((r, _)) = a.reg {
            let rv = self.load_reg(r);
            off = self.m.emit(op::I_ADD, u, &[rv, off]);
        }
        let k = self.m.c_u32(bytes.trailing_zeros());
        let elem = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[off, k]);
        let var = self.slot_view(slot, ty, bytes);
        let z = self.m.c_u32(0);
        let p = self.m.t_ptr(STORAGE_BUFFER, ty);
        self.iface.storage_slots.insert(slot);
        Some(self.m.emit(op::ACCESS_CHAIN, p, &[var, z, elem]))
    }
}
