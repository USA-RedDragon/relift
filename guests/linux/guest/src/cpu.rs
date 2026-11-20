//! The CPU a program runs on and the capabilities the kernel reports for
//! it.
//!
//! The runtime's CPU is the one relift lifts for: Armv8.0-A with the
//! Cryptographic Extension and CRC32, which is what the auxiliary vector's
//! `AT_HWCAP` must say, since libc start-up code picks its routines from it
//! (atomics, string functions, crypto) and a program that finds a feature
//! there uses it. An instruction of a feature the CPU lacks lifts as
//! undefined, as on the hardware.

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::{Feature, Features};

/// The features of the CPU: a Cortex-A57.
pub const FEATURES: Features = Features::CORTEX_A57;

/// The address bits of a process, `TCR_EL1`'s T0SZ of 25, without
/// top-byte ignore for instruction fetches.
pub const VA: Va = Va::BITS39;

/// `HWCAP_FP`.
const HWCAP_FP: u64 = 1 << 0;
/// `HWCAP_ASIMD`.
const HWCAP_ASIMD: u64 = 1 << 1;
/// `HWCAP_AES`.
const HWCAP_AES: u64 = 1 << 3;
/// `HWCAP_PMULL`.
const HWCAP_PMULL: u64 = 1 << 4;
/// `HWCAP_SHA1`.
const HWCAP_SHA1: u64 = 1 << 5;
/// `HWCAP_SHA2`.
const HWCAP_SHA2: u64 = 1 << 6;
/// `HWCAP_CRC32`.
const HWCAP_CRC32: u64 = 1 << 7;
/// `HWCAP_CPUID`: `MRS` of the ID registers at EL0 is emulated by the kernel.
pub const HWCAP_CPUID: u64 = 1 << 11;

/// The CPU to lift for.
pub fn cpu() -> Cpu {
    Cpu::new(FEATURES, VA)
}

/// The `AT_HWCAP` word of the CPU's features, the way the kernel's
/// `arch/arm64/include/uapi/asm/hwcap.h` numbers them.
pub fn hwcap() -> u64 {
    [
        (Feature::FP, HWCAP_FP),
        (Feature::AdvSIMD, HWCAP_ASIMD),
        (Feature::AES, HWCAP_AES),
        (Feature::PMULL, HWCAP_PMULL),
        (Feature::SHA1, HWCAP_SHA1),
        (Feature::SHA256, HWCAP_SHA2),
        (Feature::CRC32, HWCAP_CRC32),
    ]
    .into_iter()
    .filter(|(f, _)| FEATURES.has(*f))
    .fold(0, |w, (_, bit)| w | bit)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_hardware_capabilities_are_the_cpus_features() {
        assert_eq!(hwcap(), 0b1111_1011);
    }
}
