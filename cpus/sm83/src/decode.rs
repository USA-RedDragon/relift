use crate::table::{BASE, CB};
use crate::types::{Mnemonic, Opcode, Operand};

/// The immediate bytes that follow an opcode, already assembled.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Immediate {
    /// The instruction has no immediate.
    None,
    /// An unsigned 8-bit immediate. STOP carries its padding byte here.
    U8(u8),
    /// A signed 8-bit immediate.
    I8(i8),
    /// A little-endian 16-bit immediate.
    U16(u16),
}

/// Why decoding failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeError {
    /// The input ends before the instruction does.
    Truncated {
        /// How many bytes the instruction needs.
        needed: u8,
    },
}

impl core::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            DecodeError::Truncated { needed } => {
                write!(
                    f,
                    "instruction needs {needed} bytes but the input is shorter"
                )
            }
        }
    }
}

impl core::error::Error for DecodeError {}

/// One decoded instruction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Instruction {
    /// The table entry.
    pub opcode: &'static Opcode,
    /// The immediate, if the entry has one.
    pub immediate: Immediate,
}

impl Instruction {
    /// The length in bytes.
    pub const fn length(&self) -> u8 {
        self.opcode.length
    }

    /// The address a branch goes to when taken, for an instruction located at
    /// `address`. JP HL, RET and RETI have no static target and return `None`.
    pub fn branch_target(&self, address: u16) -> Option<u16> {
        match (self.opcode.mnemonic, self.immediate) {
            (Mnemonic::Jr, Immediate::I8(offset)) => Some(
                address
                    .wrapping_add(u16::from(self.opcode.length))
                    .wrapping_add_signed(i16::from(offset)),
            ),
            (Mnemonic::Jp | Mnemonic::Call, Immediate::U16(target)) => Some(target),
            (Mnemonic::Rst, _) => match self.opcode.dst {
                Operand::Vector(vector) => Some(u16::from(vector)),
                _ => None,
            },
            _ => None,
        }
    }
}

/// Decodes the instruction at the start of `bytes`.
///
/// The 11 unused opcodes decode to an entry whose mnemonic is
/// [`Mnemonic::Illegal`]; they are not an error, so a scan can report them.
/// A `STOP` that is the last byte of `bytes`, as one at the end of a ROM
/// window is, decodes with a padding byte of 0: no execution reads it as an
/// operand.
pub fn decode(bytes: &[u8]) -> Result<Instruction, DecodeError> {
    let first = *bytes.first().ok_or(DecodeError::Truncated { needed: 1 })?;
    let (opcode, immediate_at) = if first == 0xCB {
        let second = *bytes.get(1).ok_or(DecodeError::Truncated { needed: 2 })?;
        (&CB[usize::from(second)], 2)
    } else {
        (&BASE[usize::from(first)], 1)
    };
    let needed = opcode.length;
    if opcode.mnemonic == Mnemonic::Stop && bytes.len() == 1 {
        return Ok(Instruction {
            opcode,
            immediate: Immediate::U8(0),
        });
    }
    if bytes.len() < usize::from(needed) {
        return Err(DecodeError::Truncated { needed });
    }
    let immediate = match (opcode.src, opcode.dst) {
        (Operand::Imm16 | Operand::IndirectImm16, _) | (_, Operand::IndirectImm16) => {
            Immediate::U16(u16::from_le_bytes([
                bytes[immediate_at],
                bytes[immediate_at + 1],
            ]))
        }
        (Operand::SignedImm8 | Operand::SpPlusSigned8, _) => {
            Immediate::I8(bytes[immediate_at] as i8)
        }
        (Operand::Imm8 | Operand::HighImm8, _) | (_, Operand::HighImm8) => {
            Immediate::U8(bytes[immediate_at])
        }
        _ if opcode.mnemonic == Mnemonic::Stop => Immediate::U8(bytes[immediate_at]),
        _ => Immediate::None,
    };
    Ok(Instruction { opcode, immediate })
}
