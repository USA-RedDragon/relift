//! The CPUs the generator knows: each overlay names its profile with a
//! `$relift profile NAME` line (`vr4300` without one), which picks the
//! mnemonic table, the template classes, the lifter's register file the
//! cut checks register names against, and the Rust paths of the generated
//! files.

use relift_cpu_mips::{r3000a, vr4300};

/// A mnemonic table: by AST constructor and in clause order, the
/// mnemonic of every decode clause, with the class of its template.
type Mnemonics = &'static [(&'static str, &'static [&'static str], &'static str)];

/// One MIPS CPU.
pub struct Profile {
    /// The `$relift profile` name, also the lifter's module.
    pub name: &'static str,
    /// The name in generated documentation.
    pub title: &'static str,
    /// The Rust path of the module that holds the profile's `decode` and
    /// `lift`: the crate root for the VR4300, whose modules it re-exports.
    pub module: &'static str,
    /// The template classes the generator writes, one file each.
    pub classes: &'static [&'static str],
    /// The mnemonic of every decode clause the overlay keeps.
    pub mnemonics: Mnemonics,
    /// Whether the lifter's register file has a slot for a model register.
    pub has_slot: fn(&str) -> bool,
}

/// The VR4300 (Nintendo 64 CPU).
pub static VR4300: Profile = Profile {
    name: "vr4300",
    title: "VR4300",
    module: "crate",
    classes: &["int", "ldst", "branch", "system", "cop1"],
    mnemonics: VR4300_MNEMONICS,
    has_slot: |n| vr4300::regs::slot(n).is_some(),
};

/// The R3000A (PlayStation CPU). A stub: only the `int` class has
/// templates yet.
pub static R3000A: Profile = Profile {
    name: "r3000a",
    title: "R3000A",
    module: "crate::r3000a",
    classes: &["int"],
    mnemonics: R3000A_MNEMONICS,
    has_slot: |n| r3000a::regs::slot(n).is_some(),
};

/// The profile named `name`.
pub fn by_name(name: &str) -> Option<&'static Profile> {
    [&VR4300, &R3000A].into_iter().find(|p| p.name == name)
}

/// The profile an overlay's `$relift profile` line names.
pub fn of_overlay(text: &str) -> Result<&'static Profile, String> {
    let name = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("$relift profile "))
        .map_or("vr4300", str::trim);
    by_name(name).ok_or_else(|| format!("no profile {name}"))
}

/// The VR4300 table. A `COP1.fmt` encoding is every arithmetic,
/// conversion and compare operation of one format.
const VR4300_MNEMONICS: Mnemonics = &[
    (
        "COP1_Move",
        &["MFC1", "DMFC1", "CFC1", "MTC1", "DMTC1", "CTC1"],
        "cop1",
    ),
    ("COP1_BC", &["BC1F", "BC1T", "BC1FL", "BC1TL"], "branch"),
    (
        "COP1_Arith",
        &["COP1.S", "COP1.D", "COP1.W", "COP1.L"],
        "cop1",
    ),
    ("COP1_Mem", &["LWC1", "LDC1", "SWC1", "SDC1"], "cop1"),
    ("DADDIU", &["DADDIU"], "int"),
    ("DADDU", &["DADDU"], "int"),
    ("DADDI", &["DADDI"], "int"),
    ("DADD", &["DADD"], "int"),
    ("ADD", &["ADD"], "int"),
    ("ADDI", &["ADDI"], "int"),
    ("ADDU", &["ADDU"], "int"),
    ("ADDIU", &["ADDIU"], "int"),
    ("DSUBU", &["DSUBU"], "int"),
    ("DSUB", &["DSUB"], "int"),
    ("SUB", &["SUB"], "int"),
    ("SUBU", &["SUBU"], "int"),
    ("AND", &["AND"], "int"),
    ("ANDI", &["ANDI"], "int"),
    ("OR", &["OR"], "int"),
    ("ORI", &["ORI"], "int"),
    ("NOR", &["NOR"], "int"),
    ("XOR", &["XOR"], "int"),
    ("XORI", &["XORI"], "int"),
    ("LUI", &["LUI"], "int"),
    ("DSLL", &["DSLL"], "int"),
    ("DSLL32", &["DSLL32"], "int"),
    ("DSLLV", &["DSLLV"], "int"),
    ("DSRA", &["DSRA"], "int"),
    ("DSRA32", &["DSRA32"], "int"),
    ("DSRAV", &["DSRAV"], "int"),
    ("DSRL", &["DSRL"], "int"),
    ("DSRL32", &["DSRL32"], "int"),
    ("DSRLV", &["DSRLV"], "int"),
    ("SLL", &["SLL"], "int"),
    ("SLLV", &["SLLV"], "int"),
    ("SRA", &["SRA"], "int"),
    ("SRAV", &["SRAV"], "int"),
    ("SRL", &["SRL"], "int"),
    ("SRLV", &["SRLV"], "int"),
    ("SLT", &["SLT"], "int"),
    ("SLTI", &["SLTI"], "int"),
    ("SLTU", &["SLTU"], "int"),
    ("SLTIU", &["SLTIU"], "int"),
    ("MFHI", &["MFHI"], "int"),
    ("MFLO", &["MFLO"], "int"),
    ("MTHI", &["MTHI"], "int"),
    ("MTLO", &["MTLO"], "int"),
    ("MULT", &["MULT"], "int"),
    ("MULTU", &["MULTU"], "int"),
    ("DMULT", &["DMULT"], "int"),
    ("DMULTU", &["DMULTU"], "int"),
    ("DIV", &["DIV"], "int"),
    ("DIVU", &["DIVU"], "int"),
    ("DDIV", &["DDIV"], "int"),
    ("DDIVU", &["DDIVU"], "int"),
    ("J", &["J"], "branch"),
    ("JAL", &["JAL"], "branch"),
    ("JR", &["JR"], "branch"),
    ("JALR", &["JALR"], "branch"),
    ("BEQ", &["BEQ", "BEQL", "BNE", "BNEL"], "branch"),
    (
        "BCMPZ",
        &[
            "BLTZ", "BLTZAL", "BLTZL", "BLTZALL", "BGEZ", "BGEZAL", "BGEZL", "BGEZALL", "BGTZ",
            "BGTZL", "BLEZ", "BLEZL",
        ],
        "branch",
    ),
    ("SYSCALL", &["SYSCALL"], "system"),
    ("BREAK", &["BREAK"], "system"),
    (
        "TRAPREG",
        &["TGE", "TGEU", "TLT", "TLTU", "TEQ", "TNE"],
        "system",
    ),
    (
        "TRAPIMM",
        &["TEQI", "TNEI", "TGEI", "TGEIU", "TLTI", "TLTIU"],
        "system",
    ),
    (
        "Load",
        &["LB", "LBU", "LH", "LHU", "LW", "LWU", "LD", "LL", "LLD"],
        "ldst",
    ),
    ("Store", &["SB", "SH", "SW", "SD", "SC", "SCD"], "ldst"),
    ("LWL", &["LWL"], "ldst"),
    ("LWR", &["LWR"], "ldst"),
    ("SWL", &["SWL"], "ldst"),
    ("SWR", &["SWR"], "ldst"),
    ("LDL", &["LDL"], "ldst"),
    ("LDR", &["LDR"], "ldst"),
    ("SDL", &["SDL"], "ldst"),
    ("SDR", &["SDR"], "ldst"),
    ("CACHE", &["CACHE"], "system"),
    ("SYNC", &["SYNC"], "system"),
    ("MFC0", &["MFC0", "DMFC0"], "system"),
    ("MTC0", &["MTC0", "DMTC0"], "system"),
    ("TLBWI", &["TLBWI"], "system"),
    ("TLBWR", &["TLBWR"], "system"),
    ("TLBR", &["TLBR"], "system"),
    ("TLBP", &["TLBP"], "system"),
    ("ERET", &["ERET"], "system"),
];

