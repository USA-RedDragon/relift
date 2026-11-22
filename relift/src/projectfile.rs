//! The project file, `relift.toml`: one per project folder, holding both
//! what relift reads as its own settings (top-level keys and the
//! `toolchain`, `emit` and `package` tables, see [`crate::settings`]) and the
//! project's own tables ([`PROJECT_TABLES`]): `[game]` with the guest, the
//! import and the name, `[hints]` that relift writes, and the tables a guest
//! or a game declares for itself. The settings loader ignores the project's
//! tables; the project readers ignore the settings.

use std::path::Path;

use serde::de::DeserializeOwned;

/// The file name of the project file.
pub const FILE: &str = "relift.toml";

/// The name an earlier relift gave the project data.
const OLD_FILE: &str = "game.toml";

/// The top-level tables that belong to the project, not to relift's
/// settings.
pub const PROJECT_TABLES: [&str; 7] = ["game", "hints", "settings", "input", "mods", "n64", "nds"];

/// `text`, the content of a project file, decoded as `T`, the settings, with
/// the project's own tables left out.
///
/// # Errors
///
/// `text` is not TOML, or what is left of it does not fit `T`.
pub fn settings_from_str<T: DeserializeOwned>(text: &str) -> Result<T, toml::de::Error> {
    let mut table: toml::Table = text.parse()?;
    for name in PROJECT_TABLES {
        table.remove(name);
    }
    table.try_into()
}

/// Whether the folder `dir` still holds the project data in the file an
/// earlier relift wrote, as an error naming the new place.
///
/// # Errors
///
/// `dir` holds a `game.toml`.
pub fn refuse_old_file(dir: &Path) -> Result<(), String> {
    if dir.join(OLD_FILE).exists() {
        return Err(format!(
            "{}: the project data now lives in {FILE}; move the tables of {OLD_FILE} (such as [game] and [hints]) into {FILE} and delete {OLD_FILE}",
            dir.join(OLD_FILE).display()
        ));
    }
    Ok(())
}

/// Puts the project tables of `stub` into the project file `path`: the file
/// is made when it is missing, and the tables are added after the settings
/// when it holds no `[game]` yet. A file with a `[game]` is left as it is.
/// Returns whether the file changed.
///
/// # Errors
///
/// The file cannot be read or written.
pub fn write_stub(path: &Path, stub: &str) -> std::io::Result<bool> {
    let have = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(),
        Err(e) => return Err(e),
    };
    let has_game = have
        .parse::<toml::Table>()
        .is_ok_and(|t| t.contains_key("game"));
    if has_game {
        return Ok(false);
    }
    let text = if have.trim().is_empty() {
        stub.to_string()
    } else {
        format!("{}\n\n{stub}", have.trim_end())
    };
    relift_cache::write_if_changed(path, text.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(serde::Deserialize, Debug, PartialEq)]
    #[serde(deny_unknown_fields)]
    struct S {
        jobs: u32,
    }

    fn scratch(tag: &str) -> std::path::PathBuf {
        let d =
            std::env::temp_dir().join(format!("relift-projectfile-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn the_settings_loader_skips_the_project_tables_only() {
        let s: S =
            settings_from_str("jobs = 2\n[game]\nguest = \"gb\"\n[hints]\ncode = []\n").unwrap();
        assert_eq!(s, S { jobs: 2 });
        assert!(settings_from_str::<S>("jobs = 2\njobz = 3\n").is_err());
    }

    #[test]
    fn a_stub_joins_the_settings_that_are_already_there() {
        let d = scratch("stub");
        let p = d.join(FILE);
        assert!(write_stub(&p, "[game]\nguest = \"gb\"\n").unwrap());
        assert!(!write_stub(&p, "[game]\nguest = \"gba\"\n").unwrap());
        assert!(std::fs::read_to_string(&p).unwrap().contains("gb\""));
        let q = d.join("other.toml");
        relift_cache::write_if_changed(&q, b"jobs = 2\n").unwrap();
        assert!(write_stub(&q, "[game]\nguest = \"gb\"\n").unwrap());
        let t: toml::Table = std::fs::read_to_string(&q).unwrap().parse().unwrap();
        assert_eq!(t["jobs"].as_integer(), Some(2));
        assert_eq!(t["game"]["guest"].as_str(), Some("gb"));
    }

    #[test]
    fn the_old_file_is_refused_with_the_new_name() {
        let d = scratch("old");
        assert!(refuse_old_file(&d).is_ok());
        relift_cache::write_if_changed(&d.join("game.toml"), b"[game]\n").unwrap();
        let e = refuse_old_file(&d).unwrap_err();
        assert!(e.contains("relift.toml") && e.contains("game.toml"), "{e}");
    }
}
