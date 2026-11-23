//! Ordering and coherence of global memory accesses.
//!
//! The guest follows the PTX memory model. A weak access (`LDG`, `STG`
//! without an order) takes part in synchronization through fences
//! (`MEMBAR`) and barriers (`BAR`); a strong access (`.STRONG.SM`,
//! `.STRONG.GPU`, `.STRONG.SYS`, `.MMIO`) is in addition morally strong
//! with the other strong accesses of its scope, so racing on it is
//! defined. `.CONSTANT` loads read memory that does not change while the
//! program runs.
//!
//! Under the GLSL450 memory model the module declares, plain storage
//! buffer accesses are neither coherent (another invocation may not see
//! them even after a barrier) nor allowed to race; atomics are both. So a
//! global access that may synchronize with another invocation becomes one
//! relaxed atomic per 32-bit word, at a scope that covers the invocations
//! it may meet:
//!
//! * a strong GPU or system access: always, at Device scope;
//! * any other access except `.CONSTANT` loads, in a program with a GPU or
//!   system fence: at Device scope;
//! * a strong CTA access, or a weak one in a program with a barrier or a
//!   CTA fence (`.CTA`, `.SM`, `.VC`), in a program that writes global
//!   memory (a weak access can only meet a write of its own workgroup
//!   through those): at Workgroup scope in compute, task and mesh programs,
//!   Device scope elsewhere (Vulkan allows Workgroup scope only in those).
//!
//! Every other access is plain: nothing it could meet is ordered with it.
//! PTX treats a vector access as one access per element, so splitting a
//! 64- or 128-bit weak access into words keeps its meaning. A strong
//! 64-bit access may be a single-copy atomic `.b64` access, and a strong
//! 128-bit one two of them (`.v2.b64`; PTX has no wider scalar), so each
//! 64-bit element is one 64-bit atomic where the host has them and taken
//! under the element's lock otherwise (see [`super::atomic64`]); as one
//! access per 32-bit element (`.v4.b32`) they are then stronger than the
//! guest asks, which is still one of its outcomes. System scope (`.SYS`) is
//! carried out at Device scope, the widest Vulkan offers without the Vulkan
//! memory model, and listed inexact.
//!
//! Sub-word stores replace their bytes in the containing word with a
//! compare-and-swap loop, one read-modify-write that neither shows an
//! intermediate value nor merges two writers' bits. The loop is lock free:
//! each round some contending lane succeeds, so lanes of one subgroup do
//! not wait on each other.

use super::spv;
use crate::Inst;
use crate::program::Stage;
use crate::spirv::op;
use crate::translate::Ctx;

const SCOPE_DEVICE: u32 = 1;
const SCOPE_WORKGROUP: u32 = 2;
const CAP_INT64_ATOMICS: u32 = 12;
const ATOMIC_STORE: u16 = 228;

/// The order a global access's modifiers ask for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Order {
    Weak,
    Constant,
    /// `.STRONG.SM`: CTA scope.
    StrongCta,
    /// `.STRONG.GPU`, `.MMIO.GPU`.
    StrongGpu,
    /// `.STRONG.SYS`, `.MMIO.SYS`.
    StrongSys,
}

impl Order {
    pub(crate) fn of(m: &str) -> Option<Order> {
        Some(if m.starts_with("CONSTANT") {
            Order::Constant
        } else if m.starts_with("STRONG.SM") {
            Order::StrongCta
        } else if m.starts_with("STRONG.GPU") || m == "MMIO.GPU" {
            Order::StrongGpu
        } else if m.starts_with("STRONG.SYS") || m == "MMIO.SYS" {
            Order::StrongSys
        } else {
            return None;
        })
    }

    fn strong(self) -> bool {
        matches!(self, Order::StrongCta | Order::StrongGpu | Order::StrongSys)
    }
}

/// What the whole program says about the coherence of its global
/// accesses (see the module docs).
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct Plan {
    /// Scope of weak accesses that must be coherent.
    weak: Option<u32>,
    /// Scope of CTA-strong accesses that must be coherent.
    cta: Option<u32>,
}

fn writes_global(i: &Inst) -> bool {
    matches!(
        i.mnemonic,
        "STG" | "ST" | "ATOMG" | "ATOM" | "RED" | "SUST" | "SUATOM" | "SURED"
    )
}

impl Plan {
    /// Some global access of the program must be coherent.
    pub(crate) fn coherent(&self) -> bool {
        self.weak.is_some() || self.cta.is_some()
    }
}

impl Ctx {
    /// Scope of a CTA-wide guarantee in this stage.
    pub(crate) fn cta_scope(&self) -> u32 {
        if self.stage == Stage::Compute || self.stage.is_mesh_pipeline() {
            SCOPE_WORKGROUP
        } else {
            SCOPE_DEVICE
        }
    }

    pub(crate) fn mem_plan(&mut self, insts: &[Inst]) {
        let device_fence = insts
            .iter()
            .any(|i| i.mnemonic == "MEMBAR" && !i.has("CTA") && !i.has("SM") && !i.has("VC"));
        let local_sync = insts
            .iter()
            .any(|i| i.mnemonic == "BAR" || i.mnemonic == "MEMBAR");
        let writes = insts.iter().any(writes_global);
        let cta = self.cta_scope();
        self.mem = if device_fence {
            Plan {
                weak: Some(SCOPE_DEVICE),
                cta: Some(SCOPE_DEVICE),
            }
        } else {
            Plan {
                weak: (local_sync && writes).then_some(cta),
                cta: writes.then_some(cta),
            }
        };
    }

    /// Records a construct that is not exact on this host, once.
    pub(crate) fn inexact(&mut self, what: String) {
        if !self.iface.host.inexact.contains(&what) {
            self.iface.host.inexact.push(what);
        }
    }

