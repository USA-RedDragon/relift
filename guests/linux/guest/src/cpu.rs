//! The CPU a program runs on and the capabilities the kernel reports for
//! it.
//!
//! The CPU is the project's choice ([`GuestCpu`]), a Cortex-A57 (Armv8.0-A
//! with the Cryptographic Extension and CRC32) unless the project names
//! another. Everything a program can learn about the CPU comes from its one
//! feature set: the auxiliary vector's `AT_HWCAP` and `AT_HWCAP2`, since libc
//! start-up code picks its routines from them (atomics, string functions,
//! crypto) and a program that finds a feature there uses it, and the ID
//! registers an `MRS` at EL0 reads, which the kernel answers for user space
//! (`HWCAP_CPUID`). An instruction of a feature the CPU lacks lifts as
//! undefined, as on the hardware.

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::{Feature, Features};

/// A CPU a Linux program can be lifted for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GuestCpu {
    /// The Cortex-A57: Armv8.0-A, the default.
    CortexA57,
    /// The Cortex-A78C: Armv8.2-A with LSE, LRCPC2, DotProd and PAuth2
    /// ([`Features::CORTEX_A78C`]).
    CortexA78C,
}

impl GuestCpu {
    /// The CPU the project's `game.cpu` names (`cortex-a57`, `cortex-a78c`).
    pub fn from_name(name: &str) -> Option<GuestCpu> {
        match name {
            "cortex-a57" => Some(GuestCpu::CortexA57),
            "cortex-a78c" => Some(GuestCpu::CortexA78C),
            _ => None,
        }
    }

    /// Its architecture features.
    pub const fn features(self) -> Features {
        match self {
            GuestCpu::CortexA57 => Features::CORTEX_A57,
            GuestCpu::CortexA78C => Features::CORTEX_A78C,
        }
    }

    /// Its `MIDR_EL1`: Arm's implementer code, the variant, part number and
    /// revision of a Cortex-A57 r1p1 and a Cortex-A78C r0p2.
    pub const fn midr(self) -> u64 {
        match self {
            GuestCpu::CortexA57 => 0x411f_d071,
            GuestCpu::CortexA78C => 0x410f_d4b2,
        }
    }

    /// The CPU to lift for: its features, the process's address layout, and
    /// the kernel's answers to `MRS` of the ID registers.
    pub const fn cpu(self) -> Cpu {
        Cpu::new(self.features(), VA).with_user_ids(self.midr())
    }

    /// The `AT_HWCAP` word of its features, the way the kernel's
    /// `arch/arm64/include/uapi/asm/hwcap.h` numbers them.
    pub fn hwcap(self) -> u64 {
        bits(self.features(), HWCAP) | HWCAP_CPUID
    }

    /// The `AT_HWCAP2` word of its features.
    pub fn hwcap2(self) -> u64 {
        bits(self.features(), HWCAP2)
    }
}

/// The address bits of a process, `TCR_EL1`'s T0SZ of 25, without
/// top-byte ignore for instruction fetches.
pub const VA: Va = Va::BITS39;

/// `HWCAP_CPUID`: `MRS` of the ID registers at EL0 is emulated by the kernel.
pub const HWCAP_CPUID: u64 = 1 << 11;

/// `AT_HWCAP` bits and the features (all of them) each one needs.
const HWCAP: &[(u32, &[&str])] = &[
    (0, &["FP"]),
    (1, &["AdvSIMD"]),
    (3, &["AES"]),
    (4, &["PMULL"]),
    (5, &["SHA1"]),
    (6, &["SHA256"]),
    (7, &["CRC32"]),
    (8, &["LSE"]),
    (9, &["FP16"]),
    (10, &["FP16"]),
    (12, &["RDM"]),
    (13, &["JSCVT"]),
    (14, &["FCMA"]),
    (15, &["LRCPC"]),
    (16, &["DPB"]),
    (17, &["SHA3"]),
    (18, &["SM3"]),
    (19, &["SM4"]),
    (20, &["DotProd"]),
    (21, &["SHA512"]),
    (22, &["SVE"]),
    (23, &["FHM"]),
    (24, &["DIT"]),
    (25, &["LSE2"]),
    (26, &["LRCPC2"]),
    (27, &["FlagM"]),
    (28, &["SSBS"]),
    (29, &["SB"]),
    (30, &["PAuth", "PACQARMA5"]),
    (31, &["PAuth", "PACQARMA5"]),
];

