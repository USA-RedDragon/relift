//! The code mods `relift build --mods` names, read from the Atmosphere and
//! Ryujinx layouts and applied in order to the imported modules.
//!
//! A mod is a directory or one patch file. In a directory relift reads
//! `exefs/` (an NSO named for its slot replaces that module; `.ips` and
//! `.pchtxt` files there are patches, as Ryujinx keeps them),
//! `exefs_patches/<name>/` (Atmosphere), patch files at the top, and the
//! same under `contents/<title id>/`. `romfs/` and `cheats/` are run-time
//! data mods and are left to the runtime.

use std::collections::BTreeMap;
use std::ops::Range;
use std::path::{Path, PathBuf};

use relift_switch_formats::nso::Nso;

use super::{ExefsPatch, hash_files, walk};
use crate::import::BoxError;

/// One code mod as read.
#[derive(Clone, Debug)]
pub struct UserMod {
    /// The mod's name: its directory or file name.
    pub name: String,
    /// Where it was read from.
    pub path: PathBuf,
    /// SHA-256 over the files used.
    pub hash: String,
    /// The NSOs that replace modules, by slot.
    pub replacements: Vec<(String, Vec<u8>)>,
    /// The exefs patches.
    pub patches: Vec<ExefsPatch>,
}

fn is_patch(rel: &str) -> bool {
    rel.ends_with(".ips") || rel.ends_with(".pchtxt")
}

/// Where in `dir` a mod's exefs files may be: `dir` itself and
/// `contents/<title id>` (any title id when `title_id` is empty).
fn roots(dir: &Path, title_id: &str) -> Vec<String> {
    let mut out = vec![String::new()];
    let contents = dir.join("contents");
    if let Ok(rd) = std::fs::read_dir(&contents) {
        let mut ids: Vec<String> = rd
            .filter_map(Result::ok)
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| title_id.is_empty() || n.eq_ignore_ascii_case(title_id))
            .collect();
        ids.sort();
        out.extend(ids.into_iter().map(|i| format!("contents/{i}/")));
    }
    out
}

/// Reads the mod at `path`; `title_id` picks the `contents/<title id>`
/// directory.
///
/// # Errors
///
/// The path is missing, a file does not parse, an exefs replacement is not
/// an NSO, or the mod is a handheld ROM patch.
pub fn read(path: &Path, title_id: &str) -> Result<UserMod, BoxError> {
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.display().to_string());
    let mut files: BTreeMap<String, Vec<u8>> = BTreeMap::new();
    let mut m = UserMod {
        name: name.clone(),
        path: path.to_path_buf(),
        hash: String::new(),
        replacements: Vec::new(),
        patches: Vec::new(),
    };
    if path.is_file() {
        if name.ends_with(".ups") || name.ends_with(".bps") {
            return Err(format!(
                "{name} is a ROM patch; ROM patches apply to handheld games, not to exefs modules"
            )
            .into());
        }
        if !is_patch(&name) {
            return Err(format!("{name}: a code mod is a directory, an .ips or a .pchtxt").into());
        }
        m.patches.push(ExefsPatch::read(path, &name)?);
        files.insert(name.clone(), std::fs::read(path)?);
        m.hash = hash_files(&files);
        return Ok(m);
    }
    if !path.is_dir() {
        return Err(format!("{}: no such mod", path.display()).into());
    }
    let all = walk(path)?;
    for root in roots(path, title_id) {
        for rel in all.iter().filter(|r| r.starts_with(&root)) {
            let sub = &rel[root.len()..];
            let full = path.join(rel);
            let used = if let Some(f) = sub.strip_prefix("exefs/") {
                if f.contains('/') {
                    false
                } else if is_patch(f) {
                    m.patches.push(ExefsPatch::read(&full, rel)?);
                    true
                } else if f.ends_with(".npdm") {
                    return Err(format!(
                        "{name}: {rel} replaces the program metadata, which code mods cannot change"
                    )
                    .into());
                } else {
                    let bytes = std::fs::read(&full)?;
                    Nso::parse(&bytes).map_err(|e| format!("{name}: {rel}: {e}"))?;
                    m.replacements.push((f.to_string(), bytes));
                    true
                }
            } else if is_patch(sub)
                && (sub.starts_with("exefs_patches/") || (root.is_empty() && !sub.contains('/')))
            {
                m.patches.push(ExefsPatch::read(&full, rel)?);
                true
            } else {
                false
            };
            if used {
                files.insert(rel.clone(), std::fs::read(&full)?);
            }
        }
    }
    if m.replacements.is_empty() && m.patches.is_empty() {
        return Err(format!(
            "{name}: no exefs replacement or patch in it (romfs and cheats are run-time mods)"
        )
        .into());
    }
    m.hash = hash_files(&files);
    Ok(m)
}

/// One module as the build loads it.
#[derive(Clone, Debug)]
pub struct Loaded {
    /// The exefs slot (`main`, `subsdk0`).
    pub slot: String,
    /// The module name.
    pub name: String,
    /// The module.
    pub nso: Nso,
}

/// What one mod wrote into one module.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Writes {
    /// The mod's index in the list.
    pub user_mod: usize,
    /// The module's index.
    pub module: usize,
    /// The image ranges written.
    pub ranges: Vec<Range<u64>>,
    /// Whether the mod replaced the whole module.
    pub replaced: bool,
}

/// What [`apply`] did.
#[derive(Clone, Debug, Default)]
pub struct Applied {
    /// The writes, in order.
    pub writes: Vec<Writes>,
    /// Things worth telling: patches for no module of the build.
    pub notes: Vec<String>,
}

