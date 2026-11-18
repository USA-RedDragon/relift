//! The console's shared system fonts from the player's own system data.
//!
//! `relift import --system-data <dir>` takes a folder of decrypted font
//! system data titles as switch-tools exports them from the player's
//! console: one folder per title id (`0100000000000810` to
//! `0100000000000814`) holding the title's romfs as `romfs.img`, a romfs map
//! or an extracted `romfs/` tree. Each `.bfttf` is decoded into
//! `fonts/<type>.ttf` in the import, `<type>` the `SharedFontType`, which the
//! runtime's pl:u serves (`RELIFT_FONTS`). relift ships no font data.
//!
//! For the types the player has not imported, `relift build` makes the open
//! fallback (relift-switch-fonts) into `fonts/` beside the build: Noto Sans
//! CJK, fetched once into `<cache>/sdk/fonts/<tag>/` and accepted only with
//! its pinned SHA-256, cut to each type's standard character set. pl:u serves
//! it where the player's own font is missing (`RELIFT_FONTS_FALLBACK`).

use std::path::{Path, PathBuf};

use relift_switch_fonts::{Charset, Source};
use relift_switch_formats::bfttf::{self, FontType};
use relift_switch_formats::romfs::Romfs;
use relift_switch_formats::romfs_image;

use crate::import::BoxError;

/// The folder in the import.
pub const DIR: &str = "fonts";

/// The decoded fonts of `import`, if any.
#[must_use]
pub fn dir(import: &Path) -> Option<PathBuf> {
    let d = import.join(DIR);
    d.is_dir().then_some(d)
}

/// The bytes of `name` in the romfs of title folder `title`, if it has one.
fn read_font(title: &Path, name: &str) -> Result<Option<Vec<u8>>, BoxError> {
    let tree = title.join("romfs").join(name);
    if tree.is_file() {
        return Ok(Some(std::fs::read(tree)?));
    }
    let Some(found) = romfs_image::find(title) else {
        return Ok(None);
    };
    let romfs = Romfs::open(romfs_image::open(found.path())?)?;
    let Some(f) = romfs.files().iter().find(|f| f.path == name) else {
        return Ok(None);
    };
    let mut out = Vec::with_capacity(usize::try_from(f.size).unwrap_or(0));
    romfs.copy_file(f, &mut out)?;
    Ok(Some(out))
}

/// Decodes every shared font `system_data` holds into `out`, replacing what
/// was there; returns the types found and the types missing.
///
/// # Errors
///
/// A romfs or font is malformed, or a file cannot be read or written.
pub fn import(system_data: &Path, out: &Path) -> Result<(Vec<FontType>, Vec<FontType>), BoxError> {
    if !system_data.is_dir() {
        return Err(format!("--system-data: {} is not a folder", system_data.display()).into());
    }
    if out.exists() {
        std::fs::remove_dir_all(out)?;
    }
    let (mut found, mut missing) = (Vec::new(), Vec::new());
    for t in FontType::ALL {
        let (title, name) = t.source();
        let folder = system_data.join(format!("{title:016x}"));
        match read_font(&folder, name)? {
            Some(file) => {
                let font = bfttf::decode(&file)
                    .map_err(|e| format!("{}: {e}", folder.join(name).display()))?;
                std::fs::create_dir_all(out)?;
                relift_cache::write_if_changed(&out.join(format!("{}.ttf", t.index())), &font)?;
                found.push(t);
            }
            None => missing.push(t),
        }
    }
    Ok((found, missing))
}

/// The variable the runtime reads the fallback folder from.
pub const FALLBACK_ENV: &str = "RELIFT_FONTS_FALLBACK";

