//! The cryptographic instructions as one intrinsic each, for the emitter:
//! the runtime's `relift/a64/crypto.h` helpers run each on the same host
//! instruction where the host has one (AES, PMULL, SHA on arm64; AES-NI,
//! PCLMULQDQ, SHA-NI on x86-64), with a portable path otherwise.
//!
//! The generated templates stay the reference semantics: the checker runs
//! them against the oracles, and the runtime's tests run the helpers'
//! host and portable paths against each other and on the Apple M2. Only
//! [`crate::frontend::A64`] lifts through this module.

use relift_ir::{Block, Builder, Exit, Flow, Next, Width};

use crate::decode::{Encoding, Insn};
use crate::regs;

/// The intrinsic name of `enc`'s instruction, for the encodings this module
/// lifts.
pub fn intrinsic_name(enc: Encoding) -> Option<&'static str> {
    use Encoding::*;
    Some(match enc {
        AESE_B_cryptoaes => "a64.aese",
        AESD_B_cryptoaes => "a64.aesd",
        AESMC_B_cryptoaes => "a64.aesmc",
        AESIMC_B_cryptoaes => "a64.aesimc",
        PMULL_asimddiff_L => "a64.pmull64",
        SHA1C_QSV_cryptosha3 => "a64.sha1c",
        SHA1P_QSV_cryptosha3 => "a64.sha1p",
        SHA1M_QSV_cryptosha3 => "a64.sha1m",
        SHA1H_SS_cryptosha2 => "a64.sha1h",
        SHA1SU0_VVV_cryptosha3 => "a64.sha1su0",
        SHA1SU1_VV_cryptosha2 => "a64.sha1su1",
        SHA256H_QQV_cryptosha3 => "a64.sha256h",
        SHA256H2_QQV_cryptosha3 => "a64.sha256h2",
        SHA256SU0_VV_cryptosha2 => "a64.sha256su0",
        SHA256SU1_VVV_cryptosha3 => "a64.sha256su1",
        SHA512H_QQV_cryptosha512_3 => "a64.sha512h",
        SHA512H2_QQV_cryptosha512_3 => "a64.sha512h2",
        SHA512SU0_VV2_cryptosha512_2 => "a64.sha512su0",
        SHA512SU1_VVV2_cryptosha512_3 => "a64.sha512su1",
        SM3PARTW1_VVV4_cryptosha512_3 => "a64.sm3partw1",
        SM3PARTW2_VVV4_cryptosha512_3 => "a64.sm3partw2",
        SM3SS1_VVV4_crypto4 => "a64.sm3ss1",
        SM3TT1A_VVV4_crypto3_imm2 => "a64.sm3tt1a",
        SM3TT1B_VVV4_crypto3_imm2 => "a64.sm3tt1b",
        SM3TT2A_VVV4_crypto3_imm2 => "a64.sm3tt2a",
        SM3TT2B_VVV_crypto3_imm2 => "a64.sm3tt2b",
        SM4E_VV4_cryptosha512_2 => "a64.sm4e",
        SM4EKEY_VVV4_cryptosha512_3 => "a64.sm4ekey",
        _ => return None,
    })
}

/// The C call for intrinsic `name` of this module, over the C expressions
/// `args`.
pub fn c_call(name: &str, args: &[String]) -> Option<String> {
    let f = name.strip_prefix("a64.")?;
    intrinsic_name_known(name).then(|| format!("rl_{f}({})", args.join(", ")))
}

fn intrinsic_name_known(name: &str) -> bool {
    Encoding::all().any(|e| intrinsic_name(e) == Some(name))
}

/// Lifts `insn` at `pc` as one intrinsic call, or `None` when it is not a
/// cryptographic instruction this module covers (PMULL other than the
/// 64-bit form included). The caller decoded `insn` for the CPU's features.
pub fn lift(insn: Insn, pc: u64) -> Option<Block> {
    use Encoding::*;
    let name = intrinsic_name(insn.encoding)?;
    if insn.encoding == PMULL_asimddiff_L && insn.get("size") != 3 {
        return None;
    }
    let mut b = Builder::new(regs::regs(), Width::W64, pc);
    let v = |b: &mut Builder, f: &str| b.get(regs::vreg(insn.get(f)));
    let d = insn.get("Rd");
    let r = match insn.encoding {
        AESMC_B_cryptoaes | AESIMC_B_cryptoaes | SM4EKEY_VVV4_cryptosha512_3 => {
            let n = v(&mut b, "Rn");
            if insn.encoding == SM4EKEY_VVV4_cryptosha512_3 {
                let m = v(&mut b, "Rm");
                b.intrinsic(name, &[n, m], Some(Width::W128))
            } else {
                b.intrinsic(name, &[n], Some(Width::W128))
            }
        }
        PMULL_asimddiff_L => {
            let (n, m) = (v(&mut b, "Rn"), v(&mut b, "Rm"));
            let lo = if insn.get("Q") == 1 { 64 } else { 0 };
            let a = b.extract(n, lo, Width::W64);
            let c = b.extract(m, lo, Width::W64);
            b.intrinsic(name, &[a, c], Some(Width::W128))
        }
        SHA1C_QSV_cryptosha3 | SHA1P_QSV_cryptosha3 | SHA1M_QSV_cryptosha3 => {
            let (dv, n, m) = (v(&mut b, "Rd"), v(&mut b, "Rn"), v(&mut b, "Rm"));
            let e = b.trunc(n, Width::W32);
            b.intrinsic(name, &[dv, e, m], Some(Width::W128))
        }
        SHA1H_SS_cryptosha2 => {
            let n = v(&mut b, "Rn");
            let s = b.trunc(n, Width::W32);
            let h = b.intrinsic(name, &[s], Some(Width::W32));
            b.zext(h, Width::W128)
        }
        SM3SS1_VVV4_crypto4 => {
            let (n, m, a) = (v(&mut b, "Rn"), v(&mut b, "Rm"), v(&mut b, "Ra"));
            b.intrinsic(name, &[n, m, a], Some(Width::W128))
        }
        SM3TT1A_VVV4_crypto3_imm2
        | SM3TT1B_VVV4_crypto3_imm2
        | SM3TT2A_VVV4_crypto3_imm2
        | SM3TT2B_VVV_crypto3_imm2 => {
            let (dv, n, m) = (v(&mut b, "Rd"), v(&mut b, "Rn"), v(&mut b, "Rm"));
            let i = b.konst(Width::W32, u128::from(insn.get("imm2")));
            b.intrinsic(name, &[dv, n, m, i], Some(Width::W128))
        }
        SHA1SU1_VV_cryptosha2
        | SHA256SU0_VV_cryptosha2
        | SHA512SU0_VV2_cryptosha512_2
        | SM4E_VV4_cryptosha512_2
        | AESE_B_cryptoaes
        | AESD_B_cryptoaes => {
            let (dv, n) = (v(&mut b, "Rd"), v(&mut b, "Rn"));
            b.intrinsic(name, &[dv, n], Some(Width::W128))
        }
        _ => {
            let (dv, n, m) = (v(&mut b, "Rd"), v(&mut b, "Rn"), v(&mut b, "Rm"));
            b.intrinsic(name, &[dv, n, m], Some(Width::W128))
        }
    };
    b.set(regs::vreg(d), r);
    Some(b.finish(
        4,
        Exit {
            next: Next::Const(pc.wrapping_add(4)),
            flow: Flow::Jump,
        },
    ))
}
