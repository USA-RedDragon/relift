//! The outlines of each module, kept between builds: a build with other
//! mods outlines again only the functions whose bytes differ
//! ([`relift_analyze::rediscover`]), and an unchanged module is not
//! outlined again at all.

use std::path::Path;

use relift_analyze::Layout;
use relift_guest::Module;

use crate::import::BoxError;

/// The key of a cached layout: this relift and everything of the module
/// that discovery reads.
pub fn key(tool: &[u8; 32], m: &Module) -> String {
    let mut all = tool.to_vec();
    all.extend_from_slice(m.name.as_bytes());
    all.push(0);
    all.extend_from_slice(&relift_switch_formats::sha256(&m.image));
    all.extend_from_slice(&m.base.to_le_bytes());
    all.extend_from_slice(format!("{:?}", (&m.sections, &m.roots, &m.pads)).as_bytes());
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

/// The cached layout at `path` when its key is `key`.
pub fn read(path: &Path, key: &str) -> Option<Layout> {
    let z = std::fs::read(path).ok()?;
    let bytes = zstd::decode_all(z.as_slice()).ok()?;
    let (k, l) = relift_analyze::from_bytes(&bytes).ok()?;
    (k == key).then_some(l)
}

/// Writes `l` to `path` under `key`.
///
/// # Errors
///
/// The file cannot be written.
pub fn write(path: &Path, key: &str, l: &Layout) -> Result<(), BoxError> {
    let bytes = relift_analyze::to_bytes(l, key);
    let z = zstd::encode_all(bytes.as_slice(), 3)?;
    if let Some(d) = path.parent() {
        std::fs::create_dir_all(d)?;
    }
    relift_cache::write_if_changed(path, &z)?;
    Ok(())
}
