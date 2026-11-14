//! Readers for decrypted Switch files: romfs, NSO, NRO, NPDM and NUP, and
//! the import of a decrypted dump (a `.nup`, or an extracted
//! `exefs`/`romfs` tree) into a project.
//!
//! relift reads decrypted dumps only; this crate holds no keys and no
//! cryptography beyond the SHA-256 the formats carry. Readers sit on
//! [`storage::Storage`] values, so a 4 GiB romfs streams without being held
//! in memory. Every format module also has a small builder used by the tests
//! to make synthetic fixtures; no game data is needed to test any of it.

mod bytes;

pub mod error;
pub mod import;
pub mod npdm;
pub mod nro;
pub mod nso;
pub mod nup;
pub mod romfs;
pub mod storage;
pub mod synth;

pub use bytes::{hex, sha256};
pub use error::{Error, Result};
