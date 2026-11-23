//! The self-description of a build and `relift info`: the document of
//! design 006 assembled from the import and `relift.toml`, the C file that
//! embeds it, and the reader of a built binary.

use std::collections::HashMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_info::{
    Identity, Info, Mods, ModuleId, Needs, Repo, Save, Setting, TreeNeed, embed, section,
};
use relift_input::PortSet;
use relift_switch::program::Placed;
use relift_switch_formats::romfs_image::Found;
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

/// A file's identity as the tree-hash cache sees it: if none of these
/// changed, neither did its contents.
#[derive(Clone, Copy, PartialEq, Eq)]
struct Stamp {
    size: u64,
    mtime: i128,
    ctime: i128,
    inode: u64,
}

fn nanos(t: std::io::Result<std::time::SystemTime>) -> i128 {
    match t {
        Ok(t) => match t.duration_since(std::time::UNIX_EPOCH) {
            Ok(d) => i128::try_from(d.as_nanos()).unwrap_or(0),
            Err(e) => -i128::try_from(e.duration().as_nanos()).unwrap_or(0),
        },
        Err(_) => 0,
    }
}

#[cfg(unix)]
fn stamp(m: &std::fs::Metadata) -> Stamp {
    use std::os::unix::fs::MetadataExt as _;
    Stamp {
        size: m.len(),
        mtime: i128::from(m.mtime()) * 1_000_000_000 + i128::from(m.mtime_nsec()),
        ctime: i128::from(m.ctime()) * 1_000_000_000 + i128::from(m.ctime_nsec()),
        inode: m.ino(),
    }
}

#[cfg(not(unix))]
fn stamp(m: &std::fs::Metadata) -> Stamp {
    Stamp {
        size: m.len(),
        mtime: nanos(m.modified()),
        ctime: 0,
        inode: 0,
    }
}

/// Files modified this close to the moment they were hashed may still be
/// written to within the clock's granularity, so their stamp proves
/// nothing and they are hashed again next time.
const RACY: i128 = 2_000_000_000;

const HASHES_MAGIC: &str = "relift romfs hashes 1";

fn read_hashes(path: &Path) -> HashMap<String, (Stamp, String)> {
    let mut out = HashMap::new();
    let Ok(text) = std::fs::read_to_string(path) else {
        return out;
    };
    let mut lines = text.lines();
    if lines.next() != Some(HASHES_MAGIC) {
        return out;
    }
    for l in lines {
        let mut it = l.splitn(6, ' ');
        let (Some(size), Some(mtime), Some(ctime), Some(inode), Some(hash), Some(key)) = (
            it.next().and_then(|v| v.parse().ok()),
            it.next().and_then(|v| v.parse().ok()),
            it.next().and_then(|v| v.parse().ok()),
            it.next().and_then(|v| v.parse().ok()),
            it.next().filter(|h| h.len() == 64),
            it.next(),
        ) else {
            continue;
        };
        out.insert(
            key.to_string(),
            (
                Stamp {
                    size,
                    mtime,
                    ctime,
                    inode,
                },
                hash.to_string(),
            ),
        );
    }
    out
}

fn hash_file(path: &Path) -> std::io::Result<[u8; 32]> {
    use std::io::Read as _;
    let mut f = std::fs::File::open(path)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().into())
}

/// The hash of a directory tree: SHA-256 over, for each file in sorted
/// path order, the `/`-separated relative path, a NUL and the SHA-256 of
/// its contents.
///
/// # Errors
///
/// A directory or file cannot be read.
pub fn tree_hash(root: &Path) -> std::io::Result<String> {
    tree_hash_cached(root, None)
}

/// [`tree_hash`] with the SHA-256 of each file kept in `cache` under the
/// file's size, modification and change times and inode: a file whose
/// stamp is unchanged is not read again, a file whose stamp changed is
/// hashed again, and so is a file modified within two seconds of the
/// previous hash. The result is the same as without the cache.
///
/// # Errors
///
/// A directory or file cannot be read.
pub fn tree_hash_cached(root: &Path, cache: Option<&Path>) -> std::io::Result<String> {
    tree_hash_at(root, cache, nanos(Ok(std::time::SystemTime::now())))
}