/// The source and character set of `t`'s fallback, if relift makes one.
#[must_use]
pub fn fallback_of(t: FontType) -> Option<(Source, Charset)> {
    match t {
        FontType::Standard => Some((relift_switch_fonts::NOTO_JP, Charset::Japanese)),
        FontType::ChineseSimplified => Some((relift_switch_fonts::NOTO_SC, Charset::Gb2312)),
        FontType::ChineseSimplifiedExt => Some((relift_switch_fonts::NOTO_SC, Charset::Empty)),
        FontType::ChineseTraditional => Some((relift_switch_fonts::NOTO_TC, Charset::Big5)),
        FontType::Korean => Some((relift_switch_fonts::NOTO_KR, Charset::KsX1001)),
        FontType::NintendoExt => None,
    }
}

/// Where `source` is kept in the cache.
#[must_use]
pub fn source_path(cache_root: &Path, source: &Source) -> PathBuf {
    cache_root
        .join("sdk")
        .join("fonts")
        .join(relift_switch_fonts::NOTO_TAG)
        .join(source.name)
}

/// The bytes of `source`: the cached file when it matches its pinned hash,
/// else fetched, checked and moved into place in one rename.
fn fetch(cache_root: &Path, source: &Source) -> Result<Vec<u8>, BoxError> {
    let dest = source_path(cache_root, source);
    if !crate::appimage_runtime::matches(&dest, source.sha256) {
        let dir = dest.parent().ok_or("the font cache has no folder")?;
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        let part = dir.join(format!(".part-{}", crate::appimage_runtime::unique()));
        let got = crate::appimage_runtime::download(source.url, &part).and_then(|()| {
            if crate::appimage_runtime::matches(&part, source.sha256) {
                std::fs::rename(&part, &dest).map_err(|e| e.to_string())
            } else {
                Err(format!(
                    "it does not have the pinned SHA-256 {}",
                    source.sha256
                ))
            }
        });
        let _ = std::fs::remove_file(&part);
        got.map_err(|e| {
            format!(
                "{} is not in the cache and could not be fetched from {}: {e}\n\
                 Download it on a machine with network access and put it at {} (SHA-256 {})",
                source.name,
                source.url,
                dest.display(),
                source.sha256
            )
        })?;
    }
    Ok(std::fs::read(&dest)?)
}

/// The stamp of a fallback made by this relift from these sources.
fn stamp() -> String {
    let mut s = format!("relift-switch-fonts {}\n", env!("CARGO_PKG_VERSION"));
    for src in relift_switch_fonts::SOURCES {
        s.push_str(src.sha256);
        s.push('\n');
    }
    s
}

/// Makes the fallback of each type in `types` into `out`, unless the one
/// there was made by this relift from the same sources; returns the types
/// made or kept.
///
/// # Errors
///
/// A source cannot be fetched or read, or a font cannot be made or written.
pub fn fallback(
    cache_root: &Path,
    out: &Path,
    types: &[FontType],
) -> Result<Vec<FontType>, BoxError> {
    let stamp_file = out.join("fallback.stamp");
    let wanted: Vec<(FontType, Source, Charset)> = types
        .iter()
        .filter_map(|&t| fallback_of(t).map(|(s, c)| (t, s, c)))
        .collect();
    let path = |t: FontType| out.join(format!("{}.ttf", t.index()));
    let fresh = std::fs::read_to_string(&stamp_file).is_ok_and(|s| s == stamp());
    if wanted.is_empty() || fresh && wanted.iter().all(|(t, _, _)| path(*t).is_file()) {
        return Ok(wanted.iter().map(|w| w.0).collect());
    }
    let table = String::from_utf8(fetch(cache_root, &relift_switch_fonts::JIS_X_0213)?)?;
    std::fs::create_dir_all(out)?;
    if !fresh {
        let _ = std::fs::remove_file(&stamp_file);
    }
    let mut made = Vec::new();
    for (t, source, charset) in wanted {
        let font = relift_switch_fonts::instance(
            &fetch(cache_root, &source)?,
            &charset.code_points(&table),
            relift_switch_fonts::WEIGHT,
        )
        .map_err(|e| format!("the {} fallback font from {}: {e}", t.name(), source.name))?;
        relift_cache::write_if_changed(&path(t), &font)?;
        made.push(t);
    }
    relift_cache::write_if_changed(&stamp_file, stamp().as_bytes())?;
    Ok(made)
}