/// The R3000A table: MIPS I without COP1 (the overlay drops the MIPS II
/// and III clauses).
const R3000A_MNEMONICS: Mnemonics = &[
    ("ADD", &["ADD"], "int"),
    ("ADDI", &["ADDI"], "int"),
    ("ADDU", &["ADDU"], "int"),
    ("ADDIU", &["ADDIU"], "int"),
    ("SUB", &["SUB"], "int"),
    ("SUBU", &["SUBU"], "int"),
    ("AND", &["AND"], "int"),
    ("ANDI", &["ANDI"], "int"),
    ("OR", &["OR"], "int"),
    ("ORI", &["ORI"], "int"),
    ("NOR", &["NOR"], "int"),
    ("XOR", &["XOR"], "int"),
    ("XORI", &["XORI"], "int"),
    ("LUI", &["LUI"], "int"),
    ("SLL", &["SLL"], "int"),
    ("SLLV", &["SLLV"], "int"),
    ("SRA", &["SRA"], "int"),
    ("SRAV", &["SRAV"], "int"),
    ("SRL", &["SRL"], "int"),
    ("SRLV", &["SRLV"], "int"),
    ("SLT", &["SLT"], "int"),
    ("SLTI", &["SLTI"], "int"),
    ("SLTU", &["SLTU"], "int"),
    ("SLTIU", &["SLTIU"], "int"),
    ("MFHI", &["MFHI"], "int"),
    ("MFLO", &["MFLO"], "int"),
    ("MTHI", &["MTHI"], "int"),
    ("MTLO", &["MTLO"], "int"),
    ("MULT", &["MULT"], "int"),
    ("MULTU", &["MULTU"], "int"),
    ("DIV", &["DIV"], "int"),
    ("DIVU", &["DIVU"], "int"),
    ("J", &["J"], "branch"),
    ("JAL", &["JAL"], "branch"),
    ("JR", &["JR"], "branch"),
    ("JALR", &["JALR"], "branch"),
    ("BEQ", &["BEQ", "BNE"], "branch"),
    (
        "BCMPZ",
        &["BLTZ", "BLTZAL", "BGEZ", "BGEZAL", "BGTZ", "BLEZ"],
        "branch",
    ),
    ("SYSCALL", &["SYSCALL"], "system"),
    ("BREAK", &["BREAK"], "system"),
    ("Load", &["LB", "LBU", "LH", "LHU", "LW"], "ldst"),
    ("Store", &["SB", "SH", "SW"], "ldst"),
    ("LWL", &["LWL"], "ldst"),
    ("LWR", &["LWR"], "ldst"),
    ("SWL", &["SWL"], "ldst"),
    ("SWR", &["SWR"], "ldst"),
    ("MFC0", &["MFC0"], "system"),
    ("MTC0", &["MTC0"], "system"),
];
