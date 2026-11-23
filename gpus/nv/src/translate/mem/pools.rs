//! The pool table: every guest memory pool the runtime imported, as one
//! descriptor array of storage buffers and a sorted table of their
//! address ranges.
//!
//! A global access no route proves and no storage buffer slot holds
//! searches the table at run time (see `Ctx::global_op`), as the other
//! recompilers search their bound buffers: the runtime binds, at set
//! [`POOL_SET`] binding [`POOL_ARRAY_BINDING`], one storage buffer per
//! piece of a pool (a pool past the device's storage buffer range is cut
//! into pieces), and at binding [`POOL_TABLE_BINDING`] a buffer of words:
//! word 0 the number of pieces, then from word 4 four words a piece, the
//! low and high words of the GPU address its descriptor starts at and its
//! size, sorted by address, piece `k` element `k` of the array. The search
//! finds the last piece that starts at or below the address, in
//! [`POOL_SEARCH_STEPS`] steps; the access goes through that piece when
//! the address lies inside it. An address in no piece, which faults on the
//! console, keeps the buffer device address. Graphics programs search the
//! table where they miss the slots; compute programs keep the buffer
//! device address.

use super::super::{Ctx, Stage, op};

const STORAGE_BUFFER: u32 = 12;
const DECO_BLOCK: u32 = 2;
const DECO_ARRAY_STRIDE: u32 = 6;
const DECO_NON_WRITABLE: u32 = 24;
const DECO_ALIASED: u32 = 20;
const DECO_BINDING: u32 = 33;
const DECO_SET: u32 = 34;
const DECO_OFFSET: u32 = 35;
const DECO_NON_UNIFORM: u32 = 5300;
const CAP_RUNTIME_DESCRIPTOR_ARRAY: u32 = 5302;
const FUNCTION_INLINE: u32 = 1;
const FUNCTION_DONT_INLINE: u32 = 2;
const VIEW_KEY: u32 = 0x6_0000;
const TABLE_KEY: u32 = 0x6_ffff;

/// The descriptor set of the pool table.
pub const POOL_SET: u32 = 4;
/// The binding of set [`POOL_SET`] that holds the pool pieces' storage
/// buffers.
pub const POOL_ARRAY_BINDING: u32 = 0;
/// The binding of set [`POOL_SET`] that holds the sorted table of the
/// pieces' ranges.
pub const POOL_TABLE_BINDING: u32 = 1;
/// The search's steps: the table holds at most `1 << POOL_SEARCH_STEPS`
/// pieces.
pub const POOL_SEARCH_STEPS: u32 = 16;
/// The first word of the pieces in the table.
pub const POOL_TABLE_FIRST: u32 = 4;
/// Words of the table per piece.
pub const POOL_TABLE_STRIDE: u32 = 4;