fn tree_hash_at(root: &Path, cache: Option<&Path>, started: i128) -> std::io::Result<String> {
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
    let known = cache.map(read_hashes).unwrap_or_default();
    let mut hashes: Vec<Option<(Stamp, [u8; 32])>> = vec![None; keyed.len()];
    let mut todo: Vec<usize> = Vec::new();
    for (i, (key, rel)) in keyed.iter().enumerate() {
        let st = stamp(&std::fs::metadata(root.join(rel))?);
        let hit = known
            .get(key)
            .filter(|(s, _)| *s == st)
            .and_then(|(_, h)| decode_hex(h));
        match hit {
            Some(h) => hashes[i] = Some((st, h)),
            None => todo.push(i),
        }
    }
    let next = std::sync::atomic::AtomicUsize::new(0);
    let done = std::sync::Mutex::new(Vec::new());
    let failed = std::sync::Mutex::new(None);
    relift_proc::workers::run(relift_proc::workers::jobs().min(todo.len()), || {
        loop {
            let n = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let Some(&i) = todo.get(n) else { break };
            let path = root.join(&keyed[i].1);
            let r = std::fs::metadata(&path)
                .map(|m| stamp(&m))
                .and_then(|st| hash_file(&path).map(|h| (i, st, h)));
            match r {
                Ok(v) => done.lock().expect("lock").push(v),
                Err(e) => {
                    *failed.lock().expect("lock") = Some(e);
                    break;
                }
            }
        }
    });
    if let Some(e) = failed.into_inner().expect("lock") {
        return Err(e);
    }
    for (i, st, h) in done.into_inner().expect("lock") {
        hashes[i] = Some((st, h));
    }
    let mut all = Sha256::new();
    let mut text = String::from(HASHES_MAGIC);
    text.push('\n');
    for ((key, _), h) in keyed.iter().zip(&hashes) {
        let (st, h) = h.expect("every file hashed");
        all.update(key.as_bytes());
        all.update([0]);
        all.update(h);
        if started - st.mtime > RACY && !key.contains('\n') {
            let _ = writeln!(
                text,
                "{} {} {} {} {} {key}",
                st.size,
                st.mtime,
                st.ctime,
                st.inode,
                hex(&h)
            );
        }
    }
    if let Some(c) = cache {
        let _ = relift_cache::write_if_changed(c, text.as_bytes());
    }
    Ok(hex(&all.finalize()))
}

fn decode_hex(s: &str) -> Option<[u8; 32]> {
    let mut out = [0u8; 32];
    if s.len() != 64 {
        return None;
    }
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(s.get(2 * i..2 * i + 2)?, 16).ok()?;
    }
    Some(out)
}

/// The document of a Switch build.
///
/// The identity of a Switch title whose import holds no NACP: `name` (the package's name record, else the project name) as the only title, and the control
/// icon when the import has one (a package can carry its icon without its NACP).
fn no_nacp_identity(title_id: u64, project: &str, icon: Option<&[u8]>) -> Identity {
    let mut id = Identity {
        platform: "switch".into(),
        title_id: Some(format!("{title_id:016x}")),
        ..Identity::default()
    };
    id.names.insert("default".into(), project.to_string());
    id.icon = icon.and_then(crate::icon::from_image);
    id
}

/// Opens `path` as a `.nup`; `None` when it is not one or cannot be read.
#[must_use]
pub fn open_nup(path: &Path) -> Option<relift_switch_formats::nup::Nup> {
    if path.extension().and_then(|e| e.to_str()) != Some("nup") {
        return None;
    }
    let s = relift_switch_formats::storage::FileStorage::open(path).ok()?;
    relift_switch_formats::nup::Nup::open(std::sync::Arc::new(s)).ok()
}

/// The control data of the title in the dump `relift.toml` names
/// (`dump`, checked against `dump_toc`): its NACP, name and icon, from the
/// newest update that has each, else the base.
#[derive(Debug, Default)]
pub struct DumpControl {
    pub nacp: Option<Vec<u8>>,
    pub name: Option<String>,
    pub icon: Option<Vec<u8>>,
}

