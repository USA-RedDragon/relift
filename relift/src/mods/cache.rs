//! The analysis of each module before the code mods, kept between builds
//! so a build with other mods lifts again only the functions whose bytes
//! differ ([`relift_analyze::rediscover`]).

use std::path::Path;

use relift_analyze::Analysis;
use relift_ir::RegFile;

use crate::import::BoxError;

/// The key of a cached analysis: this relift, the module's bytes and load
/// address, and the hints that add its roots.
pub fn key(tool: &[u8; 32], image: &[u8], base: u64, hints: &str) -> String {
    let mut all = tool.to_vec();
    all.extend_from_slice(&relift_switch_formats::sha256(image));
    all.extend_from_slice(&base.to_le_bytes());
    all.extend_from_slice(hints.as_bytes());
    relift_switch_formats::sha256(&all)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// The SHA-256 of the running relift, so a cache from another build of
/// relift is never read.
///
/// # Errors
///
/// The executable cannot be read.
pub fn tool_hash() -> Result<[u8; 32], BoxError> {
    let exe = std::env::current_exe()?;
    Ok(relift_switch_formats::sha256(&std::fs::read(exe)?))
}

/// The cached analysis at `path` when its key is `key`.
pub fn read(path: &Path, key: &str, files: &[RegFile]) -> Option<Analysis> {
    let z = std::fs::read(path).ok()?;
    let bytes = zstd::decode_all(z.as_slice()).ok()?;
    let (k, a) = relift_analyze::from_bytes(&bytes, files).ok()?;
    (k == key).then_some(a)
}

/// Writes `a` to `path` under `key`.
///
/// # Errors
///
/// The file cannot be written.
pub fn write(path: &Path, key: &str, a: &Analysis, files: &[RegFile]) -> Result<(), BoxError> {
    let bytes = relift_analyze::to_bytes(a, key, files);
    let z = zstd::encode_all(bytes.as_slice(), 3)?;
    if let Some(d) = path.parent() {
        std::fs::create_dir_all(d)?;
    }
    relift_cache::write_if_changed(path, &z)?;
    Ok(())
}
