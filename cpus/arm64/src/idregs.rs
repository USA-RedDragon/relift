//! The ID registers an OS shows user space, built from the CPU's features.
//!
//! Linux traps an `MRS` of the ID register space (op0 3, op1 0, CRn 0) at
//! EL0 and answers it with a sanitized copy of the register: only the
//! fields the kernel's `Documentation/arch/arm64/cpu-feature-registers.rst`
//! marks visible, and zero for the rest of the space. A lifted program reads
//! the same values from the same feature set the lifter lifts for, so it
//! never finds a feature whose instructions would not lift.

use crate::decode::{Feature, Features};

/// An `MRS`/`MSR` system register as (op0, op1, CRn, CRm, op2).
pub type SysReg = (u32, u32, u32, u32, u32);

/// `MIDR_EL1`.
pub const MIDR_EL1: SysReg = (3, 0, 0, 0, 0);
/// `MPIDR_EL1`.
pub const MPIDR_EL1: SysReg = (3, 0, 0, 0, 5);
/// `REVIDR_EL1`.
pub const REVIDR_EL1: SysReg = (3, 0, 0, 0, 6);
/// `ID_AA64PFR0_EL1`.
pub const ID_AA64PFR0_EL1: SysReg = (3, 0, 0, 4, 0);
/// `ID_AA64PFR1_EL1`.
pub const ID_AA64PFR1_EL1: SysReg = (3, 0, 0, 4, 1);
/// `ID_AA64ISAR0_EL1`.
pub const ID_AA64ISAR0_EL1: SysReg = (3, 0, 0, 6, 0);
/// `ID_AA64ISAR1_EL1`.
pub const ID_AA64ISAR1_EL1: SysReg = (3, 0, 0, 6, 1);
/// `ID_AA64MMFR2_EL1`.
pub const ID_AA64MMFR2_EL1: SysReg = (3, 0, 0, 7, 2);

/// Whether the feature named `name` (`FEAT_` left off) is in `f`; false for
/// a name the specification does not have.
fn has(f: &Features, name: &str) -> bool {
    Feature::from_name(name).is_some_and(|x| f.has(x))
}

/// A 4-bit field value at bit `at`.
const fn field(v: u64, at: u32) -> u64 {
    (v & 0xf) << at
}

/// `ID_AA64ISAR0_EL1` as user space reads it.
fn isar0(f: &Features) -> u64 {
    let h = |n| has(f, n);
    let aes = if h("PMULL") { 2 } else { u64::from(h("AES")) };
    let sha2 = if h("SHA512") {
        2
    } else {
        u64::from(h("SHA256"))
    };
    let ts = if h("FlagM2") {
        2
    } else {
        u64::from(h("FlagM"))
    };
    field(aes, 4)
        | field(u64::from(h("SHA1")), 8)
        | field(sha2, 12)
        | field(u64::from(h("CRC32")), 16)
        | field(if h("LSE") { 2 } else { 0 }, 20)
        | field(u64::from(h("RDM")), 28)
        | field(u64::from(h("SHA3")), 32)
        | field(u64::from(h("SM3")), 36)
        | field(u64::from(h("SM4")), 40)
        | field(u64::from(h("DotProd")), 44)
        | field(u64::from(h("FHM")), 48)
        | field(ts, 52)
        | field(u64::from(h("RNG")), 60)
}

/// `ID_AA64ISAR1_EL1` as user space reads it.
fn isar1(f: &Features) -> u64 {
    let h = |n| has(f, n);
    let dpb = if h("DPB2") { 2 } else { u64::from(h("DPB")) };
    let lrcpc = if h("LRCPC2") {
        2
    } else {
        u64::from(h("LRCPC"))
    };
    // APA: 1 PAuth, 2 with EPAC, 3 PAuth2, 4 FPAC, 5 FPACCOMBINE; QARMA5 only.
    let apa = if !(h("PAuth") && h("PACQARMA5")) {
        0
    } else if h("FPACCOMBINE") {
        5
    } else if h("FPAC") {
        4
    } else if h("PAuth2") {
        3
    } else if h("EPAC") {
        2
    } else {
        1
    };
    let gpa = u64::from(h("PAuth") && h("PACQARMA5"));
    field(dpb, 0)
        | field(apa, 4)
        | field(u64::from(h("JSCVT")), 12)
        | field(u64::from(h("FCMA")), 16)
        | field(lrcpc, 20)
        | field(gpa, 24)
        | field(u64::from(h("FRINTTS")), 32)
        | field(u64::from(h("SB")), 36)
        | field(u64::from(h("BF16")), 44)
        | field(u64::from(h("DGH")), 48)
        | field(u64::from(h("I8MM")), 52)
}

