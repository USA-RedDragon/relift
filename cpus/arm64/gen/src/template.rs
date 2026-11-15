//! The A64 side of template generation: an AARCHMRS encoding as a
//! [`relift_sail::stage::Form`], with the A64 decode groups of the Sail
//! top-level decoder and the A64 decoder as the reachability check.

use relift_cpu_arm64::decode::Encoding;
use relift_sail::model::Model;
use relift_sail::stage::{self, Form, Template};

use crate::cut::Arm64;

/// The decode group of a word, as in the Sail top-level decoder.
pub fn group_of(word: u32) -> Option<&'static str> {
    let op0 = (word >> 25) & 0xf;
    Some(match op0 {
        0 if word >> 31 == 1 => "__DecodeA64_SME",
        0 => "__DecodeA64_Reserved",
        1 => "__DecodeA64_Unallocated1",
        2 => "__DecodeA64_SVE",
        3 => "__DecodeA64_Unallocated2",
        8 | 9 => "__DecodeA64_DataProcImm",
        10 | 11 => "__DecodeA64_BranchExcSys",
        _ if op0 & 0b0101 == 0b0100 => "__DecodeA64_LoadStore",
        _ if op0 & 0b0111 == 0b0101 => "__DecodeA64_DataProcReg",
        _ if op0 & 0b0111 == 0b0111 => "__DecodeA64_DataProcFPSIMD",
        _ => return None,
    })
}

struct Arm64Form(Encoding);

impl Form for Arm64Form {
    fn name(&self) -> String {
        self.0.name().to_string()
    }
    fn mask(&self) -> u32 {
        self.0.mask()
    }
    fn value(&self) -> u32 {
        self.0.value()
    }
    fn group(&self) -> Option<&'static str> {
        group_of(self.0.value())
    }
    /// Whether some word with the `known` bits `value` decodes to the
    /// encoding.
    fn reachable(&self, known: u32, value: u32) -> bool {
        let mut x: u32 = 0x9e37_79b9 ^ value;
        for _ in 0..512 {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            let w = value | (x & !known);
            if relift_cpu_arm64::decode::decode(w).map(|i| i.encoding) == Some(self.0) {
                return true;
            }
        }
        false
    }
}

/// Generates the template of `enc`.
pub fn template(m: &Model, enc: Encoding) -> Result<Template, String> {
    stage::template::<Arm64>(m, &Arm64Form(enc))
}