/// `AT_HWCAP2` bits and the features each one needs.
const HWCAP2: &[(u32, &[&str])] = &[
    (0, &["DPB2"]),
    (7, &["FlagM2"]),
    (8, &["FRINTTS"]),
    (13, &["I8MM"]),
    (14, &["BF16"]),
    (15, &["DGH"]),
    (16, &["RNG"]),
    (17, &["BTI"]),
];

/// The word of `table`'s bits whose features `f` all holds.
fn bits(f: Features, table: &[(u32, &[&str])]) -> u64 {
    table
        .iter()
        .filter(|(_, need)| {
            need.iter()
                .all(|n| Feature::from_name(n).is_some_and(|x| f.has(x)))
        })
        .fold(0, |w, (bit, _)| w | 1 << bit)
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_cpu_arm64::idregs::{ID_AA64ISAR0_EL1, ID_AA64ISAR1_EL1, user_id_reg};

    #[test]
    fn the_a57_reports_armv8_crypto_and_crc32_and_the_id_registers() {
        assert_eq!(GuestCpu::CortexA57.hwcap(), 0b1111_1011 | HWCAP_CPUID);
        assert_eq!(GuestCpu::CortexA57.hwcap2(), 0);
    }

    #[test]
    fn the_a78c_reports_lse_rcpc_and_pauth() {
        let h = GuestCpu::CortexA78C.hwcap();
        for bit in [8, 9, 10, 12, 15, 16, 20, 25, 26, 27, 28, 30, 31] {
            assert_ne!(h & 1 << bit, 0, "bit {bit}");
        }
        for bit in [13, 14, 17, 21, 22, 23, 24, 29] {
            assert_eq!(h & 1 << bit, 0, "bit {bit}");
        }
    }

    #[test]
    fn hwcap_and_the_id_registers_agree() {
        for c in [GuestCpu::CortexA57, GuestCpu::CortexA78C] {
            let f = c.features();
            let h = c.hwcap();
            let isar0 = user_id_reg(&f, c.midr(), ID_AA64ISAR0_EL1).unwrap();
            let isar1 = user_id_reg(&f, c.midr(), ID_AA64ISAR1_EL1).unwrap();
            let on = |bit: u32| h & 1 << bit != 0;
            assert_eq!(on(8), isar0 >> 20 & 0xf >= 2, "{c:?} ATOMICS");
            assert_eq!(on(12), isar0 >> 28 & 0xf >= 1, "{c:?} ASIMDRDM");
            assert_eq!(on(20), isar0 >> 44 & 0xf >= 1, "{c:?} ASIMDDP");
            assert_eq!(on(15), isar1 >> 20 & 0xf >= 1, "{c:?} LRCPC");
            assert_eq!(on(26), isar1 >> 20 & 0xf >= 2, "{c:?} ILRCPC");
            assert_eq!(on(16), isar1 & 0xf >= 1, "{c:?} DCPOP");
            assert_eq!(on(30), isar1 >> 4 & 0xf >= 1, "{c:?} PACA");
            assert_eq!(on(31), isar1 >> 24 & 0xf >= 1, "{c:?} PACG");
        }
    }

    #[test]
    fn the_cpu_names_are_the_project_settings() {
        assert_eq!(
            GuestCpu::from_name("cortex-a78c"),
            Some(GuestCpu::CortexA78C)
        );
        assert_eq!(GuestCpu::from_name("cortex-a57"), Some(GuestCpu::CortexA57));
        assert_eq!(GuestCpu::from_name("m1"), None);
    }
}
