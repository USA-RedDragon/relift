//! Readers the structural tests share: the members from one
//! `cargo metadata --no-deps`, the `.rs` files under a crate, and a
//! code-only view of a source file that drops comments and literals.

use std::fs;
use std::path::{Path, PathBuf};

/// The workspace root: the test's manifest dir is `relift/`.
pub const ROOT: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/..");

/// One workspace member as `cargo metadata --no-deps` lists it.
pub struct Member {
    pub name: String,
    /// The crate directory, absolute.
    pub dir: PathBuf,
    /// The crate directory relative to the root, `/`-separated.
    pub rel: String,
}

/// Every workspace member.
pub fn members() -> Vec<Member> {
    let root = fs::canonicalize(ROOT).expect("the workspace root resolves");
    let out = relift_proc::command(env!("CARGO"))
        .args(["metadata", "--no-deps", "--format-version", "1"])
        .current_dir(&root)
        .output()
        .expect("cargo metadata runs");
    assert!(
        out.status.success(),
        "cargo metadata failed:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let meta: serde_json::Value =
        serde_json::from_slice(&out.stdout).expect("cargo metadata emits JSON");
    let members: Vec<Member> = meta["packages"]
        .as_array()
        .expect("metadata carries a packages array")
        .iter()
        .map(|p| {
            let manifest = PathBuf::from(p["manifest_path"].as_str().expect("a manifest path"));
            let dir = manifest
                .parent()
                .expect("a manifest sits in a directory")
                .to_path_buf();
            let rel = dir
                .strip_prefix(&root)
                .expect("a member sits under the root")
                .to_string_lossy()
                .replace('\\', "/");
            Member {
                name: p["name"].as_str().expect("a package name").to_owned(),
                dir,
                rel,
            }
        })
        .collect();
    assert!(
        members.len() >= 16,
        "control: cargo metadata listed {} members, too few to be this workspace",
        members.len()
    );
    members
}

pub fn read(path: &Path) -> String {
    fs::read_to_string(path)
        .unwrap_or_else(|e| panic!("presence control: {} is unreadable ({e})", path.display()))
}

/// Every `.rs` file under `dir`, recursively, skipping `target` dirs. The
/// whole crate directory, since a manifest lint reaches build scripts,
/// benches and examples too.
pub fn rs_files_under(dir: &Path) -> Vec<PathBuf> {
    fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
        let entries = fs::read_dir(dir)
            .unwrap_or_else(|e| panic!("presence control: {} is unreadable ({e})", dir.display()));
        for entry in entries {
            let entry = entry.expect("readable directory entry");
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if path.is_dir() {
                if name != "target" {
                    walk(&path, out);
                }
            } else if name.ends_with(".rs") {
                out.push(path);
            }
        }
    }
    let mut files = Vec::new();
    walk(dir, &mut files);
    files.sort();
    files
}

/// The key lines of one manifest section, comments and blanks dropped.
/// Text, not a TOML parser: the tables the tests read are one line each, and a reader that cannot parse a line it does not expect fails
/// closed by not finding the line it does.
pub fn section<'a>(manifest: &'a str, header: &str) -> Option<Vec<&'a str>> {
    let mut inside = false;
    let mut found = false;
    let mut lines = Vec::new();
    for line in manifest.lines() {
        let t = line.trim();
        if t.starts_with('[') {
            inside = t == header;
            found |= inside;
            continue;
        }
        if inside && !t.is_empty() && !t.starts_with('#') {
            lines.push(t);
        }
    }
    found.then_some(lines)
}

pub fn has_key(lines: &[&str], key: &str, value: &str) -> bool {
    lines.iter().any(|l| {
        l.strip_prefix(key)
            .map(|rest| rest.trim_start())
            .and_then(|rest| rest.strip_prefix('='))
            .is_some_and(|rest| rest.trim() == value)
    })
}

/// `Some(n)` when `bytes[at..]` opens a raw string with `n` hashes.
pub fn raw_string_hashes(bytes: &[u8], at: usize) -> Option<usize> {
    let hashes = bytes[at.min(bytes.len())..]
        .iter()
        .take_while(|b| **b == b'#')
        .count();
    (bytes.get(at + hashes) == Some(&b'"')).then_some(hashes)
}

/// `source` with every comment (line, block, doc) and every string, raw
/// string, char and byte literal replaced by a space, so that what remains
/// is code and only code. Lifetimes and labels survive: a `'` that neither
/// closes within one char nor opens an escape is not a literal.
pub fn code_only(source: &str) -> String {
    let b = source.as_bytes();
    let mut out = String::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        let ident_before = i > 0 && (b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'_');
        let raw_at = match c {
            b'r' if !ident_before => Some(i + 1),
            b'b' | b'c' if !ident_before && b.get(i + 1) == Some(&b'r') => Some(i + 2),
            _ => None,
        };
        if c == b'/' && b.get(i + 1) == Some(&b'/') {
            while i < b.len() && b[i] != b'\n' {
                i += 1;
            }
        } else if c == b'/' && b.get(i + 1) == Some(&b'*') {
            let mut depth = 0usize;
            while i + 1 < b.len() {
                if b[i] == b'/' && b[i + 1] == b'*' {
                    depth += 1;
                    i += 2;
                } else if b[i] == b'*' && b[i + 1] == b'/' {
                    depth -= 1;
                    i += 2;
                    if depth == 0 {
                        break;
                    }
                } else {
                    i += 1;
                }
            }
            out.push(' ');
        } else if let Some((at, hashes)) =
            raw_at.and_then(|at| raw_string_hashes(b, at).map(|hashes| (at, hashes)))
        {
            let close: Vec<u8> = std::iter::once(b'"')
                .chain(std::iter::repeat_n(b'#', hashes))
                .collect();
            i = at + hashes + 1;
            while i < b.len() && !b[i..].starts_with(&close) {
                i += 1;
            }
            i += close.len();
            out.push(' ');
        } else if c == b'"' {
            i += 1;
            while i < b.len() && b[i] != b'"' {
                if b[i] == b'\\' {
                    i += 1;
                }
                i += 1;
            }
            i += 1;
            out.push(' ');
        } else if c == b'\'' {
            let close = match b.get(i + 1) {
                Some(b'\\') => b[(i + 3).min(b.len())..]
                    .iter()
                    .position(|&x| x == b'\'')
                    .map(|p| i + 3 + p),
                Some(&next) if next < 0x80 => (b.get(i + 2) == Some(&b'\'')).then_some(i + 2),
                Some(_) => b[(i + 2).min(b.len())..(i + 6).min(b.len())]
                    .iter()
                    .position(|&x| x == b'\'')
                    .map(|p| i + 2 + p),
                None => None,
            };
            match close {
                Some(close) => {
                    i = close + 1;
                    out.push(' ');
                }
                None => {
                    out.push('\'');
                    i += 1;
                }
            }
        } else {
            out.push(c as char);
            i += 1;
        }
    }
    out
}

/// Occurrences of `word` as a whole token in `source`'s code.
pub fn tokens(source: &str, word: &str) -> usize {
    code_only(source)
        .split(|c: char| !(c.is_alphanumeric() || c == '_' || c == '#'))
        .filter(|w| *w == word)
        .count()
}
