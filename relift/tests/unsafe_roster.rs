//! The `unsafe` roster: which crates may spell it, a ceiling on each, and the
//! workspace lint that makes every other crate's answer a build error.
//!
//! `[workspace.lints.rust] unsafe_code = "forbid"` reaches every target of
//! every member that inherits it, and a Cargo-level `forbid` cannot be
//! `allow`ed back in source. So a crate either inherits the workspace table
//! and spells no `unsafe` anywhere, or it is on [`ROSTER`], carries its own
//! `deny` table so a scoped `allow` is possible, and sits under a ceiling in
//! [`CEILINGS`].
//!
//! A ceiling equals the count it measures: the change that sheds an
//! occurrence lowers the constant with it, and a ceiling is never raised
//! without a written plan amendment. The counted keyword is built from split
//! literals so this file never spells it as code.

#[allow(dead_code, reason = "each test binary uses part of the shared readers")]
mod support;

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use support::{Member, ROOT, has_key, members, read, rs_files_under, section, tokens};

/// The keyword the walk counts, split so this file never holds it.
const KW: &str = concat!("uns", "afe");

/// The crates that may spell the keyword. Each carries `[lints.rust]` with
/// `unsafe_code = "deny"` and `unsafe_op_in_unsafe_fn = "deny"` instead of
/// `workspace = true`:
///
/// * `relift-override`: overrides export C symbols with
///   `#[unsafe(export_name)]`, which `forbid(unsafe_code)` rejects. It holds
///   its seat from the start, by design, while it is still empty.
const ROSTER: [&str; 1] = ["relift-override"];

/// Keyword tokens per rostered crate, over every `.rs` file under the crate
/// directory. Measured at the commit that added this file: the crate is
/// empty, so the ceiling is 0 until the first export lands with its count.
const CEILINGS: [(&str, usize); 1] = [("relift-override", 0)];

fn is_rostered(member: &Member) -> bool {
    ROSTER.contains(&member.name.as_str())
}

fn inherits_workspace_lints(manifest: &str) -> bool {
    section(manifest, "[lints]").is_some_and(|lines| has_key(&lines, "workspace", "true"))
}

fn has_own_deny_table(manifest: &str) -> bool {
    section(manifest, "[lints.rust]").is_some_and(|lines| {
        has_key(&lines, "unsafe_code", "\"deny\"")
            && has_key(&lines, "unsafe_op_in_unsafe_fn", "\"deny\"")
    })
}

fn tokens_by_file(dir: &Path) -> Vec<(PathBuf, usize)> {
    rs_files_under(dir)
        .into_iter()
        .map(|p| {
            let n = tokens(&read(&p), KW);
            (p, n)
        })
        .filter(|(_, n)| *n > 0)
        .collect()
}

/// The walker on planted text: every zero below is vacuous unless the
/// counter fires on code and stays quiet on prose.
#[test]
fn the_walker_counts_code_and_not_prose() {
    let cases: [(String, usize); 12] = [
        (format!("fn f() {{ {KW} {{ }} }}"), 1),
        (format!("// {KW} {{\nfn f() {{}}"), 0),
        (format!("/// {KW} fn\n//! {KW} impl\nfn f() {{}}"), 0),
        (format!("/* {KW} */ {KW} fn f() {{}}"), 1),
        (format!("/* a /* {KW} */ {KW} */ fn f() {{}}"), 0),
        (format!("let s = \"{KW} {{\"; let t = \"\\\"{KW}\";"), 0),
        (format!("let s = r#\"{KW}\"#; let t = br\"{KW}\";"), 0),
        (
            format!("let c = '\"'; {KW} {{ }} let d = '\\''; {KW} {{ }}"),
            2,
        ),
        (format!("fn f<'a>(x: &'a str) {{ {KW} {{ }} }}"), 1),
        (
            format!("#[{KW}(no_mangle)] pub extern \"C\" fn f() {{}}"),
            1,
        ),
        (format!("#![deny({KW}_code)] let r#{KW} = 1;"), 0),
        (
            format!("{KW} extern \"C\" {{ fn g(); }} type F = {KW} extern \"C\" fn();"),
            2,
        ),
    ];
    for (i, (text, want)) in cases.iter().enumerate() {
        assert_eq!(tokens(text, KW), *want, "planted case {i}: {text:?}");
    }
}