/// Applies `mods` in order to `modules`: replacements first within a mod,
/// then its patches, each to the module whose build id it names.
///
/// # Errors
///
/// Two mods replace one module, a patch runs past its module, or two mods
/// write different bytes at one address: the order alone would then decide
/// which mod's change is lost, so the build is refused.
pub fn apply(mods: &[UserMod], modules: &mut [Loaded]) -> Result<Applied, BoxError> {
    let mut out = Applied::default();
    let mut replaced_by: BTreeMap<usize, usize> = BTreeMap::new();
    let mut bytes_by: Vec<BTreeMap<u64, (usize, u8)>> = vec![BTreeMap::new(); modules.len()];
    for (k, m) in mods.iter().enumerate() {
        for (slot, bytes) in &m.replacements {
            let i = modules
                .iter()
                .position(|x| &x.slot == slot)
                .ok_or_else(|| {
                    format!("{}: exefs/{slot} replaces no module of this game", m.name)
                })?;
            if let Some(&j) = replaced_by.get(&i) {
                return Err(format!(
                    "{} and {} both replace exefs/{slot}; remove one of them",
                    mods[j].name, m.name
                )
                .into());
            }
            replaced_by.insert(i, k);
            let nso = Nso::parse(bytes).map_err(|e| format!("{}: exefs/{slot}: {e}", m.name))?;
            out.writes.push(Writes {
                user_mod: k,
                module: i,
                ranges: std::iter::once(0..nso.image.len() as u64).collect(),
                replaced: true,
            });
            modules[i].nso = nso;
        }
        for p in &m.patches {
            let Some(i) = modules.iter().position(|x| p.matches(&x.nso.build_id)) else {
                out.notes.push(format!(
                    "{}: {} is for build id {}, which no module of this game has",
                    m.name, p.file, p.build_id
                ));
                continue;
            };
            let before = modules[i].nso.image.clone();
            let ranges = p
                .apply(&mut modules[i].nso.image)
                .map_err(|e| format!("{}: {e}", m.name))?;
            for r in &ranges {
                for at in r.clone() {
                    let b = modules[i].nso.image[at as usize];
                    if let Some(&(j, old)) = bytes_by[i].get(&at)
                        && j != k
                        && old != b
                    {
                        return Err(format!(
                            "{} and {} write different bytes at {}+{at:#x}; the order would drop one of the changes, so the build is refused",
                            mods[j].name, m.name, modules[i].name
                        )
                        .into());
                    }
                    if before[at as usize] != b || bytes_by[i].contains_key(&at) {
                        bytes_by[i].insert(at, (k, b));
                    }
                }
            }
            out.writes.push(Writes {
                user_mod: k,
                module: i,
                ranges,
                replaced: false,
            });
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_switch_formats::nso::{NsoSpec, build};

    fn nso(fill: u8, id: u8) -> Vec<u8> {
        let mut build_id = [0u8; 32];
        build_id[..20].fill(id);
        build(&NsoSpec {
            text: vec![fill; 0x40],
            rodata: vec![0; 16],
            data: vec![0; 16],
            build_id,
            ..NsoSpec::default()
        })
    }

    fn modules() -> Vec<Loaded> {
        vec![Loaded {
            slot: "main".into(),
            name: "main".into(),
            nso: Nso::parse(&nso(0, 0xab)).expect("nso"),
        }]
    }

    #[test]
    fn patches_apply_in_order_by_build_id() {
        let id = "ab".repeat(20);
        let mk = |name: &str, records: Vec<(u32, &[u8])>| UserMod {
            name: name.into(),
            path: PathBuf::new(),
            hash: String::new(),
            replacements: Vec::new(),
            patches: vec![ExefsPatch {
                file: format!("{id}.ips"),
                build_id: id.clone(),
                records: records
                    .into_iter()
                    .map(|(o, b)| relift_switch_formats::pchtxt::Record {
                        offset: u64::from(o) + 0x100,
                        bytes: b.to_vec(),
                    })
                    .collect(),
                kind: "ips32",
            }],
        };
        let mut m = modules();
        let a = apply(
            &[mk("a", vec![(4, &[1, 2])]), mk("b", vec![(8, &[3])])],
            &mut m,
        )
        .expect("applies");
        assert_eq!(&m[0].nso.image[4..9], &[1, 2, 0, 0, 3]);
        assert_eq!(a.writes.len(), 2);
        let mut m = modules();
        assert!(
            apply(
                &[mk("a", vec![(4, &[1])]), mk("b", vec![(4, &[1])])],
                &mut m
            )
            .is_ok()
        );
        let mut m = modules();
        let e = apply(
            &[mk("a", vec![(4, &[1])]), mk("b", vec![(4, &[2])])],
            &mut m,
        )
        .expect_err("ambiguous")
        .to_string();
        assert!(e.contains("a and b write different bytes"), "{e}");
        let mut other = mk("c", vec![(4, &[1])]);
        other.patches[0].build_id = "ff".into();
        let mut m = modules();
        let a = apply(&[other], &mut m).expect("applies");
        assert_eq!(a.notes.len(), 1);
    }

    #[test]
    fn two_replacements_of_one_module_are_refused() {
        let r = |name: &str| UserMod {
            name: name.into(),
            path: PathBuf::new(),
            hash: String::new(),
            replacements: vec![("main".into(), nso(1, 1))],
            patches: Vec::new(),
        };
        let mut m = modules();
        let a = apply(&[r("a")], &mut m).expect("applies");
        assert!(a.writes[0].replaced);
        assert_eq!(m[0].nso.build_id[..20], [1; 20]);
        let mut m = modules();
        assert!(apply(&[r("a"), r("b")], &mut m).is_err());
    }
}
