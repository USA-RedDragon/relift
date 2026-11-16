//! The Nintendo DS cartridge format: the header at `0x000-0x1FF` with its
//! CRCs and logo hash, the SDK's module parameters, BLZ compression, the
//! static binaries with their autoload blocks, the overlay tables, the
//! file allocation and name tables.
//!
//! Sources: GBATEK's DS cartridge header chapter and the SDK's own
//! conventions. Only decrypted images are read. The Nintendo logo is never
//! stored: it is checked by the SHA-256 of its 156 bytes. Every test builds
//! its input with [`synth`]; no game data is involved.

pub mod blz;
mod bytes;
pub mod checksum;
pub mod error;
pub mod fs;
pub mod header;
pub mod module;
pub mod overlay;
pub mod params;
pub mod rom;
pub mod synth;

pub use error::{Error, Result};
pub use header::{Checks, Header};
pub use module::{Autoload, Cpu, StaticUnit};
pub use overlay::Overlay;
pub use params::ModuleParams;
pub use rom::Rom;
