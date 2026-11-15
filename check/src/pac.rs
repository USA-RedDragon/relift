//! States for the pointer authentication forms (`--pac-states`): the
//! authenticated register holds a pointer signed with the form's key and
//! modifier, or one with a flipped PAC bit or a flipped address bit, so
//! both the success and the failure paths run. Signing runs the matching
//! PAC instruction in the interpreter on the state.
//!
//! The guest's address space is 39 bits with top-byte ignore off (see the
//! generator's `reg_config`), so a canonical pointer has bits 63 to 39 all
//! equal and the PAC field is bits 63 to 56 and 54 to 39.

use std::sync::atomic::AtomicBool;

use relift_cpu_arm64::decode::Insn;

use crate::emu::{CODE, Guest, Outcome, interpret, lift};
use crate::states::Rng;

/// Whether states for the authenticating forms carry signed pointers.
pub static PAC_STATES: AtomicBool = AtomicBool::new(false);

const VA_BITS: u32 = 39;
const LOW: u64 = (1 << VA_BITS) - 1;

/// What an authenticating form reads, and how to sign it.
struct Auth {
    /// The register holding the pointer.
    reg: u32,
    /// The PAC instruction that signs it in place.
    sign: u32,
    /// Where the authenticated pointer ends up.
    to: To,
}

#[derive(Clone, Copy)]
enum To {
    /// In this register.
    Reg(u32),
    /// In the PC.
    Pc,
    /// As the address of a load.
    Load,
    /// Nowhere; the form strips.
    Strip,
}

const HINT: u32 = 0xd503_201f;
const PACIA: u32 = 0xdac1_0000;
const PACIZA: u32 = 0xdac1_23e0;
const PACDZA: u32 = 0xdac1_2be0;

fn auth_of(insn: &Insn) -> Option<Auth> {
    let n = insn.encoding.name();
    let w = insn.word;
    let f = |s: &str| insn.get(s);
    let key_b = u32::from(n.contains("AB_") || n.contains("ABZ_"));
    let hint = |op: u32, reg: u32| Auth {
        reg,
        sign: HINT | (op << 5),
        to: To::Reg(reg),
    };
    Some(match n {
        _ if n.ends_with("_dp_1src") && n.starts_with("AUT") => Auth {
            reg: f("Rd"),
            sign: w & !(1 << 12),
            to: To::Reg(f("Rd")),
        },
        "XPACI_64Z_dp_1src" | "XPACD_64Z_dp_1src" => Auth {
            reg: f("Rd"),
            sign: if n.starts_with("XPACI") {
                PACIZA
            } else {
                PACDZA
            } | f("Rd"),
            to: To::Strip,
        },
        "AUTIA1716_HI_hints" => hint(8, 17),
        "AUTIB1716_HI_hints" => hint(10, 17),
        "AUTIAZ_HI_hints" => hint(24, 30),
        "AUTIASP_HI_hints" => hint(25, 30),
        "AUTIBZ_HI_hints" => hint(26, 30),
        "AUTIBSP_HI_hints" => hint(27, 30),
        "XPACLRI_HI_hints" => Auth {
            reg: 30,
            sign: HINT | (25 << 5),
            to: To::Strip,
        },
        _ if n.ends_with("Z_64_branch_reg") => Auth {
            reg: f("Rn"),
            sign: PACIZA | (key_b << 10) | f("Rn"),
            to: To::Pc,
        },
        _ if n.starts_with("RETA") => Auth {
            reg: 30,
            sign: PACIA | (key_b << 10) | (31 << 5) | 30,
            to: To::Pc,
        },
        _ if n.ends_with("_branch_reg") => Auth {
            reg: f("Rn"),
            sign: PACIA | (key_b << 10) | (f("Rm") << 5) | f("Rn"),
            to: To::Pc,
        },
        _ if n.starts_with("LDRA") => Auth {
            reg: f("Rn"),
            sign: PACDZA | (key_b << 10) | f("Rn"),
            to: To::Load,
        },
        _ => return None,
    })
}

/// Replaces the pointer the form authenticates with a signed one, a
/// tampered one or (a quarter of the time each) leaves it random.
pub fn place(insn: &Insn, g: &mut Guest, r: &mut Rng) {
    let Some(a) = auth_of(insn) else {
        return;
    };
    if a.reg == 31 {
        return;
    }
    let mut p = r.next() & LOW;
    if matches!(a.to, To::Pc) {
        p &= !3;
    }
    if matches!(a.to, To::Load) {
        p &= !7;
    }
    if r.chance(0.25) {
        p |= !LOW;
    }
    g.x[a.reg as usize] = p;
    let Ok(b) = lift(a.sign, CODE) else {
        return;
    };
    let Outcome::Done(s) = interpret(&[b], &[a.sign], g) else {
        return;
    };
    let mut signed = s.x[a.reg as usize];
    match r.below(4) {
        0 | 1 => {}
        2 => {
            let bit = r.pick(&[39, 42, 47, 50, 54, 56, 60, 63]);
            signed ^= 1 << bit;
        }
        _ => signed ^= 1 << r.below(u64::from(VA_BITS)),
    }
    g.x[a.reg as usize] = signed;
}

fn canonical(p: u64) -> bool {
    let top = p >> VA_BITS;
    top == 0 || top == u64::MAX >> VA_BITS
}

/// The fault address in an outcome's message: the interpreter's
/// `... at 0x.. faulted` or the harness's `..., address 0x..`.
fn fault_addr(o: &Outcome) -> Option<u64> {
    let Outcome::Fault(m) = o else {
        return None;
    };
    let s = m
        .split_once("address 0x")
        .map(|(_, a)| a)
        .or_else(|| m.split_once(" at 0x").map(|(_, a)| a))?;
    let hex: String = s.chars().take_while(char::is_ascii_hexdigit).collect();
    u64::from_str_radix(&hex, 16).ok()
}

/// Whether the oracle's run of the form authenticated its pointer; `None`
/// for a form that does not authenticate or an outcome that does not say.
pub fn authenticated(insn: &Insn, or: &Outcome) -> Option<bool> {
    let a = auth_of(insn)?;
    match (a.to, or) {
        (To::Reg(d), Outcome::Done(g)) => g.x.get(d as usize).map(|x| canonical(*x)),
        (To::Pc, Outcome::Done(g)) => Some(canonical(g.pc)),
        (To::Load, Outcome::Fault(_)) => fault_addr(or).map(canonical),
        _ => None,
    }
}

/// A difference in the fault address of two faulting runs of a load,
/// which the generic comparison ignores; `None` when they agree.
pub fn fault_differs(ir: &Outcome, or: &Outcome) -> Option<String> {
    let (a, b) = (fault_addr(ir)?, fault_addr(or)?);
    (a != b).then(|| format!("fault address ir={a:#x} or={b:#x}"))
}
