//! What the analysis found.

use std::collections::BTreeMap;

use crate::func::{CallKind, CopyLoop, RamTransfer};
use crate::loc::Loc;

/// Why a transfer could not be followed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum UnresolvedKind {
    /// A target in `$4000-$7FFF` while the mapped bank is not known.
    UnknownBank,
    /// The mapped bank is not in the image.
    BankOutOfRange,
    /// `jp hl` with a value that is not constant.
    IndirectJump,
    /// A write to a bank register of a value that is not constant; kept out
    /// of [`Report::unresolved`].
    DynamicBankWrite,
    /// A call through a bank-switch routine whose bank argument is not
    /// constant.
    TrampolineBank,
    /// A call through a routine that jumps to `hl` whose address is not
    /// constant.
    TrampolineTarget,
    /// An unused opcode.
    Illegal,
    /// An instruction runs past the end of its bank window.
    RunsOffWindow,
    /// A location the image does not hold.
    OutsideRom,
}

impl UnresolvedKind {
    /// A short stable name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            UnresolvedKind::UnknownBank => "unknown-bank",
            UnresolvedKind::BankOutOfRange => "bank-out-of-range",
            UnresolvedKind::IndirectJump => "indirect-jump",
            UnresolvedKind::DynamicBankWrite => "dynamic-bank-write",
            UnresolvedKind::TrampolineBank => "trampoline-bank",
            UnresolvedKind::TrampolineTarget => "trampoline-target",
            UnresolvedKind::Illegal => "illegal-opcode",
            UnresolvedKind::RunsOffWindow => "runs-off-window",
            UnresolvedKind::OutsideRom => "outside-rom",
        }
    }
}

/// A transfer or bank write the analysis could not resolve.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Unresolved {
    /// The instruction.
    pub at: Loc,
    /// Why.
    pub kind: UnresolvedKind,
    /// More detail for a person.
    pub detail: String,
}

/// How a function was found.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Origin {
    /// The cartridge entry point or an interrupt or restart vector.
    Root,
    /// The target of a call, restart, trampoline or `jp hl` helper.
    Call(CallKind),
}

/// One function.
#[derive(Clone, Debug)]
pub struct Function {
    /// Where it starts.
    pub entry: Loc,
    /// How it was found.
    pub origin: Origin,
    /// Instruction locations with their lengths.
    pub body: BTreeMap<Loc, u8>,
    /// Distinct bytes the body covers.
    pub bytes: usize,
    /// Whether it writes a bank register itself or through a callee.
    pub switches_bank: bool,
    /// What it does when called as a bank-switch routine.
    pub trampoline: Option<Trampoline>,
}

/// The argument registers of a bank-switch routine.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Trampoline {
    /// The register holding the bank, as a name.
    pub bank_reg: Option<char>,
    /// The register pair holding the target address.
    pub target_regs: Option<(char, char)>,
}

/// An edge between functions.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Edge {
    /// The calling instruction.
    pub from: Loc,
    /// The callee entry.
    pub to: Loc,
    /// How it was reached.
    pub kind: CallKind,
}

/// Coverage of one bank.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BankCoverage {
    /// Functions whose entry is in the bank.
    pub functions: usize,
    /// Bytes of the bank that decode as reached code.
    pub bytes: usize,
}

/// A transfer into RAM and the copy loops that might have put code there.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RamCode {
    /// The transfer.
    pub transfer: RamTransfer,
    /// The function that makes it.
    pub function: Loc,
    /// Copy loops, anywhere in the ROM, whose destination constants could
    /// hold the target.
    pub copied_by: Vec<(Loc, CopyLoop)>,
}

/// The result of analyzing an image.
#[derive(Clone, Debug)]
pub struct Report {
    /// The mapper name.
    pub mapper: &'static str,
    /// The number of ROM banks.
    pub banks: usize,
    /// All functions, by entry.
    pub functions: BTreeMap<Loc, Function>,
    /// Edges found.
    pub edges: Vec<Edge>,
    /// Coverage per bank.
    pub coverage: BTreeMap<u16, BankCoverage>,
    /// Transfers that could not be resolved.
    pub unresolved: Vec<Unresolved>,
    /// Bank register writes of a value that is not constant, such as the
    /// restore of a saved bank. They matter only when a later transfer into
    /// the window needs the bank, and that transfer is listed in
    /// `unresolved`.
    pub dynamic_writes: Vec<Loc>,
    /// Transfers into RAM.
    pub ram_code: Vec<RamCode>,
}
