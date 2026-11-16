//! The Sega Master System, Game Gear and SG-1000 cartridge format: image
//! size and bank layout, the `TMR SEGA` header, the export BIOS checksum,
//! the Codemasters header and checksum, system detection and a static
//! first guess at the mapper.
//!
//! Sources: the SMS Power documentation of the ROM header, the Codemasters
//! header and the mappers. Mappers are named here and modelled by the
//! runtime; this crate only reads bytes. A cartridge image is a plain ROM
//! dump, so nothing here needs keys or decryption. Every test builds its
//! input with [`synth`]; no game data is involved.

pub mod banks;
pub mod cartridge;
pub mod codemasters;
pub mod error;
pub mod header;
pub mod mapper;
pub mod synth;
pub mod system;

pub use banks::Layout;
pub use cartridge::Cartridge;
pub use codemasters::CodemastersHeader;
pub use error::{Error, Result};
pub use header::Header;
pub use mapper::{MapperDecision, MapperKind};
pub use system::{System, SystemSource};
