//! Lowering Maxwell instructions onto the instructions of
//! `relift-gpu-nv`'s translator.
//!
//! Every Maxwell form has a row in [`table`]: lowered to exactly one SM86
//! form, a pseudo-instruction the translator handles from its decoded
//! operands, both by case, or a stop. [`lower`] does it for one
//! instruction and [`program`] for a whole program, after the control-flow
//! pass ([`flow`]) has resolved the reconvergence stack. A form is lowered
//! only with evidence that the two behave the same in every case (rounding
//! and denormal modes, saturation, flags, edge values); otherwise it is a
//! pseudo-instruction or a stop. `LOWERING.md` is the table as text.
//!
//! Instruction `k` of the Maxwell code (scheduling words skipped) becomes
//! instruction `k`, so branch targets keep their instruction.

mod build;
pub mod flow;
pub mod table;

pub use build::Lowered;

use crate::bundle::slots;
use crate::form::{Op, Src};
use crate::inst::Inst;
use crate::program::Program;
use relift_gpu_nv::Inst as Inst86;
use relift_gpu_nv::program::{Program as Program86, Sph as Sph86};
use table::Row;

/// What `i` becomes on its own: the stop of its table row, or its
/// lowering. The push and pop forms stop here; they lower only in a
/// program (see [`program`]).
pub fn lower(i: &Inst) -> Lowered {
    lower_in(i, None)
}

/// What `i` becomes in a program whose control-flow pass found `f`.
pub fn lower_with(i: &Inst, f: &flow::Flow) -> Lowered {
    lower_in(i, Some(f))
}

fn lower_in(i: &Inst, f: Option<&flow::Flow>) -> Lowered {
    if let Row::Stop { reason } = table::row(i.op(), i.enc.src) {
        return Lowered::Stop(reason);
    }
    build::lower(i, f).unwrap_or(Lowered::Stop("no lowering for a checked row"))
}

/// A form a program uses that does not lower: where, which and why.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stop {
    /// Code offset of the first instruction of the form.
    pub at: u64,
    pub op: Op,
    pub src: Src,
    pub reason: &'static str,
}

/// The SM86 header of a Maxwell SPH: its 20 words, version 4, the rest
/// zero. The two headers share the layout of those words (Ryujinx's
/// `ShaderHeader` and Mesa NAK's `sph.rs`, both MIT). A header SM86 would
/// read as a task program's has no SM86 counterpart.
fn sph86(p: &Program) -> Result<Option<Sph86>, &'static str> {
    let Some(s) = &p.sph else {
        return Ok(None);
    };
    let mut words = [0u32; 32];
    words[..20].copy_from_slice(&s.words);
    words[0] = (words[0] & !(0x1f << 5)) | (4 << 5);
    let sph = Sph86 { words };
    if sph.isbe_program() {
        return Err("an SPH SM86 reads as a task or mesh program");
    }
    Ok(Some(sph))
}

/// The program `p` lowered: one instruction or pseudo-instruction per
/// Maxwell instruction control reaches, a `NOP` for each it does not.
///
/// # Errors
///
/// Every form that stops, once per form and reason at its first use, or
/// the first construct the control-flow pass does not resolve.
pub fn program(p: &Program) -> Result<Program86, Vec<Stop>> {
    let mut f = flow::flow(&p.code).map_err(|s| vec![s])?;
    f.cc = flow::free_predicate(&p.code);
    let sph = sph86(p).map_err(|reason| {
        vec![Stop {
            at: 0,
            op: Op::Nop,
            src: Src::None,
            reason,
        }]
    })?;
    let n = f.reached.len().max(f.kil_exits.last().map_or(0, |k| k + 1));
    let mut code = vec![Inst86::new("NOP"); n];
    let mut out: Vec<Stop> = Vec::new();
    for s in slots(&p.code) {
        let k = flow::index(s.at);
        if !f.reached[k] {
            continue;
        }
        let Some(i) = Inst::decode(s.word, s.at) else {
            continue;
        };
        if i.guard() == Some((7, true)) {
            continue;
        }
        match lower_in(&i, Some(&f)) {
            Lowered::Exact(x) | Lowered::Pseudo(x) => code[k] = x,
            Lowered::Stop(reason) => {
                let (op, src) = (i.op(), i.enc.src);
                if !out
                    .iter()
                    .any(|x| (x.op, x.src, x.reason) == (op, src, reason))
                {
                    out.push(Stop {
                        at: s.at,
                        op,
                        src,
                        reason,
                    });
                }
            }
        }
    }
    for &k in &f.kil_exits {
        code[k] = Inst86::new("EXIT");
    }
    if !out.is_empty() {
        return Err(out);
    }
    Ok(Program86 {
        sph,
        code: code.into_iter().map(Ok).collect(),
    })
}

/// The forms of `p` that stop (see [`program`]); empty when the whole
/// program lowers.
pub fn stops(p: &Program) -> Vec<Stop> {
    program(p).err().unwrap_or_default()
}
