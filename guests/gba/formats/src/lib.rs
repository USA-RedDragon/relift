//! The Game Boy Advance cartridge format: the header at `0x00-0xBF`, its
//! complement checksum, the logo hash, the size rules, the three ROM
//! mirrors with the open-bus rule past the image, the save library strings
//! a ROM carries and the multiboot entry an image that fits work RAM may
//! carry.
//!
//! Sources: GBATEK's cartridge header and memory map chapters and the
//! save-type notes of the community emulators. A `.gba` file is a plain
//! image, so nothing here needs keys or decryption. The Nintendo logo is
//! never stored: it is checked by the SHA-256 of its 156 bytes. Every test
//! builds its input with [`synth`]; no game data is involved.

pub mod checksum;
pub mod error;
pub mod header;
pub mod rom;
pub mod save;
pub mod synth;

pub use error::{Error, Result};
pub use header::{Checks, Header};
pub use rom::Rom;
pub use save::{SaveKind, SaveString};