/// The control data of the game's dump. `Ok(None)` when `relift.toml`
/// names no dump (an import by an older relift, or not a `.nup`); an error
/// that says why when the dump is gone or is not the one imported.
///
/// # Errors
///
/// The dump cannot be opened, or its table hash differs from `dump_toc`.
pub fn dump_control(game: &toml::Table) -> Result<Option<DumpControl>, String> {
    use relift_switch_formats::nup::{PackageKind, TLV_ICON, TLV_NACP, TLV_NAME};
    let g = game.get("game").and_then(|g| g.as_table());
    let (Some(path), Some(toc)) = (
        g.and_then(|g| g.get("dump")).and_then(|v| v.as_str()),
        g.and_then(|g| g.get("dump_toc")).and_then(|v| v.as_str()),
    ) else {
        return Ok(None);
    };
    let nup =
        open_nup(Path::new(path)).ok_or_else(|| format!("the dump {path} cannot be opened"))?;
    let have: String = nup
        .superblock()
        .toc_sha256
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    if have != toc {
        return Err(format!(
            "the dump {path} is not the one imported (its table hash changed)"
        ));
    }
    let tid = g
        .and_then(|g| g.get("title_id"))
        .and_then(|v| v.as_str())
        .and_then(|s| u64::from_str_radix(s, 16).ok())
        .unwrap_or(0);
    let mut order: Vec<_> = nup
        .packages()
        .iter()
        .filter(|p| {
            p.record.title_id & !0xfff == tid & !0xfff && p.record.kind != PackageKind::Addon
        })
        .collect();
    order.sort_by_key(|p| p.record.kind != PackageKind::Update);
    let find = |ty| order.iter().find_map(|p| p.tlv(ty)).map(<[u8]>::to_vec);
    Ok(Some(DumpControl {
        nacp: find(TLV_NACP),
        name: find(TLV_NAME)
            .and_then(|b| String::from_utf8(b).ok())
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty()),
        icon: find(TLV_ICON),
    }))
}

/// What the game will show in place of its own name or icon, in one
/// line: a dump without an icon shows a generated one; an import by an
/// older relift whose dump is unknown, gone or changed shows the project
/// folder's name. `None` when both are the title's.
#[must_use]
pub fn identity_gaps(
    import: &Path,
    game: &toml::Table,
    id: &Identity,
    project: &str,
) -> Option<String> {
    let dump = dump_control(game);
    let named = import.join("control.nacp").is_file()
        || import
            .join(relift_switch_formats::import::CONTROL_TITLE)
            .is_file()
        || matches!(&dump, Ok(Some(d)) if d.nacp.is_some() || d.name.is_some());
    if named && id.icon.is_none() {
        return Some("note: the game's dump carries no icon: a generated one is shown".to_string());
    }
    let mut gaps = Vec::new();
    if !named {
        gaps.push(format!("its name (shown as \"{project}\")"));
    }
    if id.icon.is_none() {
        gaps.push("its icon (a generated one is shown)".to_string());
    }
    let why = match &dump {
        Err(e) => format!(" and {e}"),
        _ => String::new(),
    };
    (!gaps.is_empty()).then(|| {
        format!(
            "warning: the import in {} has no control data for {}{why}: run relift import again from the dump to show the game's own",
            import.display(),
            gaps.join(" or ")
        )
    })
}

