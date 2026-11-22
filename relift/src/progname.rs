//! The name of a project's built program.
//!
//! The program, its dev build, the app bundles and the packages are all named
//! after the project: the `name` in the `[game]` table of its `relift.toml`,
//! else the name of the project folder. The name is cut down to a file name
//! that is safe on every host file system ([`slug`]); the human title of a
//! program stays a separate thing, shown where a host shows one.

use std::path::Path;

/// What a name with no usable character becomes.
pub const FALLBACK: &str = "program";

/// A file name for `name`: lowercase ASCII letters and digits, with every
/// other run of characters turned into one dash, none at either end. Never
/// empty: a name with nothing usable gives [`FALLBACK`].
#[must_use]
pub fn slug(name: &str) -> String {
    let mut s = String::new();
    for c in name.chars() {
        if c.is_ascii_alphanumeric() {
            s.push(c.to_ascii_lowercase());
        } else if !s.is_empty() && !s.ends_with('-') {
            s.push('-');
        }
    }
    let s = s.trim_end_matches('-');
    if s.is_empty() {
        FALLBACK.into()
    } else {
        s.into()
    }
}

/// The project in the working directory: its `relift.toml` name, else the name
/// of the folder.
#[must_use]
pub fn project_name() -> String {
    named(
        Path::new("relift.toml"),
        crate::paths::canonical(".")
            .ok()
            .as_deref()
            .and_then(Path::file_name)
            .map(|n| n.to_string_lossy().into_owned()),
    )
}

fn named(manifest: &Path, folder: Option<String>) -> String {
    let from_toml = std::fs::read_to_string(manifest)
        .ok()
        .and_then(|t| t.parse::<toml::Table>().ok())
        .and_then(|t| {
            t.get("game")?
                .get("name")?
                .as_str()
                .map(|s| s.trim().to_string())
        })
        .filter(|s| !s.is_empty());
    from_toml.or(folder).unwrap_or_else(|| "project".into())
}

/// The file name stem of the project in the working directory: its name as
/// a [`slug`].
#[must_use]
pub fn stem() -> String {
    slug(&project_name())
}

/// The file name of the host program of the project in the working
/// directory, with the host's executable suffix.
#[must_use]
pub fn program() -> String {
    format!("{}{}", stem(), std::env::consts::EXE_SUFFIX)
}

/// The file name of the host program of a project called `name`.
#[must_use]
pub fn program_of(name: &str) -> String {
    format!("{}{}", slug(name), std::env::consts::EXE_SUFFIX)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("relift-progname-{tag}-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn a_title_becomes_a_portable_file_name() {
        assert_eq!(slug("Example Project 2"), "example-project-2");
        assert_eq!(slug("  A & B -- C!  "), "a-b-c");
        assert_eq!(slug("../x"), "x");
        assert_eq!(slug("Ünï Çode"), "n-ode");
        assert_eq!(slug("a.b_c"), "a-b-c");
    }

    #[test]
    fn a_name_with_nothing_usable_is_never_empty() {
        assert_eq!(slug(""), FALLBACK);
        assert_eq!(slug("..."), FALLBACK);
        assert_eq!(slug("日本語"), FALLBACK);
    }

    #[test]
    fn the_manifest_name_wins_over_the_folder() {
        let d = scratch("named");
        let m = d.join("relift.toml");
        relift_cache::write_if_changed(&m, "[game]\nname = \"Example Project 2\"\n".as_bytes())
            .unwrap();
        let n = named(&m, Some("folder".into()));
        assert_eq!(n, "Example Project 2");
        assert_eq!(slug(&n), "example-project-2");
    }

    #[test]
    fn a_nameless_project_uses_its_folder() {
        let d = scratch("folder");
        let m = d.join("relift.toml");
        relift_cache::write_if_changed(&m, "[game]\ntitle = \"T\"\n".as_bytes()).unwrap();
        assert_eq!(named(&m, Some("My Folder".into())), "My Folder");
        assert_eq!(named(&d.join("none.toml"), Some("f".into())), "f");
        assert_eq!(named(&m, None), "project");
        assert_eq!(
            program_of("My Folder"),
            format!("my-folder{}", std::env::consts::EXE_SUFFIX)
        );
    }
}
