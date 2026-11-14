//! The clippy funnels: `clippy.toml` disallows the subprocess and file-write
//! APIs everywhere, and exactly one function in the owning crate opts out
//! with `#[expect(clippy::disallowed_methods)]`.
//!
//! clippy enforces the disallow; this test holds the other half, that the
//! opt-out is not spread around. The lint name is built from split literals
//! so this file never spells it as code.

#[allow(dead_code, reason = "each test binary uses part of the shared readers")]
mod support;

use std::collections::BTreeMap;
use std::path::PathBuf;

use support::{ROOT, members, read, rs_files_under, tokens};

/// The lint name the walk counts.
const LINT: &str = concat!("disallowed", "_methods");

/// Opt-outs allowed per member: one funnel function each, nowhere else.
const OPT_OUTS: [(&str, usize); 2] = [("relift-cache", 1), ("relift-proc", 1)];

/// What `clippy.toml` must disallow, and the crate that owns each.
const FUNNELED: [(&str, &str); 6] = [
    ("std::process::Command::new", "relift-proc"),
    ("std::fs::write", "relift-cache"),
    ("std::fs::File::create", "relift-cache"),
    ("std::fs::OpenOptions::open", "relift-cache"),
    ("std::fs::copy", "relift-cache"),
    ("std::io::copy", "relift-cache"),
];

/// `clippy.toml` disallows every funneled path, each with a reason that
/// names its owning crate.
#[test]
fn clippy_toml_disallows_every_funneled_path() {
    let toml = read(&PathBuf::from(ROOT).join("clippy.toml"));
    for (path, owner) in FUNNELED {
        let line = toml
            .lines()
            .find(|l| l.contains(&format!("path = \"{path}\"")))
            .unwrap_or_else(|| panic!("clippy.toml no longer disallows {path}"));
        let owner_ident = owner.replace('-', "_");
        assert!(
            line.contains("reason = ") && line.contains(&owner_ident),
            "clippy.toml disallows {path} without a reason naming {owner_ident}: {line}"
        );
    }
}

/// Only the owning crates opt out, once each, and no crate carries its own
/// `clippy.toml` that would shadow the root one.
#[test]
fn only_the_funnel_functions_opt_out() {
    let mut counts: BTreeMap<String, usize> = BTreeMap::new();
    let all = members();
    for member in &all {
        assert!(
            !member.dir.join("clippy.toml").exists() && !member.dir.join(".clippy.toml").exists(),
            "{} has its own clippy.toml, which would replace the root funnels for that crate",
            member.name
        );
        let n: usize = rs_files_under(&member.dir)
            .iter()
            .map(|p| tokens(&read(p), LINT))
            .sum();
        if n > 0 {
            counts.insert(member.name.clone(), n);
        }
    }
    let want: BTreeMap<String, usize> = OPT_OUTS
        .iter()
        .map(|(n, c)| ((*n).to_string(), *c))
        .collect();
    assert_eq!(
        counts, want,
        "opt-outs of the clippy funnels differ from the one funnel function per owner"
    );
    assert!(
        tokens(&format!("#[expect(clippy::{LINT})]"), LINT) == 1,
        "control: the counter does not see the attribute"
    );
}