/// The import directory supplies `control.nacp` and `icon.jpg` (JPEG or,
/// from a Switch 2 package, WebP) when it has them; without a NACP the title
/// is the package's name (`title.txt`), else the project name.
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
    let mut icon = std::fs::read(import.join("icon.jpg")).ok();
    let mut nacp = std::fs::read(import.join("control.nacp")).ok();
    let mut named =
        std::fs::read_to_string(import.join(relift_switch_formats::import::CONTROL_TITLE))
            .ok()
            .map(|t| t.trim().to_string())
            .filter(|t| !t.is_empty());
    if nacp.is_none()
        && (named.is_none() || icon.is_none())
        && let Ok(Some(d)) = dump_control(game)
    {
        icon = icon.or(d.icon);
        nacp = d.nacp;
        named = named.or(d.name);
    }
    let mut id = match nacp {
        Some(n) => identity::from_nacp(&n, title_id, icon.as_deref())?,
        None => no_nacp_identity(
            title_id,
            named.as_deref().unwrap_or(project),
            icon.as_deref(),
        ),
    };
    id.default_language = id.first_language();
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
    let mut info = Info::new(env!("CARGO_PKG_VERSION"));
    info.identity = id;
    info.needs = Needs {
        romfs: match crate::gamedata::romfs(import) {
            Some(Found::Tree(romfs)) => Some(TreeNeed {
                hash: tree_hash_cached(&romfs, Some(&import.with_file_name("romfs-hashes.txt")))?,
            }),
            Some(found) => Some(TreeNeed {
                hash: crate::gamedata::tree_hash(
                    &found,
                    &import.with_file_name("romfs-digests.tsv"),
                )?,
            }),
            None => None,
        },
        dlc: crate::gamedata::dlc(import).is_some(),
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
    name: &str,
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
    info.identity.names = std::iter::once(("default".to_string(), name.to_string())).collect();
    info.identity.default_language = info.identity.first_language();
    info.save = save;
    info.mods = mods;
    info.settings = if gba {
        game_settings::section(settings)
    } else {
        game_settings::gb_section(settings)
    };
    info.input = relift_input::input_section(ports);
    Ok(info)
}

/// The document of a Linux program build: the name and the platform; the
/// program has no header to read more from.
#[must_use]
pub fn linux_document(name: &str) -> Info {
    let mut info = Info::new(env!("CARGO_PKG_VERSION"));
    info.identity.platform = "linux".into();
    info.identity.names = std::iter::once(("default".to_string(), name.to_string())).collect();
    info.identity.default_language = info.identity.first_language();
    info.save = Save {
        kind: "none".into(),
        ..Save::default()
    };
    info
}

/// The document of a Master System, Game Gear or SG-1000 build
/// (`platform` `sms`, `gg` or `sg1000`): the name, the platform and the input section of its ports;
/// the cartridge has no title or save information in its header.
#[must_use]
pub fn sms_document(name: &str, platform: &str, settings: Vec<Setting>, ports: &PortSet) -> Info {
    let mut info = Info::new(env!("CARGO_PKG_VERSION"));
    info.identity.platform = platform.into();
    info.identity.names = std::iter::once(("default".to_string(), name.to_string())).collect();
    info.identity.default_language = info.identity.first_language();
    info.save = Save {
        kind: "none".into(),
        ..Save::default()
    };
    info.settings = game_settings::section(settings);
    info.input = relift_input::input_section(ports);
    info
}

/// The document of a Nintendo 64 build: the identity from the cartridge
/// header (title, game id, version), the runtime settings with the picture
/// settings at the developer's `picture` defaults ([`crate::n64_picture`])
/// and the input section of its ports; the save kind is not known before the
/// game runs.
#[must_use]
pub fn n64_document(
    name: &str,
    rom: &relift_n64_formats::Rom,
    settings: Vec<Setting>,
    picture: &[String],
    ports: &PortSet,
) -> Info {
    let h = rom.header();
    let mut info = Info::new(env!("CARGO_PKG_VERSION"));
    info.identity.platform = "n64".into();
    info.identity.title_id = Some(
        h.game_id
            .iter()
            .map(|b| relift_n64_formats::Header::printable(*b))
            .collect(),
    );
    info.identity.version = Some(h.version.to_string());
    info.identity.names = std::iter::once(("default".to_string(), name.to_string())).collect();
    info.identity.default_language = info.identity.first_language();
    info.save = Save {
        kind: "none".into(),
        ..Save::default()
    };
    info.settings = game_settings::section(settings);
    info.settings
        .runtime
        .extend(crate::n64_picture::with_defaults(picture));
    info.input = relift_input::input_section(ports);
    info
}

