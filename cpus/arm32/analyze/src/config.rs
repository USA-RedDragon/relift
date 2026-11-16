//! What a discovery run starts from and the rules it may use beyond the
//! ones always on.

use crate::loc::Loc;
use crate::report::RamCopy;

/// Discovery rules for code that needs them; all are off by default.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Options {
    /// Even words in the image are ARM-state code address candidates
    /// too: ARMv5 code calls through ARM-state pointers (`BLX` with a
    /// register, loads of the PC). A candidate's first instruction must
    /// run whatever the flags are.
    pub arm_pointers: bool,
    /// Every aligned word of the image is a code address candidate (odd:
    /// Thumb; even, with [`Options::arm_pointers`]: ARM), but the words
    /// found as instructions: pointers in tables that discovered code
    /// reaches only through other pointers. A candidate in another segment
    /// than its word must open a stack frame, and a Thumb candidate only
    /// this scan gives is rejected when its run covers ARM code.
    pub scan_words: bool,
    /// Switches: the compare that bounds a jump table may sit further back
    /// past instructions that leave the flags, and a jump to the address
    /// after it plus an index with no bound enters a table of branches
    /// there, or the code there part way (an unrolled loop).
    pub switches: bool,
    /// A call with constant arguments is run when one names the image,
    /// even when none names RAM.
    pub image_calls: bool,
    /// A call may write every local of the caller's frame it can reach:
    /// a word whose address a register or a stack word holds at the call,
    /// up to the registers the caller saved. The code after the call
    /// reads those words as unknown (a flag a callee or an interrupt
    /// clears through a pointer it was given).
    pub reachable_locals: bool,
}

/// The input of one discovery run.
#[derive(Clone, Debug, Default)]
pub struct Config {
    /// The rules beyond the ones always on.
    pub options: Options,
    /// Images a loader put in RAM, of kind [`crate::CopyKind::Load`],
    /// which the roots may name by index.
    pub loads: Vec<RamCopy>,
    /// The entries ([`crate::Origin::Entry`]).
    pub roots: Vec<Loc>,
    /// Code addresses other runs transfer to ([`crate::Report::outside`]):
    /// entries too when the code there opens a stack frame.
    pub seeds: Vec<Loc>,
    /// Code addresses a run reached that discovery did not (the hints of a
    /// stop report, [`crate::Origin::Hint`]): roots too.
    pub hints: Vec<Loc>,
}
