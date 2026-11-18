//! The decrypted-only policy, held by names and by behavior and not by a
//! ban on crates: relift reads and ships decrypted Switch inputs only.
//!
//! No workspace source handles key files, title keys, tickets or the NCA,
//! BKTR and ticket structures. Crypto crates are allowed in the graph where
//! a charter row states why (guest crypto instructions, SDK `nn::crypto`
//! replacements, save data hashing), so this is a scan of what the sources
//! say. The refusal tests are `encrypted_inputs_are_refused` and
//! `an_encrypted_header_is_refused_without_a_key` in
//! `guests/switch/formats/tests/import.rs` and
//! `import_refuses_encrypted_dumps_pointing_to_a_separate_tool` in
//! `relift/tests/cli.rs`.
//!
//! Every forbidden word is built from split literals, so this file never
//! spells one, and a planted fixture is built at run time.

#[allow(dead_code, reason = "each test binary uses part of the shared readers")]
mod support;

use std::path::{Path, PathBuf};

use support::{code_only, members, read, rs_files_under};

/// Strings that no source may hold, matched without regard to case in the
/// whole file, comments included: key files and key material names.
fn forbidden_strings() -> Vec<String> {
    vec![
        ["prod", ".keys"].concat(),
        ["dev", ".keys"].concat(),
        ["title", ".keys"].concat(),
        ["console", ".keys"].concat(),
        ["title", "key"].concat(),
        ["title", "_key"].concat(),
        ["header", "_key"].concat(),
        ["key_area", "_key"].concat(),
        ["title", "kek"].concat(),
        ["e", "ticket"].concat(),
        ["master", "_key"].concat(),
    ]
}

/// Identifier parts that no code may hold: the ticket and BKTR structures.
/// Matched inside a lower-cased identifier.
fn forbidden_parts() -> Vec<String> {
    vec![
        ["tick", "et"].concat(),
        ["bk", "tr"].concat(),
        ["nca", "header"].concat(),
        ["nca", "_header"].concat(),
        ["rights", "id"].concat(),
        ["rights", "_id"].concat(),
    ]
}

/// Whole identifiers that no code may hold: the NCA module and type names.
fn forbidden_identifiers() -> Vec<String> {
    vec![["n", "ca"].concat(), ["N", "ca"].concat()]
}

/// Entries of the scan that are allowed to hold a forbidden word, as
/// `(path from the workspace root, word)`; each needs a written reason here.
///
/// - `gpus/sm53/src/field.rs`, the word in tickets: the Maxwell special
///   register `SR_ORDERING_TICKET`, a GPU register name in the generated
///   operand table, which has nothing to do with Switch tickets.
const ALLOWED: &[(&str, &str)] = &[("gpus/sm53/src/field.rs", "ticket")];

fn is_allowed(path: &Path, word: &str) -> bool {
    let shown = path.to_string_lossy().replace('\\', "/");
    ALLOWED
        .iter()
        .any(|(suffix, w)| shown.ends_with(suffix) && *w == word)
}

/// Every finding in the `.rs` files under `dir`, as `path: word`.
fn scan(dir: &Path) -> (usize, Vec<String>) {
    let strings = forbidden_strings();
    let parts = forbidden_parts();
    let idents = forbidden_identifiers();
    let files = rs_files_under(dir);
    let mut found = Vec::new();
    for path in &files {
        let source = read(path);
        let lower = source.to_ascii_lowercase();
        for word in &strings {
            if lower.contains(word.as_str()) && !is_allowed(path, word) {
                found.push(format!("{}: {word}", path.display()));
            }
        }
        let code = code_only(&source);
        for ident in code.split(|c: char| !(c.is_alphanumeric() || c == '_')) {
            let low = ident.to_ascii_lowercase();
            for part in &parts {
                if low.contains(part.as_str()) && !is_allowed(path, part) {
                    found.push(format!("{}: {ident}", path.display()));
                }
            }
            if idents.iter().any(|i| i == ident) && !is_allowed(path, ident) {
                found.push(format!("{}: {ident}", path.display()));
            }
        }
    }
    found.sort();
    found.dedup();
    (files.len(), found)
}

fn scratch(name: &str) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("scratch dir");
    d
}

#[test]
fn no_workspace_source_handles_key_material() {
    let mut walked = 0usize;
    let mut found = Vec::new();
    for member in members() {
        let (n, f) = scan(&member.dir);
        walked += n;
        found.extend(f);
    }
    assert!(
        walked >= 15,
        "control: the scan read {walked} files, too few"
    );
    assert!(
        found.is_empty(),
        "relift reads decrypted dumps only; these sources name keys, tickets or NCA structures:\n{}",
        found.join("\n")
    );
}

/// The scan finds a planted word of each kind, in a fixture built at run
/// time, and finds nothing in a clean one, so a zero above is not vacuous.
#[test]
fn the_scan_finds_planted_words() {
    let d = scratch("decrypted-only-planted");
    let key_file = format!("let f = \"{}\";\n", forbidden_strings()[0]);
    let upper = format!("// {}\n", forbidden_strings()[4].to_ascii_uppercase());
    let part = format!("struct My{}Reader;\n", forbidden_parts()[0]);
    let ident = format!("mod {};\n", forbidden_identifiers()[0]);
    for (name, text) in [
        ("string.rs", &key_file),
        ("comment.rs", &upper),
        ("part.rs", &part),
        ("ident.rs", &ident),
    ] {
        relift_cache::write_if_changed(&d.join(name), text.as_bytes()).expect("fixture writes");
    }
    let (n, found) = scan(&d);
    assert_eq!(n, 4);
    assert_eq!(found.len(), 4, "{found:?}");
    for name in ["string.rs", "comment.rs", "part.rs", "ident.rs"] {
        assert!(found.iter().any(|f| f.contains(name)), "{name}: {found:?}");
    }

    let clean = scratch("decrypted-only-clean");
    relift_cache::write_if_changed(
        &clean.join("ok.rs"),
        b"fn hash(a: &[u8]) -> u8 { a.len() as u8 }\nconst MAGIC: &[u8] = b\"PFS0\";\n",
    )
    .expect("fixture writes");
    let (n, found) = scan(&clean);
    assert_eq!((n, found.len()), (1, 0), "{found:?}");
}

/// Every row of the allow list names a file that exists.
#[test]
fn the_allow_list_names_real_files() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    for (suffix, _) in ALLOWED {
        assert!(
            root.join(suffix).exists()
                || members()
                    .iter()
                    .any(|m| m.dir.join(suffix).exists() || Path::new(suffix).exists()),
            "ALLOWED names {suffix}, which does not exist; strike the row"
        );
    }
}