/// The document of a Nintendo DS build: identity from the cartridge header,
/// no save yet, no input ports yet.
///
/// # Errors
///
/// The header is too short.
pub fn nds_document(
    name: &str,
    rom: &[u8],
    settings: Vec<Setting>,
    ports: &PortSet,
) -> Result<Info, BoxError> {
    let (id, save) = identity::from_nds(rom)?;
    let mut info = Info::new(env!("CARGO_PKG_VERSION"));
    info.identity = id;
    info.identity.names = std::iter::once(("default".to_string(), name.to_string())).collect();
    info.identity.default_language = info.identity.first_language();
    info.save = save;
    info.settings = game_settings::nds_section(settings);
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

/// The C definition of `rl_image_id`, the hash of a program's module images.
#[must_use]
pub fn image_id_c(image_id: &[u8; 32]) -> String {
    let mut t = String::from("const unsigned char rl_image_id[32] = {");
    for b in image_id {
        let _ = write!(t, " {b},");
    }
    t.push_str(" };\n");
    t
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
    let appimage = image.get(8..11) == Some(b"AI\x02");
    let stored;
    let doc = if appimage {
        stored = crate::appimage::read_info(&path)?;
        stored.as_slice()
    } else {
        section::read(&image).map_err(|e| format!("{}: {e}", path.display()))?
    };
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
    fn the_cached_tree_hash_matches_and_follows_changes() {
        let a = scratch("info-tree-cached");
        std::fs::create_dir_all(a.join("d")).unwrap();
        relift_cache::write_if_changed(&a.join("x"), b"1").unwrap();
        relift_cache::write_if_changed(&a.join("d/y"), b"22").unwrap();
        let later = nanos(Ok(std::time::SystemTime::now())) + 3_600_000_000_000;
        let cache = scratch("info-tree-cached-store").join("hashes.txt");
        let plain = tree_hash(&a).unwrap();
        assert_eq!(tree_hash_at(&a, Some(&cache), later).unwrap(), plain);
        assert_eq!(std::fs::read_to_string(&cache).unwrap().lines().count(), 3);
        let text = std::fs::read_to_string(&cache).unwrap();
        let forged = text.replace(&hex(&Sha256::digest(b"1")), &"0".repeat(64));
        assert_ne!(forged, text);
        relift_cache::write_if_changed(&cache, forged.as_bytes()).unwrap();
        assert_ne!(tree_hash_at(&a, Some(&cache), later).unwrap(), plain);
        relift_cache::write_if_changed(&cache, text.as_bytes()).unwrap();
        relift_cache::write_if_changed(&a.join("x"), b"3").unwrap();
        assert_ne!(tree_hash_at(&a, Some(&cache), later).unwrap(), plain);
        assert_eq!(
            tree_hash_at(&a, Some(&cache), later).unwrap(),
            tree_hash(&a).unwrap()
        );
        assert_eq!(std::fs::read_to_string(&cache).unwrap().lines().count(), 3);
        tree_hash_at(&a, Some(&cache), 0).unwrap();
        assert_eq!(std::fs::read_to_string(&cache).unwrap().lines().count(), 1);
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
                "relift.toml",
                relift_input::Origin::Game,
                "[input.port.dial]\nkind = \"Rotary dial\"\n[input.port.dial.control.turn]\nkind = \"axis\"\n",
            )
            .unwrap();
        let info = handheld_document(
            "My Project",
            &rom,
            true,
            game,
            &ports,
            Mods::default(),
            None,
        )
        .unwrap();
        assert_eq!(info.identity.platform, "gba");
        assert_eq!(info.identity.names.len(), 1);
        assert_eq!(info.identity.title(&["French"]), Some("My Project"));
        assert_eq!(info.identity.default_language.as_deref(), Some("default"));
        assert!(info.identity.icon.is_none());
        assert_eq!(info.settings.runtime.len(), 39);
        assert_eq!(info.settings.game.len(), 1);
        let ids: Vec<&str> = info.input.ports.iter().map(|p| p.id.as_str()).collect();
        assert_eq!(ids, ["p1", "dial"]);
    }

    #[test]
    fn an_import_with_an_icon_and_no_nacp_keeps_the_icon_and_the_project_name() {
        let id = no_nacp_identity(0x1234, "proj", Some(include_bytes!("../tests/icon.jpg")));
        assert_eq!(id.title(&[]), Some("proj"));
        let icon = id.icon.expect("the icon");
        assert_eq!((icon.width, icon.height), (16, 8));
        assert!(no_nacp_identity(1, "p", None).icon.is_none());
    }

    #[test]
    fn a_package_import_gives_the_titles_name_and_its_webp_icon_not_the_folders() {
        let d = std::env::temp_dir().join(format!("relift-info-webp-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let mut webp = Vec::new();
        image_webp::WebPEncoder::new(&mut webp)
            .encode(&[200u8; 12 * 6 * 3], 12, 6, image_webp::ColorType::Rgb8)
            .unwrap();
        relift_cache::write_if_changed(&d.join("icon.jpg"), &webp).unwrap();
        relift_cache::write_if_changed(
            &d.join(relift_switch_formats::import::CONTROL_TITLE),
            b"A Title\n",
        )
        .unwrap();
        let game: toml::Table =
            toml::from_str("[game]\ntitle_id = \"0400000000001000\"\n").unwrap();
        let info = switch_document(
            &d,
            &game,
            &[],
            "fe",
            Vec::new(),
            &PortSet::default(),
            Mods::default(),
        )
        .unwrap();
        assert_eq!(info.identity.title(&[]), Some("A Title"));
        let icon = info
            .identity
            .icon
            .as_ref()
            .expect("the title's icon, not a generated one");
        assert_eq!((icon.width, icon.height), (12, 6));
        assert!(identity_gaps(&d, &game, &info.identity, "fe").is_none());
        let launcher = crate::icon::launcher_pngs(Some(icon), "fe");
        assert_ne!(launcher, crate::icon::launcher_pngs(None, "fe"));
        std::fs::remove_file(d.join(relift_switch_formats::import::CONTROL_TITLE)).unwrap();
        let w = identity_gaps(&d, &game, &info.identity, "fe").unwrap();
        assert!(w.contains("\"fe\"") && w.contains("relift import"), "{w}");
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn an_old_import_takes_the_name_and_icon_from_its_dump_and_checks_its_hash() {
        use relift_switch_formats::nup::{self, BuildPackage, PackageKind, TLV_ICON, TLV_NAME};
        let d = std::env::temp_dir().join(format!("relift-info-dump-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("import")).unwrap();
        let mut webp = Vec::new();
        image_webp::WebPEncoder::new(&mut webp)
            .encode(&[90u8; 4 * 4 * 3], 4, 4, image_webp::ColorType::Rgb8)
            .unwrap();
        let bytes = nup::build(&[BuildPackage {
            title_id: 0x0400_0000_0000_1000,
            version: 0,
            kind: PackageKind::Base,
            standalone: false,
            files: vec![("exefs/main", b"x".as_slice())],
            empty_dirs: Vec::new(),
            meta: vec![(TLV_NAME, b"Dump Title".to_vec()), (TLV_ICON, webp)],
        }]);
        let dump = d.join("game.nup");
        relift_cache::write_if_changed(&dump, &bytes).unwrap();
        let toc: String = open_nup(&dump)
            .unwrap()
            .superblock()
            .toc_sha256
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
        let text = format!(
            "[game]\ntitle_id = \"0400000000001000\"\ndump = {:?}\ndump_toc = \"{toc}\"\n",
            dump.to_str().unwrap()
        );
        let game: toml::Table = toml::from_str(&text).unwrap();
        let import = d.join("import");
        let doc = |game: &toml::Table| {
            switch_document(
                &import,
                game,
                &[],
                "fe",
                Vec::new(),
                &PortSet::default(),
                Mods::default(),
            )
            .unwrap()
        };
        let info = doc(&game);
        assert_eq!(info.identity.title(&[]), Some("Dump Title"));
        assert_eq!(
            info.identity.icon.as_ref().map(|i| (i.width, i.height)),
            Some((4, 4))
        );
        assert!(identity_gaps(&import, &game, &info.identity, "fe").is_none());
        let changed: toml::Table = toml::from_str(&text.replace(&toc, &"0".repeat(64))).unwrap();
        let info = doc(&changed);
        assert_eq!(info.identity.title(&[]), Some("fe"));
        let w = identity_gaps(&import, &changed, &info.identity, "fe").unwrap();
        assert!(w.contains("not the one imported"), "{w}");
        std::fs::remove_dir_all(&d).unwrap();
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