impl Ctx {
    fn pool_table(&mut self) -> u32 {
        if let Some(&v) = self.builtins.get(&TABLE_KEY) {
            return v;
        }
        let u = self.u32t();
        let arr = self.m.id();
        crate::spirv::inst(&mut self.m.types, op::TYPE_RUNTIME_ARRAY, &[arr, u]);
        self.m.deco(arr, DECO_ARRAY_STRIDE, &[4]);
        let st = self.m.t_struct_unique(&[arr]);
        self.m.deco(st, DECO_BLOCK, &[]);
        self.m.member_deco(st, 0, DECO_OFFSET, &[0]);
        self.m.member_deco(st, 0, DECO_NON_WRITABLE, &[]);
        let p = self.m.t_ptr(STORAGE_BUFFER, st);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, DECO_SET, &[POOL_SET]);
        self.m.deco(var, DECO_BINDING, &[POOL_TABLE_BINDING]);
        self.builtins.insert(TABLE_KEY, var);
        self.globals.push(var);
        self.iface.pools = true;
        var
    }

    fn table_word(&mut self, var: u32, at: u32) -> u32 {
        let u = self.u32t();
        let z = self.m.c_u32(0);
        let p = self.m.t_ptr(STORAGE_BUFFER, u);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, z, at]);
        self.m.emit(op::LOAD, u, &[ptr])
    }

    /// The table word of field `f` (0 address low, 1 high, 2 size) of
    /// piece `k`.
    fn piece_word(&mut self, var: u32, k: u32, f: u32) -> u32 {
        let u = self.u32t();
        let stride = self.m.c_u32(POOL_TABLE_STRIDE);
        let first = self.m.c_u32(POOL_TABLE_FIRST + f);
        let at = self.m.emit(op::I_MUL, u, &[k, stride]);
        let at = self.m.emit(op::I_ADD, u, &[at, first]);
        self.table_word(var, at)
    }

    /// The run-time search of the pool table for the 64-bit `addr`:
    /// whether a piece holds it, that piece, and the low word of the
    /// address its descriptor starts at. One function per program, kept a
    /// call but in task and mesh programs, as the slot search.
    pub(crate) fn pool_search(&mut self, addr: u32) -> (u32, u32, u32) {
        let u = self.u32t();
        let b = self.boolt();
        let (lo, hi) = self.addr_words(addr);
        let control = if matches!(self.stage, Stage::Task | Stage::Mesh) {
            FUNCTION_INLINE
        } else {
            FUNCTION_DONT_INLINE
        };
        let table = self.pool_table();
        let r = self.shared_fn::<std::convert::Infallible>(
            "pool_search".to_string(),
            &[lo, hi],
            control,
            |c, p| {
                let (lo, hi) = (p[0], p[1]);
                let z = c.m.c_u32(0);
                let one = c.m.c_u32(1);
                let n = c.table_word(table, z);
                let mut idx = z;
                for step in (0..POOL_SEARCH_STEPS).rev() {
                    let s = c.m.c_u32(1 << step);
                    let cand = c.m.emit(op::I_ADD, u, &[idx, s]);
                    let ok = c.m.emit(op::U_LESS_THAN_EQUAL, b, &[cand, n]);
                    let e = c.m.emit(op::I_SUB, u, &[cand, one]);
                    let e = c.select(u, ok, e, z);
                    let blo = c.piece_word(table, e, 0);
                    let bhi = c.piece_word(table, e, 1);
                    let hi_lt = c.m.emit(op::U_LESS_THAN, b, &[bhi, hi]);
                    let hi_eq = c.m.emit(op::I_EQUAL, b, &[bhi, hi]);
                    let lo_le = c.m.emit(op::U_LESS_THAN_EQUAL, b, &[blo, lo]);
                    let eq_le = c.m.emit(op::LOGICAL_AND, b, &[hi_eq, lo_le]);
                    let le = c.m.emit(op::LOGICAL_OR, b, &[hi_lt, eq_le]);
                    let take = c.m.emit(op::LOGICAL_AND, b, &[ok, le]);
                    idx = c.select(u, take, cand, idx);
                }
                let any = c.m.emit(op::I_NOT_EQUAL, b, &[idx, z]);
                let e = c.m.emit(op::I_SUB, u, &[idx, one]);
                let e = c.select(u, any, e, z);
                let blo = c.piece_word(table, e, 0);
                let bhi = c.piece_word(table, e, 1);
                let size = c.piece_word(table, e, 2);
                let dlo = c.m.emit(op::I_SUB, u, &[lo, blo]);
                let borrow = c.m.emit(op::U_LESS_THAN, b, &[lo, blo]);
                let bv = c.select(u, borrow, one, z);
                let dhi = c.m.emit(op::I_SUB, u, &[hi, bhi]);
                let dhi = c.m.emit(op::I_SUB, u, &[dhi, bv]);
                let high_ok = c.m.emit(op::I_EQUAL, b, &[dhi, z]);
                let low_ok = c.m.emit(op::U_LESS_THAN, b, &[dlo, size]);
                let inside = c.m.emit(op::LOGICAL_AND, b, &[high_ok, low_ok]);
                let inside = c.m.emit(op::LOGICAL_AND, b, &[inside, any]);
                Ok(c.select(u, inside, idx, z))
            },
        );
        let r = match r {
            Ok(r) => r,
            Err(e) => match e {},
        };
        let z = self.m.c_u32(0);
        let one = self.m.c_u32(1);
        let found = self.m.emit(op::I_NOT_EQUAL, b, &[r, z]);
        let idx = self.m.emit(op::I_SUB, u, &[r, one]);
        let idx = self.select(u, found, idx, z);
        let base = self.piece_word(table, idx, 0);
        (found, idx, base)
    }

    fn pool_view(&mut self, ty: u32, size: u32) -> u32 {
        let key = VIEW_KEY + ty;
        if let Some(&v) = self.builtins.get(&key) {
            return v;
        }
        self.cap(CAP_RUNTIME_DESCRIPTOR_ARRAY);
        let st = self.view_block(ty, size);
        let arr = self.m.t_runtime_array(st);
        let p = self.m.t_ptr(STORAGE_BUFFER, arr);
        let var = self.m.global(p, STORAGE_BUFFER);
        self.m.deco(var, DECO_SET, &[POOL_SET]);
        self.m.deco(var, DECO_BINDING, &[POOL_ARRAY_BINDING]);
        self.m.deco(var, DECO_ALIASED, &[]);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    /// A pointer to `ty` at `addr` through pool piece `idx`, whose
    /// descriptor starts at the address with low word `base_lo`; `None` for
    /// a type whose size is not a power of two.
    pub(crate) fn pool_ptr(&mut self, idx: u32, base_lo: u32, addr: u32, ty: u32) -> Option<u32> {
        let bytes = self.access_bytes(ty);
        if !bytes.is_power_of_two() {
            return None;
        }
        let u = self.u32t();
        let lo = self.addr_low(addr);
        let off = self.m.emit(op::I_SUB, u, &[lo, base_lo]);
        let sh = self.m.c_u32(bytes.trailing_zeros());
        let elem = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[off, sh]);
        let var = self.pool_view(ty, bytes);
        let z = self.m.c_u32(0);
        let p = self.m.t_ptr(STORAGE_BUFFER, ty);
        self.m.deco(idx, DECO_NON_UNIFORM, &[]);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, idx, z, elem]);
        self.nonuniform_caps();
        self.m.deco(ptr, DECO_NON_UNIFORM, &[]);
        self.iface.pools = true;
        Some(ptr)
    }
}
