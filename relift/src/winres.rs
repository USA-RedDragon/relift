//! The Windows resources of a game's executable, as a `.res` file the
//! linker takes: the game's icon (file explorer, taskbar) and version
//! information that names the game. Written here, so the build needs no
//! resource compiler.

use relift_info::Icon;

const RT_ICON: u16 = 3;
const RT_GROUP_ICON: u16 = 14;
const RT_VERSION: u16 = 16;
const LANG_EN_US: u16 = 0x0409;

fn pad4(v: &mut Vec<u8>) {
    while !v.len().is_multiple_of(4) {
        v.push(0);
    }
}

fn entry(out: &mut Vec<u8>, kind: u16, id: u16, data: &[u8]) {
    let mut head = Vec::new();
    head.extend([0xff, 0xff]);
    head.extend(kind.to_le_bytes());
    head.extend([0xff, 0xff]);
    head.extend(id.to_le_bytes());
    head.extend(0u32.to_le_bytes());
    head.extend(0x0030u16.to_le_bytes());
    head.extend(LANG_EN_US.to_le_bytes());
    head.extend(0u32.to_le_bytes());
    head.extend(0u32.to_le_bytes());
    out.extend((data.len() as u32).to_le_bytes());
    out.extend((head.len() as u32 + 8).to_le_bytes());
    out.extend(head);
    out.extend(data);
    pad4(out);
}

fn wide(s: &str) -> Vec<u8> {
    s.encode_utf16()
        .chain([0])
        .flat_map(u16::to_le_bytes)
        .collect()
}

fn block(key: &str, value_len: u16, kind: u16, value: &[u8], children: &[Vec<u8>]) -> Vec<u8> {
    let mut b = vec![0, 0];
    b.extend(value_len.to_le_bytes());
    b.extend(kind.to_le_bytes());
    b.extend(wide(key));
    pad4(&mut b);
    b.extend(value);
    for c in children {
        pad4(&mut b);
        b.extend(c);
    }
    let len = b.len() as u16;
    b[..2].copy_from_slice(&len.to_le_bytes());
    b
}

fn version_info(strings: &[(&str, &str)]) -> Vec<u8> {
    let mut fixed = Vec::new();
    for v in [
        0xFEEF_04BDu32,
        0x0001_0000,
        0,
        0,
        0,
        0,
        0x3f,
        0,
        0x0004_0004,
        1,
        0,
        0,
        0,
    ] {
        fixed.extend(v.to_le_bytes());
    }
    let items: Vec<Vec<u8>> = strings
        .iter()
        .map(|(k, v)| {
            let value = wide(v);
            block(k, (value.len() / 2) as u16, 1, &value, &[])
        })
        .collect();
    let table = block("040904B0", 0, 1, &[], &items);
    let string_info = block("StringFileInfo", 0, 1, &[], &[table]);
    let translation = block("Translation", 4, 0, &[0x09, 0x04, 0xB0, 0x04], &[]);
    let var_info = block("VarFileInfo", 0, 1, &[], &[translation]);
    block(
        "VS_VERSION_INFO",
        fixed.len() as u16,
        0,
        &fixed,
        &[string_info, var_info],
    )
}

/// The `.res` file for `title` and the game's own icon (none without one:
/// the executable keeps the system's icon).
#[must_use]
pub fn res(title: &str, icon: Option<&Icon>) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend(0u32.to_le_bytes());
    out.extend(32u32.to_le_bytes());
    out.extend([0xff, 0xff, 0, 0, 0xff, 0xff, 0, 0]);
    out.extend([0u8; 16]);
    if let Some(ico) = crate::icon::ico(icon) {
        let count = usize::from(u16::from_le_bytes([ico[4], ico[5]]));
        let mut group = vec![0, 0, 1, 0];
        group.extend((count as u16).to_le_bytes());
        for i in 0..count {
            let e = &ico[6 + 16 * i..22 + 16 * i];
            let len = u32::from_le_bytes([e[8], e[9], e[10], e[11]]) as usize;
            let at = u32::from_le_bytes([e[12], e[13], e[14], e[15]]) as usize;
            let id = (i + 1) as u16;
            entry(&mut out, RT_ICON, id, &ico[at..at + len]);
            group.extend(&e[..12]);
            group.extend(id.to_le_bytes());
        }
        entry(&mut out, RT_GROUP_ICON, 1, &group);
    }
    entry(
        &mut out,
        RT_VERSION,
        1,
        &version_info(&[("FileDescription", title), ("ProductName", title)]),
    );
    out
}

