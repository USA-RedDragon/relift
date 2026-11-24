//! The guests relift builds besides the Switch: one table of what each
//! imports, builds and runs, which `relift import`, `relift build` and
//! `relift run` read instead of asking every guest in turn. A project
//! whose `relift.toml` names no guest of the table is a Switch project and
//! goes through [`crate::build`]'s pipeline.

use std::path::{Path, PathBuf};

use crate::cross;
use crate::gb::Built;
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

/// When a release build of a guest is packaged.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Package {
    /// For the host and for a cross target.
    Always,
    /// Only for a cross target; a host build leaves the program where it
    /// is.
    Cross,
    /// Never: the build is the program.
    Never,
}

/// Builds a guest's project: the settings, the mods, whether the build is
/// a release one, the cross target and the screen.
pub type BuildFn =
    fn(&Settings, &[PathBuf], bool, Option<cross::Target>, &mut Ui) -> Result<Built, BoxError>;

/// One guest of the table.
pub struct Guest {
    /// The guest's name in `relift.toml` (`[game] guest`).
    pub key: &'static str,
    /// The guest's name in messages.
    pub label: &'static str,
    /// What the guest imports, in messages ("a cartridge ROM").
    pub input: &'static str,
    /// Whether `relift build --dev` makes this guest's dev build (or only
    /// `relift run` does).
    pub dev_build: bool,
    /// When a release build is packaged.
    pub package: Package,
    /// Whether the import takes `--sysroot`.
    pub sysroot: bool,
    /// Whether `path` is this guest's input.
    pub is_input: fn(&Path) -> bool,
    /// Imports `path`, with the sysroot when [`Guest::sysroot`].
    pub import: fn(&Settings, &Path, Option<&Path>, &mut Ui) -> Result<(), BoxError>,
    /// Builds the project with the mods given, as a release build or not,
    /// for the host or a cross target.
    pub build: BuildFn,
    /// Builds the dev program and runs it with the arguments given;
    /// returns its exit code.
    pub run: fn(&Settings, &[String], &mut Ui) -> Result<i32, BoxError>,
}

/// Every guest besides the Switch, in the order an import asks them
/// whether a file is theirs.
pub const GUESTS: &[Guest] = &[
    Guest {
        key: "gb",
        label: "Game Boy",
        input: "a cartridge ROM",
        dev_build: true,
        package: Package::Always,
        sysroot: false,
        is_input: crate::gb::is_rom,
        import: |s, p, _, ui| crate::gb::import(s, p, ui),
        build: crate::gb::build,
        run: |s, _, ui| crate::gb::run(s, ui),
    },
    Guest {
        key: "nds",
        label: "Nintendo DS",
        input: "a cartridge image",
        dev_build: true,
        package: Package::Cross,
        sysroot: false,
        is_input: crate::nds::is_rom,
        import: |s, p, _, ui| crate::nds::import(s, p, ui),
        build: crate::nds::build,
        run: |s, _, ui| crate::nds::run(s, ui),
    },
    Guest {
        key: "gba",
        label: "Game Boy Advance",
        input: "a cartridge ROM",
        dev_build: true,
        package: Package::Always,
        sysroot: false,
        is_input: crate::gba::is_rom,
        import: |s, p, _, ui| crate::gba::import(s, p, ui),
        build: crate::gba::build,
        run: |s, _, ui| crate::gba::run(s, ui),
    },
    Guest {
        key: "sms",
        label: "Master System",
        input: "a cartridge ROM",
        dev_build: true,
        package: Package::Always,
        sysroot: false,
        is_input: crate::sms::is_rom,
        import: |s, p, _, ui| crate::sms::import(s, p, ui),
        build: |s, mods, release, target, ui| {
            if !mods.is_empty() {
                return Err("mods are not supported for Sega 8-bit ROMs yet".into());
            }
            crate::sms::build(s, release, target, ui)
        },
        run: |s, _, ui| crate::sms::run(s, ui),
    },
    Guest {
        key: "linux",
        label: "Linux",
        input: "a Linux program",
        dev_build: true,
        package: Package::Never,
        sysroot: true,
        is_input: crate::linux::is_executable,
        import: crate::linux::import,
        build: |s, _, release, target, ui| {
            let built = crate::linux::build(s, release, target, ui)?;
            Ok(Built {
                exe: built.exe,
                source: PathBuf::new(),
                title: String::new(),
                cross: None,
            })
        },
        run: crate::linux::run,
    },
    Guest {
        key: "n64",
        label: "Nintendo 64",
        input: "a cartridge ROM",
        dev_build: true,
        package: Package::Cross,
        sysroot: false,
        is_input: crate::n64::is_rom,
        import: |s, p, _, ui| crate::n64::import(s, p, ui),
        build: crate::n64::build,
        run: |s, _, ui| crate::n64::run(s, ui),
    },
];

/// The guest `key` names, if it is one of the table.
#[must_use]
pub fn by_key(key: &str) -> Option<&'static Guest> {
    GUESTS.iter().find(|g| g.key == key)
}

/// The guest of the project in the working directory (`[game] guest` of
/// its `relift.toml`); `None` for a Switch project.
#[must_use]
pub fn of_project() -> Option<&'static Guest> {
    let t: toml::Table = std::fs::read_to_string("relift.toml").ok()?.parse().ok()?;
    by_key(t.get("game")?.get("guest")?.as_str()?)
}

/// The guest whose input `path` is; `None` for a Switch dump.
#[must_use]
pub fn of_input(path: &Path) -> Option<&'static Guest> {
    GUESTS.iter().find(|g| (g.is_input)(path))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_guest_has_its_own_key() {
        let mut keys: Vec<&str> = GUESTS.iter().map(|g| g.key).collect();
        keys.sort_unstable();
        keys.dedup();
        assert_eq!(keys.len(), GUESTS.len());
        assert!(by_key("gba").is_some_and(|g| g.label == "Game Boy Advance"));
        assert!(by_key("switch").is_none());
    }
}
