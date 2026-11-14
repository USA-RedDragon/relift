//! Path helpers.

use std::io;
use std::path::{Path, PathBuf};

/// The canonical form of `path` that the C compiler can search from. On
/// Windows `std::fs::canonicalize` returns a `\\?\` verbatim path, and
/// clang looks up a quoted `#include` beside the including file by
/// textual path, which fails for that form; a verbatim drive path is
/// turned back into the plain one. Elsewhere this is `canonicalize`.
///
/// # Errors
///
/// As [`std::fs::canonicalize`].
pub fn canonical(path: impl AsRef<Path>) -> io::Result<PathBuf> {
    let p = std::fs::canonicalize(path)?;
    Ok(plain(p))
}

fn plain(p: PathBuf) -> PathBuf {
    let s = p.to_string_lossy();
    match s.strip_prefix(r"\\?\") {
        Some(rest) if is_drive_path(rest) => PathBuf::from(rest),
        _ => p,
    }
}

fn is_drive_path(s: &str) -> bool {
    let b = s.as_bytes();
    b.len() >= 3 && b[0].is_ascii_alphabetic() && b[1] == b':' && (b[2] == b'\\' || b[2] == b'/')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_verbatim_drive_path_becomes_plain() {
        assert_eq!(
            plain(PathBuf::from(r"\\?\C:\a\b")),
            PathBuf::from(r"C:\a\b")
        );
    }

    #[test]
    fn other_paths_are_kept() {
        for p in [r"\\?\UNC\host\share", r"C:\a", "/usr/lib", r"\\?\pipe"] {
            assert_eq!(plain(PathBuf::from(p)), PathBuf::from(p));
        }
    }
}
