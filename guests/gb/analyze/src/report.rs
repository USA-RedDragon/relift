//! What the analysis found.

use std::collections::{BTreeMap, BTreeSet};

use crate::func::{AddressTaken, CallKind, CopyLoop, RamTransfer};
use crate::loc::Loc;
use crate::ramcode::RamOps;

/// Why a transfer could not be followed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
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
    /// A call through a routine that jumps to `hl`, with an address that is
    /// not constant.
    IndirectCall,
    /// A call to a routine that jumps through a table placed right after
    /// the call, and no entry of the table points to code.
    InlineTable,
    /// A jump through a table in ROM with no entry that points to code.
    JumpTable,
    /// A jump through a pointer kept in RAM whose stored values are not
    /// all known: no constant store was found, or some bytes stored there
    /// are not part of a known pointer.
    RamPointer,
    /// A jump through a pointer kept in RAM into the switchable window,
    /// while the bank at the jump is not known.
    CellBank,
    /// A callee that has to be run inside its caller, called too deep in a
    /// chain of such callees.
    ContextDepth,
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
            UnresolvedKind::IndirectCall => "indirect-call",
            UnresolvedKind::InlineTable => "inline-table",
            UnresolvedKind::JumpTable => "jump-table",
            UnresolvedKind::RamPointer => "ram-pointer",
            UnresolvedKind::CellBank => "ram-pointer-bank",
            UnresolvedKind::ContextDepth => "context-depth",
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
    /// An address the program ran in an earlier run without lifted code
    /// recorded by the runtime: code, because it ran.
    Seen,
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

/// How the length of a followed jump table is known.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum TableBound {
    /// The index is bounded by a compare, a mask or a constant.
    Index,
    /// The table is the run of entries that point to code, ending at the
    /// first entry that does not or at code.
    Run,
}

/// A jump table that was followed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct JumpTable {
    /// The jump.
    pub site: Loc,
    /// The first entry.
    pub table: Loc,
    /// Bytes from one entry to the next.
    pub step: u8,
    /// Entries followed.
    pub entries: usize,
    /// Rows of the table read, including rows left out.
    pub rows: usize,
    /// How the length is known.
    pub bound: TableBound,
}

/// A jump through a pointer kept in RAM that was followed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct RamPointerJump {
    /// The jump.
    pub site: Loc,
    /// The RAM address of the pointer's low byte.
    pub cell: u16,
    /// Targets followed: constants stored there anywhere in the program.
    pub targets: usize,
    /// Whether some values are not known.
    pub partial: bool,
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
    /// Constants loaded into a register pair that point into ROM, outside
    /// every instruction found. Some are data; some are code that is only
    /// reached through a pointer. They are not followed.
    pub unreached_pointers: Vec<AddressTaken>,
    /// Jump tables followed.
    pub tables: Vec<JumpTable>,
    /// Jumps through pointers kept in RAM that were followed.
    pub ram_pointers: Vec<RamPointerJump>,
    /// The instructions predicted to run from RAM.
    pub ram_ops: RamOps,
    /// The banks whose window code may run: banks with code found, and
    /// banks mapped at a transfer to an address that is not known.
    pub lift_banks: BTreeSet<u16>,
}
