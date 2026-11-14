//! The self-description of a build and `relift info`: the document of
//! design 006 assembled from the import and `game.toml`, the C file that
//! embeds it, and the reader of a built binary.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_info::{
    Identity, Info, Mods, ModuleId, Needs, Repo, Save, Setting, TreeNeed, embed, section,
};
use relift_input::PortSet;
use relift_switch::program::Placed;
use sha2::{Digest, Sha256};

use crate::game_settings;
use crate::identity;
use crate::import::BoxError;

/// The file the embedding C source is written to, beside `program.c`.
pub const C_FILE: &str = "relift_info.c";

fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        let _ = write!(s, "{x:02x}");
    }
    s
}

fn walk(root: &Path, dir: &Path, out: &mut Vec<PathBuf>) -> std::io::Result<()> {
    for e in std::fs::read_dir(dir)? {
        let p = e?.path();
        if p.is_dir() {
            walk(root, &p, out)?;
        } else {
            out.push(p.strip_prefix(root).unwrap_or(&p).to_path_buf());
        }
    }
    Ok(())
}

/// The hash of a directory tree: SHA-256 over, for each file in sorted
/// path order, the `/`-separated relative path, a NUL and the SHA-256 of
/// its contents.
///
/// # Errors
///
/// A directory or file cannot be read.
pub fn tree_hash(root: &Path) -> std::io::Result<String> {
    let mut files = Vec::new();
    walk(root, root, &mut files)?;
    let mut keyed: Vec<(String, PathBuf)> = files
        .into_iter()
        .map(|rel| {
            let key = rel
                .components()
                .map(|c| c.as_os_str().to_string_lossy().into_owned())
                .collect::<Vec<_>>()
                .join("/");
            (key, rel)
        })
        .collect();
    keyed.sort();
    let mut all = Sha256::new();
    for (key, rel) in keyed {
        all.update(key.as_bytes());
        all.update([0]);
        all.update(Sha256::digest(std::fs::read(root.join(rel))?));
    }
    Ok(hex(&all.finalize()))
}

/// The document of a Switch build.
///
/// The import directory supplies `control.nacp` and `icon.jpg` when it has
/// them; without a NACP the title is the project name.
///
/// # Errors
///
/// A tree or file of the import cannot be read, or the NACP is malformed.
pub fn switch_document(
    import: &Path,
    game: &toml::Table,
    placed: &[Placed],
    project: &str,
    settings: Vec<Setting>,
    ports: &PortSet,
    mods: Mods,
) -> Result<Info, BoxError> {
    let g = game.get("game").and_then(|g| g.as_table());
    let title_id = g
        .and_then(|g| g.get("title_id"))
        .and_then(|v| v.as_str())
        .and_then(|s| u64::from_str_radix(s, 16).ok())
        .unwrap_or(0);
    let icon = std::fs::read(import.join("icon.jpg")).ok();
    let mut id = match std::fs::read(import.join("control.nacp")) {
        Ok(n) => identity::from_nacp(&n, title_id, icon.as_deref())?,
        Err(_) => {
            let mut id = Identity {
                platform: "switch".into(),
                title_id: Some(format!("{title_id:016x}")),
                ..Identity::default()
            };
            id.names.insert("default".into(), project.to_string());
            id
        }
    };
    id.version = id.version.or_else(|| {
        g.and_then(|g| g.get("version"))
            .and_then(|v| v.as_integer())
            .map(|v| v.to_string())
    });
    id.modules = placed
        .iter()
        .map(|p| ModuleId {
            name: p.name.clone(),
            build_id: p.nso.build_id_hex(),
        })
        .collect();
    id.repo = game.get("repo").and_then(|r| r.as_table()).map(|r| Repo {
        name: r
            .get("name")
            .and_then(|v| v.as_str())
            .unwrap_or(project)
            .to_string(),
        version: r.get("version").and_then(|v| v.as_str()).map(String::from),
    });
    let romfs = import.join("romfs");
    let mut info = Info::new(env!("CARGO_PKG_VERSION"));
    info.identity = id;
    info.needs = Needs {
        romfs: if romfs.is_dir() {
            Some(TreeNeed {
                hash: tree_hash(&romfs)?,
            })
        } else {
            None
        },
        dlc: import.join("dlc").is_dir(),
        save_folder: true,
    };
    info.mods = Mods {
        accepts: Mods::switch_accepts(),
        ..mods
    };
    info.save = Save {
        kind: "tree".into(),
        location: Some("launcher".into()),
        ..Save::default()
    };
    info.settings = game_settings::section(settings);
    info.input = relift_input::input_section(ports);
    Ok(info)
}

/// The document of a Game Boy, Game Boy Color or Game Boy Advance build:
/// the save comes from the cartridge header, and so does the identity,
/// unless a ROM hack gives the patched ROM's.
///
/// # Errors
///
/// The header does not decode.
pub fn handheld_document(
    rom: &[u8],
    gba: bool,
    settings: Vec<Setting>,
    ports: &PortSet,
    mods: Mods,
    hack: Option<&crate::mods::rom::RomHack>,
) -> Result<Info, BoxError> {
    let (id, save) = if gba {
        identity::from_gba(rom)?
    } else {
        identity::from_gb(rom)?
    };
    let mut info = Info::new(env!("CARGO_PKG_VERSION"));
    info.identity = hack.map_or(id, |h| h.identity.clone());
    info.save = save;
    info.mods = mods;
    info.settings = game_settings::section(settings);
    info.input = relift_input::input_section(ports);
    Ok(info)
}

