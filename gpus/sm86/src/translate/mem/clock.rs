//! SR_CLOCK on any host.
//!
//! The guest's clock is a per-SM cycle counter: two reads in a row differ.
//! A host's device clock may tick slower than the shader runs (two reads
//! can be equal) and some hosts have no clock at all. Each invocation keeps
//! the last value it returned and returns `max(host clock, last + 1)`, so
//! reads strictly increase; without a host clock the counter starts from a
//! 32-bit seed the runtime puts at [`CLOCK_SEED_OFFSET`] in constant bank 0
//! (a value that rises between launches, such as a frame counter times a
//! large step) and rises by [`COUNTER_STEP`] per read. Differences of two
//! reads stay positive, which is what guest code relies on; absolute values
//! and rates are not the guest's on any host.

use super::spv;
use crate::spirv::op;
use crate::translate::{CLOCK_SEED_OFFSET, Clock, ClockPath, Ctx, Res};

const SCOPE_DEVICE: u32 = 1;
const SCOPE_SUBGROUP: u32 = 3;
const CAP_INT64: u32 = 11;
const CAP_SHADER_CLOCK: u32 = 5055;
const U_CONVERT: u16 = 113;
/// How far the counter rises per read without a host clock.
pub const COUNTER_STEP: u32 = 16;

impl Ctx {
    fn clock_last(&mut self) -> (u32, u32) {
        if let Some((lo, hi)) = self.clock_vars {
            return (lo, hi);
        }
        let lo = self.m.id();
        let hi = self.m.id();
        match &mut self.simt {
            Some(st) => st.words.extend([lo, hi]),
            None => self.locals.extend([lo, hi]),
        }
        self.clock_vars = Some((lo, hi));
        (lo, hi)
    }

    fn pair64(&mut self, lo: u32, hi: u32) -> u32 {
        let u = self.u32t();
        let v2 = self.m.t_vec(u, 2);
        let p = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, &[lo, hi]);
        let t = self.m.t_u64();
        self.m.emit(op::BITCAST, t, &[p])
    }

    fn split64(&mut self, v: u32) -> (u32, u32) {
        let u = self.u32t();
        let v2 = self.m.t_vec(u, 2);
        let p = self.m.emit(op::BITCAST, v2, &[v]);
        let lo = self.m.emit(op::COMPOSITE_EXTRACT, u, &[p, 0]);
        let hi = self.m.emit(op::COMPOSITE_EXTRACT, u, &[p, 1]);
        (lo, hi)
    }

    /// The guest clock as (low, high) words.
    pub(crate) fn guest_clock(&mut self) -> Res<(u32, u32)> {
        self.cap(CAP_INT64);
        let u = self.u32t();
        let t = self.m.t_u64();
        let (vlo, vhi) = self.clock_last();
        let (vlo, vhi) = if self.simt.is_some() {
            (self.lane_ptr(vlo, u), self.lane_ptr(vhi, u))
        } else {
            (vlo, vhi)
        };
        let llo = self.m.emit(op::LOAD, u, &[vlo]);
        let lhi = self.m.emit(op::LOAD, u, &[vhi]);
        let last = self.pair64(llo, lhi);
        let path = match self.host.clock {
            Clock::Device => ClockPath::Device,
            Clock::Subgroup => ClockPath::Subgroup,
            Clock::None => ClockPath::Counter,
        };
        self.iface.host.clock = path;
        let one = self.m.c_u64(1);
        let next = self.m.emit(op::I_ADD, t, &[last, one]);
        let v = match path {
            ClockPath::Device | ClockPath::Subgroup => {
                self.cap(CAP_SHADER_CLOCK);
                self.exts.insert("SPV_KHR_shader_clock");
                let scope = self.m.c_u32(if path == ClockPath::Device {
                    SCOPE_DEVICE
                } else {
                    SCOPE_SUBGROUP
                });
                let h = self.m.emit(spv::READ_CLOCK, t, &[scope]);
                let gt = {
                    let b = self.boolt();
                    self.m.emit(op::U_GREATER_THAN, b, &[h, next])
                };
                self.select(t, gt, h, next)
            }
            _ => {
                let z = self.m.c_u32(0);
                let b = self.boolt();
                let fresh = self.m.emit(op::I_EQUAL, b, &[llo, z]);
                let fresh_hi = self.m.emit(op::I_EQUAL, b, &[lhi, z]);
                let fresh = self.m.emit(op::LOGICAL_AND, b, &[fresh, fresh_hi]);
                let seed = self.load_cbuf(0, i64::from(CLOCK_SEED_OFFSET))?;
                let seed = self.m.emit(U_CONVERT, t, &[seed]);
                let base = self.select(t, fresh, seed, last);
                let step = self.m.c_u64(u64::from(COUNTER_STEP));
                self.m.emit(op::I_ADD, t, &[base, step])
            }
        };
        let (lo, hi) = self.split64(v);
        self.m.emit_void(op::STORE, &[vlo, lo]);
        self.m.emit_void(op::STORE, &[vhi, hi]);
        Ok((lo, hi))
    }
}
