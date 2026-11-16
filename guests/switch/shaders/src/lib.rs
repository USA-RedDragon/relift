//! Discovery of NVIDIA SM86 shader programs in a Switch 2 game: every
//! program in a NUP or an extracted `exefs`/`romfs` tree, deduplicated by
//! content, with a manifest and a summary of the containers seen.
//!
//! A program is the bytes of its 0x80-byte shader program header (graphics
//! only) and its code through the `BRA self` terminator ([`carve`]). The
//! scan unwraps what holds programs: zstd frames (with the dictionaries
//! the game ships in `ZsDic.pack.zs`), SARC archives, shader archives
//! (`bfsha`, BNSH, agl `BAHS`), sead `.bin` files, effect files with
//! embedded BNSH, KTGL `rdb`/`rdx`/`fdata` entries (whole or in zlib
//! chunks) and the NSOs of `exefs`. CUDA cubins (whole or in fat
//! binaries) give one program per kernel, linked with its callees
//! ([`cubin`]), and are not carved.
//!
//! Work is cached per input file (see [`scan`]), and runs in parallel on
//! the worker count of `relift-proc`.

pub mod carve;
pub mod cubin;
pub mod error;
pub mod inflate;
pub mod ktgl;
pub mod sarc;
pub mod scan;
pub mod source;
pub mod zst;

pub use error::{Error, Result};
pub use scan::{Options, Program, Report, scan};
pub use source::{DirSource, NupSource, Source, SourceFile};

pub(crate) fn sha256_hex(b: &[u8]) -> String {
    relift_switch_formats::hex(&relift_switch_formats::sha256(b))
}
