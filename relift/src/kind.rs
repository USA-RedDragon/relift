//! Whether a program is a game or an application: `game` in the `[game]`
//! table of `relift.toml`, `true` or `false`. Without it the guest decides:
//! every console and handheld guest is a game, the Linux guest is not.
//!
//! A PC binary does not say what it is, so the PC guests take the default
//! from the program's own packaging metadata where there is some, and from
//! nothing (not a game) where there is none. `relift.toml` always overrides.
//! The sources, in the order each guest gains them:
//!
//! - Linux: a desktop entry (`.desktop` file) for the program whose
//!   `Categories` has `Game`, found beside the program, in the
//!   `share/applications` of its prefix, or in the sysroot's
//!   `usr/share/applications` ([`desktop_entry`]). The import writes what it
//!   found into `relift.toml` as `game` and says which file decided.
//! - Windows (when that guest exists): the Game Definition File resource
//!   (`GDF`) embedded in the executable.
//! - A Steam install (when an import reads one): the app type of its
//!   `appmanifest`.
//!
//! The answer goes into the program's self-description
//! (`relift_info::Identity::game`). The hosts read it from there: the Android
//! manifest, the macOS and iOS property lists and the desktop entry name a
//! game category when it is true, and the runtime offers the pause sheet and
//! its hotkeys only then (`rl_app_is_game`).

use std::path::{Path, PathBuf};

use relift_info::Info;

use crate::import::BoxError;

/// The guest a program is built from, by its `identity.platform`, as a game
/// or not when `relift.toml` does not say.
#[must_use]
pub fn default_for(platform: &str) -> bool {
    platform != "linux"
}

/// The `game` key of the `[game]` table of the project file `path`.
///
/// # Errors
///
/// The file does not parse as TOML, or the key is not `true` or `false`.
pub fn read(path: &Path) -> Result<Option<bool>, BoxError> {
    let Ok(text) = std::fs::read_to_string(path) else {
        return Ok(None);
    };
    parse(&text).map_err(|e| format!("{}: {e}", path.display()).into())
}

fn parse(text: &str) -> Result<Option<bool>, String> {
    let t: toml::Table = text.parse().map_err(|e| format!("{e}"))?;
    match t
        .get("game")
        .and_then(|g| g.as_table())
        .and_then(|g| g.get("game"))
    {
        None => Ok(None),
        Some(toml::Value::Boolean(b)) => Ok(Some(*b)),
        Some(_) => Err("game.game is neither true nor false".into()),
    }
}

/// Whether the text of a desktop entry puts its application in the `Game`
/// main category (`Categories` of the `[Desktop Entry]` group, a `;` list;
/// <https://specifications.freedesktop.org/desktop-entry-spec/latest/>).
#[must_use]
pub fn entry_is_game(text: &str) -> bool {
    let mut in_entry = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_entry = line == "[Desktop Entry]";
        } else if in_entry && let Some(v) = line.strip_prefix("Categories=") {
            return v.split(';').any(|c| c.trim() == "Game");
        }
    }
    false
}

/// Whether the text of a desktop entry starts `program`: its `Exec` command
/// is that file name, or the entry's own file stem is.
fn entry_names(text: &str, stem: &str, program: &str) -> bool {
    if stem == program {
        return true;
    }
    let mut in_entry = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_entry = line == "[Desktop Entry]";
        } else if in_entry && let Some(v) = line.strip_prefix("Exec=") {
            let first = v.split_whitespace().next().unwrap_or("").trim_matches('"');
            return first.rsplit('/').next() == Some(program);
        }
    }
    false
}

/// The desktop entry of the program at `program` and whether it names the
/// game category: looked for beside the program, in `../share/applications`
/// of its folder, and in `usr/share/applications` and
/// `usr/local/share/applications` of `sysroot`. The first entry that names
/// the program wins; `None` when there is none.
#[must_use]
pub fn desktop_entry(program: &Path, sysroot: Option<&Path>) -> Option<(PathBuf, bool)> {
    let name = program.file_name()?.to_str()?;
    let dir = program.parent().unwrap_or(Path::new("."));
    let mut dirs = vec![dir.to_path_buf(), dir.join("../share/applications")];
    if let Some(root) = sysroot {
        dirs.push(root.join("usr/share/applications"));
        dirs.push(root.join("usr/local/share/applications"));
    }
    for d in dirs {
        let Ok(read) = std::fs::read_dir(&d) else {
            continue;
        };
        let mut files: Vec<PathBuf> = read
            .filter_map(Result::ok)
            .map(|e| e.path())
            .filter(|p| p.extension().is_some_and(|x| x == "desktop"))
            .collect();
        files.sort();
        for f in files {
            let Ok(text) = std::fs::read_to_string(&f) else {
                continue;
            };
            let stem = f.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            if entry_names(&text, stem, name) {
                return Some((f, entry_is_game(&text)));
            }
        }
    }
    None
}

