//! Which template an instruction's bytes select: the prefix page and the
//! opcode byte.

use core::fmt;

/// A template group. The discriminant is the group number of the
/// generated table (`cpus/z80/gen`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Group {
    /// No prefix.
    Base = 0,
    /// `CB`.
    Cb = 1,
    /// `ED`.
    Ed = 2,
    /// `DD`: IX in place of HL.
    Dd = 3,
    /// `FD`: IY in place of HL.
    Fd = 4,
    /// `DD CB d op`.
    Ddcb = 5,
    /// `FD CB d op`.
    Fdcb = 6,
    /// The interrupt entries; no bytes select them.
    Accept = 7,
    /// A `DD` or `FD` followed by another prefix: one 4 T-state opcode
    /// fetch, after which the next byte starts a new instruction.
    Lone = 8,
}

/// Why bytes are not an instruction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeError {
    /// The bytes end before the opcode byte.
    Short,
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            DecodeError::Short => f.write_str("the bytes end inside an instruction"),
        }
    }
}

impl core::error::Error for DecodeError {}

/// The group and opcode byte of the instruction at the start of `bytes`.
/// Every byte sequence is an instruction: the Z80 has no undefined opcode
/// (the undefined `ED` codes are 8 T-state no-ops).
///
/// # Errors
///
/// The bytes end before the opcode byte.
pub fn decode(bytes: &[u8]) -> Result<(Group, u8), DecodeError> {
    let at = |i: usize| bytes.get(i).copied().ok_or(DecodeError::Short);
    let b0 = at(0)?;
    Ok(match b0 {
        0xCB => (Group::Cb, at(1)?),
        0xED => (Group::Ed, at(1)?),
        0xDD | 0xFD => {
            let ix = b0 == 0xDD;
            match at(1)? {
                0xCB => (if ix { Group::Ddcb } else { Group::Fdcb }, at(3)?),
                0xDD | 0xFD | 0xED => (Group::Lone, b0),
                c => (if ix { Group::Dd } else { Group::Fd }, c),
            }
        }
        c => (Group::Base, c),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pages() {
        assert_eq!(decode(&[0x3C]), Ok((Group::Base, 0x3C)));
        assert_eq!(decode(&[0xCB, 0x7E]), Ok((Group::Cb, 0x7E)));
        assert_eq!(decode(&[0xED, 0xB0]), Ok((Group::Ed, 0xB0)));
        assert_eq!(decode(&[0xDD, 0x21, 0, 0]), Ok((Group::Dd, 0x21)));
        assert_eq!(decode(&[0xFD, 0x34, 5]), Ok((Group::Fd, 0x34)));
        assert_eq!(decode(&[0xDD, 0xCB, 5, 0x06]), Ok((Group::Ddcb, 0x06)));
        assert_eq!(decode(&[0xFD, 0xCB, 0xFF, 0xFE]), Ok((Group::Fdcb, 0xFE)));
        assert_eq!(decode(&[0xDD, 0xFD, 0x21]), Ok((Group::Lone, 0xDD)));
        assert_eq!(decode(&[0xFD, 0xED, 0xB0]), Ok((Group::Lone, 0xFD)));
    }

    #[test]
    fn short() {
        assert_eq!(decode(&[]), Err(DecodeError::Short));
        assert_eq!(decode(&[0xCB]), Err(DecodeError::Short));
        assert_eq!(decode(&[0xDD, 0xCB, 5]), Err(DecodeError::Short));
    }
}
