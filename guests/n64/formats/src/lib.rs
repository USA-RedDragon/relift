//! The Nintendo 64 cartridge format: the three byte orders a dump comes in
//! and their normalization to big-endian, the 0x40-byte header, the boot
//! variant picked by a CRC-32 of the boot code area, the standard CRC1/CRC2
//! checksum, the Yay0, Yaz0 and MIO0 decoders and a scan for their streams.
//!
//! Sources: the public n64brew documentation of the cartridge header and
//! of the checksum, and the published CRC-32 values of the known boot code
//! areas. The boot code itself is never read or interpreted: its bytes
//! are only hashed to name the variant. Every test builds its input with
//! [`synth`]; no game data is involved.

pub mod checksum;
pub mod cic;
pub mod compress;
pub mod error;
pub mod header;
pub mod order;
pub mod rom;
pub mod scan;
pub mod synth;

pub use cic::Cic;
pub use error::{Error, Result};
pub use header::Header;
pub use order::ByteOrder;
pub use rom::{CrcCheck, Rom};
pub use scan::{Kind, Stream};