/// The root manifest's workspace lint table forbids the keyword: the line
/// every inheriting member's zero rests on.
#[test]
fn the_workspace_lint_table_forbids_unsafe_code() {
    let manifest = read(&PathBuf::from(ROOT).join("Cargo.toml"));
    let lines = section(&manifest, "[workspace.lints.rust]")
        .unwrap_or_else(|| panic!("the root Cargo.toml has no [workspace.lints.rust] table"));
    assert!(
        has_key(&lines, "unsafe_code", "\"forbid\""),
        "the root [workspace.lints.rust] no longer reads unsafe_code = \"forbid\": {lines:?}"
    );
}

/// Every member inherits the workspace table, or is rostered and carries
/// its own `deny` pair; never neither, never both. A roster entry that
/// names no member is stale and fails too.
#[test]
fn every_member_inherits_the_forbid_or_is_rostered() {
    let all = members();
    let names: BTreeSet<&str> = all.iter().map(|m| m.name.as_str()).collect();
    for rostered in ROSTER {
        assert!(
            names.contains(rostered),
            "ROSTER names {rostered}, which is not a workspace member; strike the row"
        );
    }
    for member in &all {
        let manifest = read(&member.dir.join("Cargo.toml"));
        let inherits = inherits_workspace_lints(&manifest);
        let own = has_own_deny_table(&manifest);
        match (is_rostered(member), inherits, own) {
            (false, true, false) | (true, false, true) => {}
            (false, false, _) => panic!(
                "{} ({}) neither inherits the workspace lints nor is on ROSTER: add \
                 `[lints]` / `workspace = true`, or roster it with a deny pair of its own",
                member.name, member.rel
            ),
            (false, true, true) => panic!(
                "{} ({}) inherits the workspace lints and carries its own [lints.rust]; \
                 a crate is in one state or the other",
                member.name, member.rel
            ),
            (true, true, _) => panic!(
                "{} ({}) is on ROSTER and inherits the workspace lints; a Cargo-level forbid \
                 cannot be allowed back, so a rostered crate carries its own deny pair",
                member.name, member.rel
            ),
            (true, false, false) => panic!(
                "{} ({}) is on ROSTER without unsafe_code = \"deny\" and \
                 unsafe_op_in_unsafe_fn = \"deny\" under [lints.rust]",
                member.name, member.rel
            ),
        }
    }
}

/// Zero keyword tokens in any file of any member off the roster. The build
/// already refuses them through the forbid; this holds the same zero as a
/// test, so a crate that swapped `workspace = true` for its own table still
/// reads red.
#[test]
fn no_unrostered_member_spells_unsafe() {
    let all = members();
    let mut walked = 0usize;
    let mut offenders = Vec::new();
    for member in all.iter().filter(|m| !is_rostered(m)) {
        let files = rs_files_under(&member.dir);
        assert!(
            !files.is_empty(),
            "presence control: {} holds no .rs files",
            member.name
        );
        walked += files.len();
        for (path, n) in tokens_by_file(&member.dir) {
            offenders.push(format!("{} ({n})", path.display()));
        }
    }
    assert!(
        walked >= 15,
        "control: the walk read {walked} files across the unrostered crates, too few"
    );
    assert!(
        offenders.is_empty(),
        "the keyword appears as code outside the roster: {offenders:?}"
    );
}

/// Each rostered crate reads exactly its ceiling: over it is a regression,
/// under it means the change that shed an occurrence must lower the
/// constant. The two tables name the same crates.
#[test]
fn each_rostered_crate_holds_its_unsafe_ceiling() {
    let roster: BTreeSet<&str> = ROSTER.into_iter().collect();
    let ceilinged: BTreeSet<&str> = CEILINGS.iter().map(|(n, _)| *n).collect();
    assert_eq!(
        roster, ceilinged,
        "ROSTER and CEILINGS name different crates"
    );
    let all = members();
    let mut broken = Vec::new();
    for (name, ceiling) in CEILINGS {
        let member = all
            .iter()
            .find(|m| m.name == name)
            .unwrap_or_else(|| panic!("CEILINGS names {name}, which is not a member"));
        let by_file = tokens_by_file(&member.dir);
        let count: usize = by_file.iter().map(|(_, n)| n).sum();
        if count > ceiling {
            broken.push(format!(
                "{name} reads {count} against a ceiling of {ceiling}: {by_file:?}. A ceiling \
                 only rises with a written plan amendment"
            ));
        } else if count < ceiling {
            broken.push(format!(
                "{name} reads {count}, under its ceiling of {ceiling}; lower the constant"
            ));
        }
    }
    assert!(broken.is_empty(), "{}", broken.join("\n"));
}
