//! Discovery of NVIDIA shader programs in a Switch 2 game (SM86) or a
//! Switch 1 game (SM53, [`Arch`]): every program in a NUP, an extracted
//! `exefs`/`romfs` tree or an import folder ([`ImportSource`]),
//! deduplicated by content, with a manifest and a summary of the
//! containers seen.
//!
//! A program is the bytes of its 0x80-byte shader program header (graphics
//! only) and its code through the `BRA self` terminator ([`carve`]). Every
//! file is read by its content, not its name: the carve looks at its bytes,
//! and the scan unwraps what holds programs: zstd frames (with the
//! dictionaries the game ships in `ZsDic.pack.zs`), zlib and gzip streams
//! found at any offset ([`sweep`], chunks of one payload decoded
//! together), SARC archives, Yaz0, shader archives (`bfsha`, BNSH, agl
//! `BAHS`), sead `.bin` files, effect files with embedded BNSH and the
//! NSOs of the program. Every payload goes through the same steps again.
//! CUDA cubins (whole or in fat binaries) give one program per kernel,
//! linked with its callees ([`cubin`]), and are not carved.
//!
//! SM53 programs are found by their NVN code header ([`maxwell`]) in every
//! file, through zstd, SARC, Yaz0 ([`yaz0`]) and UnityFS bundles
//! ([`unity`]).
//!
//! Work is cached per input file (see [`scan`]), and runs in parallel on
//! the worker count of `relift-proc`.

pub mod carve;
pub mod cubin;
pub mod error;
pub mod inflate;
pub mod maxwell;
pub mod mesh;
pub mod sarc;
pub mod scan;
pub mod source;
pub mod sweep;
pub mod unity;
pub mod yaz0;
pub mod zst;

pub use error::{Error, Result};
pub use scan::{Arch, Options, Program, Report, scan};
pub use source::{DirSource, ImportSource, NupSource, Source, SourceFile};

pub(crate) fn sha256_hex(b: &[u8]) -> String {
    relift_switch_formats::hex(&relift_switch_formats::sha256(b))
}
