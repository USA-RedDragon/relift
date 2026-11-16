//! The CPU a program runs on, picked per program from its NPDM: the
//! Switch's Cortex-A57 or the Switch 2's Cortex-A78C, and the address
//! layout the kernel gives the process.
//!
//! The NPDM names no console. A program is a Switch 2 program when its
//! kernel capabilities grant an SVC at or above [`FIRST_SWITCH2_SVC`]: the
//! Switch's kernel defines SVCs below 0x80 only, and Switch 2 programs are
//! granted 0x80 (a GPU memory call). A Switch program run on a Switch 2
//! keeps its Switch NPDM and runs on the Cortex-A78C, but its code was
//! built for the Cortex-A57, whose hint-space pointer authentication
//! instructions are no-ops; it lifts as a Switch program, as it was
//! compiled. A program without an NPDM (homebrew) is a Switch program
//! unless the project names the console.

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::Features;
use relift_switch_formats::npdm::{AddressSpace, Npdm};

/// The lowest SVC number the Switch's kernel does not define and the
/// Switch 2's grants to its programs.
pub const FIRST_SWITCH2_SVC: u32 = 0x80;

/// The console a program is built for.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Console {
    /// The Switch: a Cortex-A57, Armv8.0-A with AES, PMULL, SHA1, SHA256
    /// and CRC32. Words of newer encodings are unallocated on it and lift
    /// to an undefined-instruction exception.
    Switch,
    /// The Switch 2: a Cortex-A78C, Armv8.2-A with pointer authentication
    /// (PAuth2, QARMA5, no FPAC).
    Switch2,
}

impl Console {
    /// The name `game.toml` uses: `switch` or `switch2`.
    pub fn name(self) -> &'static str {
        match self {
            Console::Switch => "switch",
            Console::Switch2 => "switch2",
        }
    }

    /// The console named `name` ([`Console::name`]).
    pub fn from_name(name: &str) -> Option<Console> {
        match name {
            "switch" => Some(Console::Switch),
            "switch2" => Some(Console::Switch2),
            _ => None,
        }
    }

    /// The architecture features of the console's CPU.
    pub const fn features(self) -> Features {
        match self {
            Console::Switch => Features::CORTEX_A57,
            Console::Switch2 => Features::CORTEX_A78C,
        }
    }

    /// The console the NPDM `npdm` was built for: the Switch 2 when it
    /// grants an SVC at or above [`FIRST_SWITCH2_SVC`].
    pub fn of(npdm: &Npdm) -> Console {
        if npdm.svcs.iter().any(|n| n >= FIRST_SWITCH2_SVC) {
            Console::Switch2
        } else {
            Console::Switch
        }
    }
}

/// The address layout of a program with the NPDM `npdm`: Horizon's kernel
/// gives every 64-bit process a 39-bit address space without top-byte
/// ignore (`TCR_EL1` T0SZ and T1SZ 25, TBI0 and TBI1 zero), whichever
/// layout its address space type picks inside it (36 or 39 bits). A
/// program without an NPDM is 64-bit.
///
/// # Errors
///
/// A 32-bit program: its code is AArch32, which relift does not lift.
pub fn va(npdm: Option<&Npdm>) -> Result<Va, String> {
    match npdm {
        None => Ok(Va::BITS39),
        Some(n) => match n.address_space {
            AddressSpace::As64Bit | AddressSpace::As64BitOld if n.is_64bit => Ok(Va::BITS39),
            s => Err(format!(
                "the NPDM asks for a {} address space{}: relift lifts 64-bit programs only",
                s.name(),
                if n.is_64bit { "" } else { " and 32-bit code" }
            )),
        },
    }
}

/// The CPU of a program with the NPDM `npdm`: the console's features
/// ([`Console::of`], or `console` when the project names one) and the
/// address layout of [`va`].
///
/// # Errors
///
/// As [`va`].
pub fn cpu(npdm: Option<&Npdm>, console: Option<Console>) -> Result<Cpu, String> {
    Ok(Cpu::new(
        program_console(npdm, console).features(),
        va(npdm)?,
    ))
}

/// The console a program with the NPDM `npdm` runs on: `console` when the
/// project names one, else [`Console::of`] the NPDM, else the Switch.
pub fn program_console(npdm: Option<&Npdm>, console: Option<Console>) -> Console {
    console.unwrap_or_else(|| npdm.map_or(Console::Switch, Console::of))
}

#[cfg(test)]
mod tests {
    use relift_cpu_arm64::decode::Feature;
    use relift_switch_formats::npdm::{build, build_with};

    use super::*;

    fn npdm(svcs: &[u32]) -> Npdm {
        Npdm::parse(&build_with(
            "p",
            0x0100_0000_0000_1000,
            0x1000,
            &[],
            Some((svcs, svcs)),
        ))
        .expect("parses")
    }

    #[test]
    fn the_switch_cpu_is_armv8_0_with_crypto_and_crc32() {
        let names: Vec<&str> = Console::Switch
            .features()
            .iter()
            .map(Feature::name)
            .collect();
        assert_eq!(
            names,
            [
                "FEAT_AES",
                "FEAT_AdvSIMD",
                "FEAT_CRC32",
                "FEAT_FP",
                "FEAT_PMULL",
                "FEAT_SHA1",
                "FEAT_SHA256"
            ]
        );
    }

    #[test]
    fn a_grant_of_svc_0x80_makes_a_switch_2_program() {
        assert_eq!(Console::of(&npdm(&[0x01, 0x27, 0x5f])), Console::Switch);
        assert_eq!(Console::of(&npdm(&[0x01, 0x27, 0x80])), Console::Switch2);
        let c = cpu(Some(&npdm(&[0x80])), None).expect("64-bit");
        assert!(c.features.has(Feature::PAuth));
        assert_eq!(c.va, Va::BITS39);
        let c = cpu(Some(&npdm(&[0x27])), None).expect("64-bit");
        assert_eq!(c.features, Features::CORTEX_A57);
    }

    #[test]
    fn the_project_setting_overrides_the_npdm() {
        let c = cpu(None, Some(Console::Switch2)).expect("64-bit");
        assert_eq!(c.features, Features::CORTEX_A78C);
        let c = cpu(Some(&npdm(&[0x80])), Some(Console::Switch)).expect("64-bit");
        assert_eq!(c.features, Features::CORTEX_A57);
        assert_eq!(
            cpu(None, None).expect("64-bit").features,
            Features::CORTEX_A57
        );
        for c in [Console::Switch, Console::Switch2] {
            assert_eq!(Console::from_name(c.name()), Some(c));
        }
    }

    #[test]
    fn a_32_bit_program_has_no_arm64_cpu() {
        let mut b = build("p", 1, 0x1000, &[]);
        b[0xC] = 0;
        let n = Npdm::parse(&b).expect("parses");
        assert!(va(Some(&n)).is_err());
        b[0xC] = 1 | (1 << 1);
        let n = Npdm::parse(&b).expect("parses");
        assert_eq!(va(Some(&n)), Ok(Va::BITS39));
    }
}
