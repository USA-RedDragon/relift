//! The code of a ROM as the lift takes it: every instruction by location,
//! with its length.
//!
//! A location is a CPU address and where its bytes come from: a ROM bank,
//! or the bytes of the instruction in work RAM. This is the shape of the
//! instructions of the discovery report (`relift-sms-analyze`); a
//! development run can also take them from the executed instructions of an
//! ares trace ([`Code::from_exec_tsv`]).

use std::collections::BTreeMap;

/// Where the bytes of an instruction come from.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum CodeSrc {
    /// A 16 KiB ROM bank.
    Rom {
        /// The bank number, reduced to the ROM's size.
        bank: u32,
    },
    /// Work RAM holding these instruction bytes.
    Ram {
        /// The instruction's bytes.
        bytes: Vec<u8>,
    },
}

/// A code location: a CPU address and the bytes there.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Loc {
    /// The CPU address.
    pub addr: u16,
    /// Where its bytes come from.
    pub src: CodeSrc,
}

impl Loc {
    /// A location in ROM bank `bank`.
    #[must_use]
    pub fn rom(addr: u16, bank: u32) -> Loc {
        Loc {
            addr,
            src: CodeSrc::Rom { bank },
        }
    }

    /// The 16 KiB slot of the CPU address.
    #[must_use]
    pub fn slot(&self) -> u8 {
        (self.addr >> 14) as u8
    }
}

/// The instructions to lift.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Code {
    /// Every instruction, with its length.
    pub instructions: BTreeMap<Loc, u8>,
}

/// A line of an exec trace that does not parse.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TraceError {
    /// The line, counted from 1.
    pub line: usize,
    /// What is wrong.
    pub message: String,
}

impl std::fmt::Display for TraceError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "line {}: {}", self.line, self.message)
    }
}

impl std::error::Error for TraceError {}

impl Code {
    /// The executed Z80 instructions of an ares trace (`exec.tsv`: cpu, pc,
    /// region, offset, first frame, count, bytes): a ROM instruction is
    /// in the bank of its file offset, less `header` copier bytes; a RAM
    /// instruction has the bytes the trace shows. Every length comes from
    /// the decoder. For development runs only: the build lifts the
    /// discovery report.
    ///
    /// # Errors
    ///
    /// A line has too few fields or a number that does not parse.
    pub fn from_exec_tsv(text: &str, header: u32) -> Result<Code, TraceError> {
        let mut out = Code::default();
        for (i, line) in text.lines().enumerate() {
            let f: Vec<&str> = line.split('\t').collect();
            if i == 0 || f.first() != Some(&"z80") {
                continue;
            }
            let bad = |message: &str| TraceError {
                line: i + 1,
                message: message.into(),
            };
            if f.len() < 7 {
                return Err(bad("too few fields"));
            }
            let pc = u16::from_str_radix(f[1], 16).map_err(|_| bad("bad pc"))?;
            let bytes: Vec<u8> = (0..f[6].len() / 2)
                .map(|k| u8::from_str_radix(&f[6][2 * k..2 * k + 2], 16))
                .collect::<Result<_, _>>()
                .map_err(|_| bad("bad bytes"))?;
            let Some(len) = length(&bytes, pc) else {
                continue;
            };
            let src = match f[2] {
                "rom" => {
                    let off = u32::from_str_radix(f[3], 16).map_err(|_| bad("bad offset"))?;
                    CodeSrc::Rom {
                        bank: off.saturating_sub(header) / 0x4000,
                    }
                }
                "ram" => CodeSrc::Ram {
                    bytes: bytes[..usize::from(len).min(bytes.len())].to_vec(),
                },
                _ => continue,
            };
            out.instructions.insert(Loc { addr: pc, src }, len);
        }
        Ok(out)
    }
}

/// The length of the instruction at the start of `bytes`, from the lifter.
#[must_use]
pub fn length(bytes: &[u8], pc: u16) -> Option<u8> {
    let opts = relift_cpu_z80::Options::new(relift_ir::IoMap::memory());
    relift_cpu_z80::lift_bytes(bytes, u64::from(pc), &opts)
        .ok()
        .and_then(|l| u8::try_from(l.len).ok())
        .filter(|&n| n > 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_exec_trace_gives_rom_banks_and_ram_bytes() {
        let tsv = "cpu\tpc\tregion\toffset\tfirst_frame\tcount\tbytes\n\
                   z80\t000000\trom\t200\t0\t1\tc3602cc5\n\
                   z80\t008000\trom\t8200\t3\t2\tcd0040ff\n\
                   z80\t00c100\tram\t100\t5\t9\t2103c034\n\
                   z80\t00c103\tram\t103\t5\t9\t34c9ffff\n";
        let code = Code::from_exec_tsv(tsv, 0x200).expect("parses");
        let got: Vec<(Loc, u8)> = code.instructions.into_iter().collect();
        assert_eq!(
            got,
            vec![
                (Loc::rom(0x0000, 0), 3),
                (Loc::rom(0x8000, 2), 3),
                (
                    Loc {
                        addr: 0xC100,
                        src: CodeSrc::Ram {
                            bytes: vec![0x21, 0x03, 0xC0]
                        }
                    },
                    3
                ),
                (
                    Loc {
                        addr: 0xC103,
                        src: CodeSrc::Ram { bytes: vec![0x34] }
                    },
                    1
                ),
            ]
        );
        assert!(Code::from_exec_tsv("h\nz80\t0\trom\n", 0).is_err());
    }
}
