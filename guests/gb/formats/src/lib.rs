//! The Game Boy and Game Boy Color cartridge format: the header at
//! `$0100-$014F`, the mapper a cartridge-type byte names, the ROM and RAM
//! size codes, both checksums and the bank layout.
//!
//! Sources: the Pan Docs cartridge header and MBC chapters, and Gekkio's
//! Game Boy: Complete Technical Reference. Mappers are named here and
//! modelled by the runtime; this crate only reads bytes. A cartridge image
//! is a plain ROM dump, so nothing here needs keys or decryption. Every
//! test builds its input with [`synth`]; no game data is involved.

pub mod banks;
pub mod cartridge;
pub mod checksum;
pub mod error;
pub mod header;
pub mod mapper;
pub mod synth;

pub use cartridge::{Cartridge, Checks};
pub use error::{Error, Result};
pub use header::{CgbFlag, Header};
pub use mapper::{Features, Mapper, MapperKind};