    /// The scope at which an access of `order` must be an atomic, if any.
    pub(crate) fn access_scope(&mut self, i: &Inst, order: Order) -> Option<u32> {
        let scope = match order {
            Order::Constant => None,
            Order::Weak => self.mem.weak,
            Order::StrongCta => self.mem.cta,
            Order::StrongGpu | Order::StrongSys => Some(SCOPE_DEVICE),
        }?;
        if order == Order::StrongSys {
            self.inexact(format!(
                "{}.STRONG.SYS: system scope as device scope",
                i.mnemonic
            ));
        }
        Some(scope)
    }

    /// Whether a strong access keeps each 64-bit element single-copy
    /// atomic: one of 64 or 128 bits.
    fn whole64(&self, order: Order, words: u32) -> bool {
        order.strong() && (words == 2 || words == 4)
    }

    /// `words` 32-bit words at global address `addr` as relaxed atomics at
    /// `scope`.
    pub(crate) fn atomic_load_words(
        &mut self,
        addr: u32,
        words: u32,
        order: Order,
        scope: u32,
    ) -> Vec<u32> {
        let u = self.u32t();
        let sc = self.m.c_u32(scope);
        let relaxed = self.m.c_u32(0);
        if self.whole64(order, words) {
            let mut out = Vec::new();
            for e in 0..words / 2 {
                let a = self.word_address(addr, 2 * e);
                if !self.host.int64_atomics_buffer {
                    let (lo, hi) = self.locked_rmw64(a, |_, _| None);
                    out.extend([lo, hi]);
                    continue;
                }
                self.cap(CAP_INT64_ATOMICS);
                let t = self.addr_t();
                let p = self.global_ptr(a, t);
                let v = self.m.emit(spv::ATOMIC_LOAD, t, &[p, sc, relaxed]);
                let v2 = self.m.t_vec(u, 2);
                let pair = self.m.emit(op::BITCAST, v2, &[v]);
                out.extend((0..2).map(|k| self.m.emit(op::COMPOSITE_EXTRACT, u, &[pair, k])));
            }
            return out;
        }
        (0..words)
            .flat_map(|k| {
                let a = self.word_address(addr, k);
                self.global_op(a, u, false, &mut |c, p| {
                    vec![c.m.emit(spv::ATOMIC_LOAD, u, &[p, sc, relaxed])]
                })
            })
            .collect()
    }

    /// Stores `vals` at global address `addr` as relaxed atomics at
    /// `scope`.
    pub(crate) fn atomic_store_words(&mut self, addr: u32, vals: &[u32], order: Order, scope: u32) {
        let u = self.u32t();
        let sc = self.m.c_u32(scope);
        let relaxed = self.m.c_u32(0);
        if self.whole64(order, vals.len() as u32) {
            for (e, half) in vals.chunks(2).enumerate() {
                let a = self.word_address(addr, 2 * e as u32);
                let (lo, hi) = (half[0], half[1]);
                if !self.host.int64_atomics_buffer {
                    self.locked_rmw64(a, |_, _| Some((lo, hi)));
                    continue;
                }
                self.cap(CAP_INT64_ATOMICS);
                let t = self.addr_t();
                let v2 = self.m.t_vec(u, 2);
                let pair = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, half);
                let v = self.m.emit(op::BITCAST, t, &[pair]);
                let p = self.global_ptr_w(a, t);
                self.m.emit_void(ATOMIC_STORE, &[p, sc, relaxed, v]);
            }
            return;
        }
        for (k, &v) in vals.iter().enumerate() {
            let a = self.word_address(addr, k as u32);
            self.global_op(a, u, true, &mut |c, p| {
                c.m.emit_void(ATOMIC_STORE, &[p, sc, relaxed, v]);
                Vec::new()
            });
        }
    }

    fn word_address(&mut self, addr: u32, k: u32) -> u32 {
        self.addr_add_const(addr, 4 * u64::from(k))
    }

    /// Replaces the bits `!keep` of the word at `p` with `bits` in one
    /// atomic read-modify-write at `scope`: a compare-and-swap loop.
    pub(crate) fn cas_insert(&mut self, p: u32, scope: u32, keep: u32, bits: u32) {
        let u = self.u32t();
        let b = self.boolt();
        let sc = self.m.c_u32(scope);
        let relaxed = self.m.c_u32(0);
        let var = self.m.id();
        self.locals.push(var);
        let first = self.m.emit(spv::ATOMIC_LOAD, u, &[p, sc, relaxed]);
        self.m.emit_void(op::STORE, &[var, first]);
        let header = self.m.id();
        let body = self.m.id();
        let cont = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::BRANCH, &[header]);
        self.m.emit_void(op::LABEL, &[header]);
        self.m.emit_void(spv::LOOP_MERGE, &[merge, cont, 0]);
        self.m.emit_void(op::BRANCH, &[body]);
        self.m.emit_void(op::LABEL, &[body]);
        let old = self.m.emit(op::LOAD, u, &[var]);
        let kept = self.m.emit(op::BITWISE_AND, u, &[old, keep]);
        let new = self.m.emit(op::BITWISE_OR, u, &[kept, bits]);
        let prev = self.m.emit(
            spv::ATOMIC_COMPARE_EXCHANGE,
            u,
            &[p, sc, relaxed, relaxed, new, old],
        );
        self.m.emit_void(op::STORE, &[var, prev]);
        let done = self.m.emit(op::I_EQUAL, b, &[prev, old]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[done, merge, cont]);
        self.m.emit_void(op::LABEL, &[cont]);
        self.m.emit_void(op::BRANCH, &[header]);
        self.m.emit_void(op::LABEL, &[merge]);
    }
}