/// The answer for `platform` given the project file's key.
#[must_use]
pub fn resolve(platform: &str, key: Option<bool>) -> bool {
    key.unwrap_or_else(|| default_for(platform))
}

/// Writes the answer into `info`, reading the project file `relift.toml` of
/// the current folder.
///
/// # Errors
///
/// As [`read`].
pub fn stamp(info: &mut Info) -> Result<(), BoxError> {
    stamp_from(info, Path::new("relift.toml"))
}

/// As [`stamp`], reading `file`.
///
/// # Errors
///
/// As [`read`].
pub fn stamp_from(info: &mut Info, file: &Path) -> Result<(), BoxError> {
    let key = read(file)?;
    info.identity.game = Some(resolve(&info.identity.platform, key));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_guest_but_linux_is_a_game_by_default() {
        for p in [
            "switch", "switch2", "gb", "gbc", "gba", "nds", "n64", "sms", "gg", "sg1000",
        ] {
            assert!(default_for(p), "{p}");
            assert!(resolve(p, None), "{p}");
        }
        assert!(!default_for("linux"));
        assert!(!resolve("linux", None));
    }

    #[test]
    fn the_key_overrides_the_default_both_ways() {
        let on = parse("[game]\nguest = \"linux\"\ngame = true\n").unwrap();
        let off = parse("[game]\nguest = \"gb\"\ngame = false\n").unwrap();
        assert!(resolve("linux", on));
        assert!(!resolve("gb", off));
    }

    #[test]
    fn a_file_without_the_key_or_the_table_says_nothing() {
        assert_eq!(parse("[game]\nguest = \"gb\"\n").unwrap(), None);
        assert_eq!(parse("jobs = 2\n").unwrap(), None);
    }

    #[test]
    fn a_key_that_is_not_a_boolean_is_refused() {
        assert!(parse("[game]\ngame = \"yes\"\n").is_err());
        assert!(parse("[game]\ngame = 1\n").is_err());
    }

    #[test]
    fn stamping_reads_the_file_and_falls_back_to_the_platform() {
        let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/tmp/kind-stamp");
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("relift.toml");
        let mut i = Info::new("0");
        i.identity.platform = "linux".into();
        stamp_from(&mut i, &file).unwrap();
        assert_eq!(i.identity.game, Some(false));
        relift_cache::write_if_changed(&file, b"[game]\ngame = true\n").unwrap();
        stamp_from(&mut i, &file).unwrap();
        assert_eq!(i.identity.game, Some(true));
        i.identity.platform = "gba".into();
        relift_cache::write_if_changed(&file, b"[game]\ngame = false\n").unwrap();
        stamp_from(&mut i, &file).unwrap();
        assert_eq!(i.identity.game, Some(false));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_game_category_decides_a_desktop_entry() {
        let game = "[Desktop Entry]\nName=X\nCategories=Game;ActionGame;\n";
        let app = "[Desktop Entry]\nName=X\nCategories=Utility;\n";
        let other = "[Desktop Entry]\nCategories=Utility;\n[Desktop Action a]\nCategories=Game;\n";
        assert!(entry_is_game(game));
        assert!(!entry_is_game(app));
        assert!(!entry_is_game(other));
        assert!(!entry_is_game("[Desktop Entry]\nName=X\n"));
        assert!(!entry_is_game("[Desktop Entry]\nCategories=GameOfLife;\n"));
    }

    #[test]
    fn the_entry_of_a_program_is_found_by_exec_or_name() {
        let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/tmp/kind-entry");
        let apps = dir.join("sysroot/usr/share/applications");
        std::fs::create_dir_all(&apps).unwrap();
        std::fs::create_dir_all(dir.join("bin")).unwrap();
        relift_cache::write_if_changed(
            &apps.join("org.example.Other.desktop"),
            "[Desktop Entry]\nExec=other\nCategories=Utility;\n".as_bytes(),
        )
        .unwrap();
        relift_cache::write_if_changed(
            &apps.join("org.example.Racer.desktop"),
            "[Desktop Entry]\nExec=/usr/bin/racer --fullscreen %U\nCategories=Game;\n".as_bytes(),
        )
        .unwrap();
        let root = dir.join("sysroot");
        let (file, game) = desktop_entry(&dir.join("bin/racer"), Some(&root)).unwrap();
        assert!(file.ends_with("org.example.Racer.desktop"));
        assert!(game);
        let (_, game) = desktop_entry(&dir.join("bin/other"), Some(&root)).unwrap();
        assert!(!game);
        assert!(desktop_entry(&dir.join("bin/none"), Some(&root)).is_none());
        assert!(desktop_entry(&dir.join("bin/racer"), None).is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
