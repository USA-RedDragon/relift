//! The console's shared system fonts from the player's own system data.
//!
//! `relift import --system-data <dir>` takes a folder of decrypted font
//! system data titles as switch-tools exports them from the player's
//! console: one folder per title id (`0100000000000810` to
//! `0100000000000814`) holding the title's romfs as `romfs.img`, a romfs map
//! or an extracted `romfs/` tree. Each `.bfttf` is decoded into
//! `fonts/<type>.ttf` in the import, `<type>` the `SharedFontType`, which the
//! runtime's pl:u serves (`RELIFT_FONTS`). relift ships no font data.

use std::path::{Path, PathBuf};

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
    fn refuses_a_missing_folder() {
        let tmp = scratch("missing");
        assert!(import(&tmp.join("none"), &tmp.join("out")).is_err());
        std::fs::remove_dir_all(&tmp).expect("cleanup");
    }
}
