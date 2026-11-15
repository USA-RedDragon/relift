//! The Switch guest, over `relift-switch-formats` and `relift-cpu-arm64`.

mod eh;
pub mod program;

use relift_cpu_arm64::decode::Features;

/// The architecture features of the Switch's CPU, a Cortex-A57: Armv8.0-A
/// with AES, PMULL, SHA1, SHA256 and CRC32. Words of newer encodings are
/// unallocated on it and lift to an undefined-instruction exception.
pub const CPU_FEATURES: Features = Features::CORTEX_A57;

#[cfg(test)]
mod tests {
    use relift_cpu_arm64::decode::Feature;

    use super::CPU_FEATURES;

    #[test]
    fn the_cpu_is_armv8_0_with_crypto_and_crc32() {
        let names: Vec<&str> = CPU_FEATURES.iter().map(Feature::name).collect();
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
}
