//! The self-description of a build and `relift info`: the document of
//! design 006 assembled from the import and `game.toml`, the C file that
//! embeds it, and the reader of a built binary.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_info::{Identity, Info, ModuleId, Needs, Repo, Save, TreeNeed, embed, section};
use relift_switch::program::Placed;
use sha2::{Digest, Sha256};

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
    info.save = Save {
        kind: "tree".into(),
        location: Some("launcher".into()),
        ..Save::default()
    };
    Ok(info)
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
        std::fs::write(a.join("x"), b"1").unwrap();
        std::fs::write(a.join("d/y"), b"2").unwrap();
        let b = scratch("info-tree-b");
        std::fs::create_dir_all(b.join("d")).unwrap();
        std::fs::write(b.join("d/y"), b"2").unwrap();
        std::fs::write(b.join("x"), b"1").unwrap();
        assert_eq!(tree_hash(&a).unwrap(), tree_hash(&b).unwrap());
        std::fs::write(b.join("x"), b"3").unwrap();
        assert_ne!(tree_hash(&a).unwrap(), tree_hash(&b).unwrap());
    }

    #[test]
    fn info_without_a_binary_or_a_build_says_what_is_missing() {
        let e = run(None, &[PathBuf::from("/nonexistent/relift-game")]).unwrap_err();
        assert!(e.to_string().contains("no built binary"), "{e}");
    }
}