/// The stored `mods` section with the run-time state a running binary
/// reports (`rl_hos_mods_json`: `active` and `conflicts`), as one section.
///
/// # Errors
///
/// `runtime` is not the JSON object the runtime writes.
pub fn with_runtime_mods(mut stored: Mods, runtime: &str) -> Result<Mods, BoxError> {
    let now: Mods = serde_json::from_str(runtime)?;
    stored.active = now.active;
    stored.conflicts = now.conflicts;
    Ok(stored)
}

/// The C file that embeds `info`.
#[must_use]
pub fn c_file(info: &Info) -> String {
    embed::c_source(&info.to_bytes())
}

/// `relift info [binary]`: print the document of a built binary, from its
/// section, without running it. Without an argument the project's last
/// build is read.
///
/// # Errors
///
/// The file cannot be read or holds no document.
pub fn run(binary: Option<&Path>, project_exes: &[PathBuf]) -> Result<(), BoxError> {
    use std::io::Write as _;
    let path = match binary {
        Some(p) => p.to_path_buf(),
        None => project_exes
            .iter()
            .find(|p| p.is_file())
            .cloned()
            .ok_or("no built binary of this project; run relift build or name a binary")?,
    };
    let image = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let doc = section::read(&image).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut out = std::io::stdout().lock();
    out.write_all(doc)?;
    out.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../target/tmp")
            .join(name);
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn the_tree_hash_depends_on_names_and_contents_not_on_order() {
        let a = scratch("info-tree-a");
        std::fs::create_dir_all(a.join("d")).unwrap();
        relift_cache::write_if_changed(&a.join("x"), b"1").unwrap();
        relift_cache::write_if_changed(&a.join("d/y"), b"2").unwrap();
        let b = scratch("info-tree-b");
        std::fs::create_dir_all(b.join("d")).unwrap();
        relift_cache::write_if_changed(&b.join("d/y"), b"2").unwrap();
        relift_cache::write_if_changed(&b.join("x"), b"1").unwrap();
        assert_eq!(tree_hash(&a).unwrap(), tree_hash(&b).unwrap());
        relift_cache::write_if_changed(&b.join("x"), b"3").unwrap();
        assert_ne!(tree_hash(&a).unwrap(), tree_hash(&b).unwrap());
    }

    #[test]
    fn info_without_a_binary_or_a_build_says_what_is_missing() {
        let e = run(None, &[PathBuf::from("/nonexistent/relift-game")]).unwrap_err();
        assert!(e.to_string().contains("no built binary"), "{e}");
    }

    #[test]
    fn a_handheld_document_lists_both_settings_and_the_ports() {
        let mut rom = vec![0u8; 0x100];
        rom[0xA0..0xA4].copy_from_slice(b"TEST");
        rom[0xAC..0xB0].copy_from_slice(b"ATSE");
        let game =
            game_settings::parse("[settings.hud]\ntype = \"bool\"\ndefault = true\n").unwrap();
        let mut ports = relift_input::native_ports("gba").unwrap();
        ports
            .extend_from_toml(
                "game.toml",
                relift_input::Origin::Game,
                "[input.port.dial]\nkind = \"Rotary dial\"\n[input.port.dial.control.turn]\nkind = \"axis\"\n",
            )
            .unwrap();
        let info = handheld_document(&rom, true, game, &ports, Mods::default(), None).unwrap();
        assert_eq!(info.identity.platform, "gba");
        assert_eq!(info.settings.runtime.len(), 3);
        assert_eq!(info.settings.game.len(), 1);
        let ids: Vec<&str> = info.input.ports.iter().map(|p| p.id.as_str()).collect();
        assert_eq!(ids, ["p1", "dial"]);
    }

    #[test]
    fn one_mods_section_holds_code_bundled_accepted_and_active_mods() {
        let stored = Mods {
            code: vec![relift_info::CodeMod {
                name: "fix".into(),
                hash: "ab".into(),
            }],
            bundled: vec!["mod.hd".into()],
            accepts: Mods::switch_accepts(),
            ..Mods::default()
        };
        let runtime = r#"{"active":[{"name":"alpha","path":"/m/alpha","layout":"ryujinx","romfs":true,"cheats":2,"cheats_need_rebuild":1}],"conflicts":["/a.txt: alpha wins over zeta"]}"#;
        let m = with_runtime_mods(stored.clone(), runtime).unwrap();
        assert_eq!(m.code, stored.code);
        assert_eq!(m.bundled, stored.bundled);
        assert_eq!(m.accepts.len(), 4);
        assert_eq!(m.active[0].name, "alpha");
        assert_eq!(m.active[0].cheats_need_rebuild, 1);
        assert_eq!(m.conflicts.len(), 1);
        let back: Mods = serde_json::from_str(&serde_json::to_string(&m).unwrap()).unwrap();
        assert_eq!(back, m);
        assert!(with_runtime_mods(stored, "3").is_err());
    }
}
