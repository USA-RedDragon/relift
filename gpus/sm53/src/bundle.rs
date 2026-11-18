//! Maxwell code in 32-byte bundles: one scheduling word, then three
//! instructions. A code offset counts bytes from the start of the code,
//! scheduling words included, as the branch targets do.

use relift_gpu_sm86::Sched;

/// Bytes in one bundle.
pub const BUNDLE: usize = 32;

/// Whether the word at code offset `at` is a scheduling word.
pub fn is_sched(at: u64) -> bool {
    at.is_multiple_of(BUNDLE as u64)
}

/// The scheduling fields of instruction `slot` (0 to 2) of a bundle whose
/// scheduling word is `ctl`: 21 bits per instruction, the stall count
/// (bits 0 to 3), yield (4), the write and read barriers (5 to 7 and 8 to
/// 10, 7 for none), the wait mask (11 to 16) and the reuse flags (17 to 20),
/// as Mesa NAK's SM50 encoder (sm50.rs, MIT) packs them.
pub fn sched(ctl: u64, slot: usize) -> Sched {
    let f = (ctl >> (21 * slot)) & 0x1f_ffff;
    let get = |lo: u32, len: u32| ((f >> lo) & ((1 << len) - 1)) as u8;
    Sched {
        stall: get(0, 4),
        yield_: get(4, 1) == 1,
        wr_bar: get(5, 3),
        rd_bar: get(8, 3),
        wait_mask: get(11, 6),
        reuse: get(17, 4),
    }
}

/// One instruction of a code stream.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Slot {
    /// Code offset of the instruction.
    pub at: u64,
    /// The instruction word.
    pub word: u64,
    /// Its scheduling fields.
    pub sched: Sched,
}

/// The instructions of `code` (whole bundles; a partial last bundle is
/// ignored), in order.
pub fn slots(code: &[u8]) -> impl Iterator<Item = Slot> + '_ {
    code.as_chunks::<BUNDLE>()
        .0
        .iter()
        .enumerate()
        .flat_map(|(b, c)| {
            let word =
                |k: usize| u64::from_le_bytes(c[8 * k..8 * k + 8].try_into().unwrap_or([0; 8]));
            let ctl = word(0);
            (0..3).map(move |s| Slot {
                at: (b * BUNDLE + 8 + 8 * s) as u64,
                word: word(1 + s),
                sched: sched(ctl, s),
            })
        })
}
