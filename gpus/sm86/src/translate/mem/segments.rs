//! Global memory without buffer device addresses.
//!
//! The guest's 64-bit address space is cut into segments of
//! `1 << shift` bytes, `shift` the specialization constant
//! [`SEGMENT_SHIFT_SPEC`] (default [`SEGMENT_SHIFT_DEFAULT`], the smallest
//! `maxStorageBufferRange` among the hosts). Segment `k` is element `k` of
//! a runtime array of storage buffers at set [`GUEST_MEMORY_SET`] binding 0,
//! and covers guest addresses `k << shift` to `(k + 1) << shift`. The
//! runtime binds, for each segment a program may reach, a buffer holding
//! that window of guest memory, and puts guest addresses in the constant
//! banks unchanged. An access is naturally aligned (the guest faults on a
//! misaligned one), and a segment is a power of two of at least 16 bytes,
//! so no access crosses a segment. Each access type has its own view of the
//! binding: a block with a runtime array of that type.
//!
//! The runtime binds [`SEGMENT_COUNT_SPEC`] segments (default
//! [`SEGMENT_COUNT_DEFAULT`]) and one more descriptor after them, a trap
//! segment: an address past the bound segments, which on the guest faults,
//! reaches the trap segment instead of indexing past the descriptor array.

use crate::spirv::op;
use crate::translate::{
    Ctx, GUEST_MEMORY_SET, GlobalPath, SEGMENT_COUNT_DEFAULT, SEGMENT_COUNT_SPEC,
    SEGMENT_SHIFT_DEFAULT, SEGMENT_SHIFT_SPEC,
};

const STORAGE_BUFFER: u32 = 12;
const DECO_BLOCK: u32 = 2;
const DECO_ARRAY_STRIDE: u32 = 6;
const DECO_ALIASED: u32 = 20;
const DECO_BINDING: u32 = 33;
const DECO_SET: u32 = 34;
const DECO_OFFSET: u32 = 35;
const DECO_NON_UNIFORM: u32 = 5300;
const CAP_SHADER_NON_UNIFORM: u32 = 5301;
const CAP_RUNTIME_DESCRIPTOR_ARRAY: u32 = 5302;
const CAP_STORAGE_BUFFER_NON_UNIFORM: u32 = 5308;
const VIEW_KEY: u32 = 0x3_0000;

impl Ctx {
    /// The byte size of an access type: `u32`, `u64` or a vector of
    /// `u32`.
    fn access_bytes(&mut self, ty: u32) -> u32 {
        let u = self.u32t();
        if ty == u {
            return 4;
        }
        if self.host.int64 && ty == self.addr_t() {
            return 8;
        }
        (2..=4)
            .find(|&n| self.m.t_vec(u, n) == ty)
            .map_or(4, |n| 4 * n)
    }

    fn segment_view(&mut self, ty: u32, size: u32) -> u32 {
        let key = VIEW_KEY + ty;
        if let Some(&v) = self.builtins.get(&key) {
            return v;
        }
        self.cap(CAP_RUNTIME_DESCRIPTOR_ARRAY);
        self.cap(CAP_SHADER_NON_UNIFORM);
        self.cap(CAP_STORAGE_BUFFER_NON_UNIFORM);
        let arr_unique = self.m.id();
        crate::spirv::inst(&mut self.m.types, op::TYPE_RUNTIME_ARRAY, &[arr_unique, ty]);
        self.m.deco(arr_unique, DECO_ARRAY_STRIDE, &[size]);
        let st = self.m.t_struct_unique(&[arr_unique]);
        self.m.deco(st, DECO_BLOCK, &[]);
        self.m.member_deco(st, 0, DECO_OFFSET, &[0]);
        let segs = self.m.t_runtime_array(st);
        let p = self.m.t_ptr(STORAGE_BUFFER, segs);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, DECO_SET, &[GUEST_MEMORY_SET]);
        self.m.deco(var, DECO_BINDING, &[0]);
        self.m.deco(var, DECO_ALIASED, &[]);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    /// A storage buffer pointer to `ty` at 64-bit guest address `addr`.
    pub(crate) fn segment_ptr(&mut self, addr: u32, ty: u32) -> u32 {
        self.iface.host.global = GlobalPath::Segments {
            shift: SEGMENT_SHIFT_DEFAULT,
        };
        let size = self.access_bytes(ty);
        let var = self.segment_view(ty, size);
        let u = self.u32t();
        let b = self.boolt();
        let shift = self.m.spec_u32(SEGMENT_SHIFT_DEFAULT, SEGMENT_SHIFT_SPEC);
        let count = self.m.spec_u32(SEGMENT_COUNT_DEFAULT, SEGMENT_COUNT_SPEC);
        let (lo, hi) = self.addr_words(addr);
        let k32 = self.m.c_u32(32);
        let back = self.m.emit(op::I_SUB, u, &[k32, shift]);
        let seg_lo = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[lo, shift]);
        let seg_hi = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[hi, back]);
        let seg = self.m.emit(op::BITWISE_OR, u, &[seg_lo, seg_hi]);
        let above = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[hi, shift]);
        let z = self.m.c_u32(0);
        let small = self.m.emit(op::I_EQUAL, b, &[above, z]);
        let below = self.m.emit(op::U_LESS_THAN, b, &[seg, count]);
        let inside = self.m.emit(op::LOGICAL_AND, b, &[small, below]);
        let seg = self.select(u, inside, seg, count);
        self.m.deco(seg, DECO_NON_UNIFORM, &[]);
        let one = self.m.c_u32(1);
        let span = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[one, shift]);
        let mask = self.m.emit(op::I_SUB, u, &[span, one]);
        let off = self.m.emit(op::BITWISE_AND, u, &[lo, mask]);
        let k = self.m.c_u32(size.trailing_zeros());
        let elem = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[off, k]);
        let p = self.m.t_ptr(STORAGE_BUFFER, ty);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, seg, z, elem]);
        self.m.deco(ptr, DECO_NON_UNIFORM, &[]);
        ptr
    }
}