/// `ID_AA64PFR0_EL1` as user space reads it: AArch64 only at every level,
/// FP and Advanced SIMD (with half precision when FP16 is there), DIT.
fn pfr0(f: &Features) -> u64 {
    let simd = |base: &str| {
        if !has(f, base) {
            0xf
        } else {
            u64::from(has(f, "FP16"))
        }
    };
    field(1, 0)
        | field(1, 4)
        | field(1, 8)
        | field(1, 12)
        | field(simd("FP"), 16)
        | field(simd("AdvSIMD"), 20)
        | field(u64::from(has(f, "DIT")), 48)
}

/// `ID_AA64PFR1_EL1` as user space reads it: BT and SSBS (2 with the
/// `MSR`/`MRS` of SSBS).
fn pfr1(f: &Features) -> u64 {
    field(u64::from(has(f, "BTI")), 0) | field(if has(f, "SSBS") { 2 } else { 0 }, 4)
}

/// `ID_AA64MMFR2_EL1` as user space reads it: AT (LSE2).
fn mmfr2(f: &Features) -> u64 {
    field(u64::from(has(f, "LSE2")), 32)
}

/// The value user space reads from `reg` on a CPU with `features` and main
/// ID `midr`, as Linux shows it; `None` for a register outside the ID space.
pub fn user_id_reg(features: &Features, midr: u64, reg: SysReg) -> Option<u64> {
    let (op0, op1, crn, crm, _) = reg;
    if (op0, op1, crn) != (3, 0, 0) {
        return None;
    }
    Some(match reg {
        MIDR_EL1 => midr,
        MPIDR_EL1 => 0x8000_0000,
        REVIDR_EL1 => 0,
        ID_AA64PFR0_EL1 => pfr0(features),
        ID_AA64PFR1_EL1 => pfr1(features),
        ID_AA64ISAR0_EL1 => isar0(features),
        ID_AA64ISAR1_EL1 => isar1(features),
        ID_AA64MMFR2_EL1 => mmfr2(features),
        // The rest of the space (CRm 0 to 7) reads as zero at EL0.
        _ if crm <= 7 => 0,
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_a57_shows_its_crypto_and_crc_only() {
        let f = Features::CORTEX_A57;
        assert_eq!(user_id_reg(&f, 0, ID_AA64ISAR0_EL1), Some(0x0001_1120));
        assert_eq!(user_id_reg(&f, 0, ID_AA64ISAR1_EL1), Some(0));
        assert_eq!(user_id_reg(&f, 0, ID_AA64PFR0_EL1), Some(0x1111));
    }

    #[test]
    fn the_a78c_shows_lse_rcpc2_and_pauth2() {
        let f = Features::CORTEX_A78C;
        let isar0 = user_id_reg(&f, 0, ID_AA64ISAR0_EL1).unwrap();
        assert_eq!(isar0 >> 20 & 0xf, 2, "LSE");
        assert_eq!(isar0 >> 28 & 0xf, 1, "RDM");
        assert_eq!(isar0 >> 44 & 0xf, 1, "DotProd");
        let isar1 = user_id_reg(&f, 0, ID_AA64ISAR1_EL1).unwrap();
        assert_eq!(isar1 & 0xf, 1, "DPB");
        assert_eq!(isar1 >> 4 & 0xf, 3, "APA: PAuth2");
        assert_eq!(isar1 >> 20 & 0xf, 2, "LRCPC2");
        assert_eq!(isar1 >> 24 & 0xf, 1, "GPA");
        assert_eq!(user_id_reg(&f, 0, ID_AA64PFR0_EL1), Some(0x0011_1111));
        assert_eq!(user_id_reg(&f, 0, ID_AA64MMFR2_EL1), Some(1 << 32));
    }

    #[test]
    fn the_rest_of_the_id_space_reads_zero_and_other_registers_are_not_id_registers() {
        let f = Features::CORTEX_A78C;
        assert_eq!(user_id_reg(&f, 0, (3, 0, 0, 5, 0)), Some(0));
        assert_eq!(user_id_reg(&f, 0x410f_d4b2, MIDR_EL1), Some(0x410f_d4b2));
        assert_eq!(user_id_reg(&f, 0, (3, 3, 13, 0, 2)), None);
    }
}