/// The file extension of a resource file in a program's source list: the
/// linker takes it, the compiler does not.
pub const EXT: &str = "res";

/// Writes `app.res` into `dir` when the build runs on Windows, and returns
/// its path for the program's file list; `None` on other hosts.
///
/// # Errors
///
/// The file cannot be written.
pub fn write(
    dir: &std::path::Path,
    title: &str,
    icon: Option<&Icon>,
) -> std::io::Result<Option<std::path::PathBuf>> {
    if !cfg!(windows) {
        return Ok(None);
    }
    let path = dir.join("app.res");
    relift_cache::write_if_changed(&path, &res(title, icon))?;
    Ok(Some(path))
}

/// Splits a program's file list into sources and linker resources.
#[must_use]
pub fn split(files: &[std::path::PathBuf]) -> (Vec<std::path::PathBuf>, Vec<String>) {
    let is_res = |p: &&std::path::PathBuf| p.extension().is_some_and(|e| e == EXT);
    (
        files.iter().filter(|p| !is_res(p)).cloned().collect(),
        files
            .iter()
            .filter(is_res)
            .map(|p| p.display().to_string())
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entries(res: &[u8]) -> Vec<(u16, u16, usize)> {
        let mut at = 0;
        let mut out = Vec::new();
        while at < res.len() {
            let size = u32::from_le_bytes(res[at..at + 4].try_into().unwrap()) as usize;
            let head = u32::from_le_bytes(res[at + 4..at + 8].try_into().unwrap()) as usize;
            let kind = u16::from_le_bytes(res[at + 10..at + 12].try_into().unwrap());
            let id = u16::from_le_bytes(res[at + 14..at + 16].try_into().unwrap());
            out.push((kind, id, size));
            at += (head + size).div_ceil(4) * 4;
        }
        out
    }

    #[test]
    fn a_game_without_icon_has_only_version_information_naming_it() {
        let r = res("My Game", None);
        let e = entries(&r);
        assert_eq!(e.len(), 2);
        assert_eq!((e[1].0, e[1].1), (RT_VERSION, 1));
        let name: Vec<u8> = wide("My Game");
        assert!(r.windows(name.len()).any(|w| w == name));
        let key = wide("VS_VERSION_INFO");
        assert!(r.windows(key.len()).any(|w| w == key));
    }

    #[test]
    fn the_icon_becomes_one_image_per_size_and_a_group() {
        let icon = crate::icon::from_image(include_bytes!("../tests/icon.jpg")).unwrap();
        let e = entries(&res("G", Some(&icon)));
        let kinds: Vec<u16> = e.iter().map(|x| x.0).collect();
        assert_eq!(
            kinds,
            [
                0,
                RT_ICON,
                RT_ICON,
                RT_ICON,
                RT_ICON,
                RT_GROUP_ICON,
                RT_VERSION
            ]
        );
        let group = e.iter().find(|x| x.0 == RT_GROUP_ICON).unwrap();
        assert_eq!(group.2, 6 + 14 * 4);
    }

    #[test]
    fn resources_split_from_sources() {
        use std::path::PathBuf;
        let files = [PathBuf::from("a.c"), PathBuf::from("d/app.res")];
        let (src, res) = split(&files);
        assert_eq!(src, [PathBuf::from("a.c")]);
        assert_eq!(res, ["d/app.res"]);
    }

    #[test]
    fn every_block_length_covers_its_children() {
        let v = version_info(&[("ProductName", "x")]);
        assert_eq!(usize::from(u16::from_le_bytes([v[0], v[1]])), v.len());
    }
}