/// Points a run of the program built in `out` at the fallback fonts made
/// beside it.
pub fn set_env(cmd: &mut std::process::Command, out: &Path) {
    let d = out.join(DIR);
    if d.is_dir() {
        cmd.env(FALLBACK_ENV, d);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("relift-fonts-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).expect("mkdir");
        d
    }

    #[test]
    fn decodes_each_font_from_its_title_folder() {
        let tmp = scratch("decode");
        let sd = tmp.join("sd");
        let std_title = sd.join("0100000000000811");
        std::fs::create_dir_all(&std_title).unwrap();
        let font = b"\x00\x01\x00\x00 a standard font".to_vec();
        let image = relift_switch_formats::romfs::build(&[(
            "nintendo_udsg-r_std_003.bfttf",
            &bfttf::encode(&font, 0x1234_5678),
        )]);
        relift_cache::write_if_changed(&std_title.join(romfs_image::IMAGE_NAME), &image).unwrap();
        let ext = sd.join("0100000000000810").join("romfs");
        std::fs::create_dir_all(&ext).unwrap();
        relift_cache::write_if_changed(
            &ext.join("nintendo_ext_003.bfttf"),
            &bfttf::encode(b"ext", 7),
        )
        .unwrap();

        let out = tmp.join("import").join(DIR);
        let (found, missing) = import(&sd, &out).unwrap();
        assert_eq!(found, vec![FontType::Standard, FontType::NintendoExt]);
        assert_eq!(missing.len(), 4);
        assert_eq!(std::fs::read(out.join("0.ttf")).unwrap(), font);
        assert_eq!(std::fs::read(out.join("5.ttf")).unwrap(), b"ext");
        assert_eq!(dir(&tmp.join("import")), Some(out));
        std::fs::remove_dir_all(&tmp).expect("cleanup");
    }

    #[test]
    fn every_type_but_the_extension_has_a_fallback() {
        for t in FontType::ALL {
            assert_eq!(
                fallback_of(t).is_some(),
                t != FontType::NintendoExt,
                "{}",
                t.name()
            );
        }
        let p = source_path(Path::new("/c"), &relift_switch_fonts::NOTO_KR);
        assert_eq!(
            p,
            Path::new("/c/sdk/fonts")
                .join(relift_switch_fonts::NOTO_TAG)
                .join("NotoSansKR-VF.ttf")
        );
    }

    #[test]
    fn a_fallback_made_from_the_same_sources_is_kept_without_fetching() {
        let tmp = scratch("kept");
        let out = tmp.join("fonts");
        assert!(
            fallback(&tmp, &out, &[FontType::NintendoExt])
                .unwrap()
                .is_empty()
        );
        std::fs::create_dir_all(&out).unwrap();
        relift_cache::write_if_changed(&out.join("4.ttf"), b"made before").unwrap();
        relift_cache::write_if_changed(&out.join("fallback.stamp"), stamp().as_bytes()).unwrap();
        assert_eq!(
            fallback(&tmp, &out, &[FontType::Korean, FontType::NintendoExt]).unwrap(),
            vec![FontType::Korean]
        );
        assert_eq!(std::fs::read(out.join("4.ttf")).unwrap(), b"made before");
        assert!(!tmp.join("sdk").exists());
        std::fs::remove_dir_all(&tmp).expect("cleanup");
    }

    #[test]
    fn refuses_a_missing_folder() {
        let tmp = scratch("missing");
        assert!(import(&tmp.join("none"), &tmp.join("out")).is_err());
        std::fs::remove_dir_all(&tmp).expect("cleanup");
    }
}
