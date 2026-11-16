//! SARC archives: the file table and names, either byte order.

use crate::error::{Result, malformed};

fn u16_at(b: &[u8], at: usize, le: bool) -> Result<u16> {
    match b.get(at..at + 2) {
        Some(s) if le => Ok(u16::from_le_bytes([s[0], s[1]])),
        Some(s) => Ok(u16::from_be_bytes([s[0], s[1]])),
        None => malformed("SARC: a field runs past the end"),
    }
}

fn u32_at(b: &[u8], at: usize, le: bool) -> Result<u32> {
    match b.get(at..at + 4) {
        Some(s) if le => Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]])),
        Some(s) => Ok(u32::from_be_bytes([s[0], s[1], s[2], s[3]])),
        None => malformed("SARC: a field runs past the end"),
    }
}

/// Whether `b` starts like a SARC archive.
pub fn is_sarc(b: &[u8]) -> bool {
    b.starts_with(b"SARC")
}

/// The longest member name, terminator excluded.
pub const MAX_NAME: usize = 0x400;

/// The members of a SARC archive: name (the hash in hex when the archive
/// stores no name) and bytes, clamped to the archive. The name table is
/// indexed once, so the cost is linear in the archive.
///
/// # Errors
///
/// The tables run past the end of `b`, or a name is not terminated
/// inside the name table within [`MAX_NAME`] bytes.
pub fn members(b: &[u8]) -> Result<Vec<(String, &[u8])>> {
    let le = b.get(6..8) == Some(&[0xFF, 0xFE]);
    let header = usize::from(u16_at(b, 4, le)?);
    let data = u32_at(b, 0x0C, le)? as usize;
    let count = usize::from(u16_at(b, header + 6, le)?);
    let nodes = header + 12;
    let names = nodes + count * 16 + 8;
    let table = b.get(names..data.min(b.len())).unwrap_or(&[]);
    let ends: Vec<usize> = table
        .iter()
        .enumerate()
        .filter_map(|(i, &c)| (c == 0).then_some(i))
        .collect();
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let at = nodes + i * 16;
        let hash = u32_at(b, at, le)?;
        let attr = u32_at(b, at + 4, le)?;
        let start = u32_at(b, at + 8, le)? as usize;
        let end = u32_at(b, at + 12, le)? as usize;
        let name = if attr & 0x0100_0000 != 0 {
            let no = (attr & 0xFFFF) as usize * 4;
            let nul = ends.get(ends.partition_point(|&e| e < no)).copied();
            match nul {
                Some(nul) if nul - no <= MAX_NAME => {
                    String::from_utf8_lossy(&table[no..nul]).into_owned()
                }
                _ => return malformed("SARC: a name is not terminated in the name table"),
            }
        } else {
            format!("{hash:08x}")
        };
        let lo = (data + start).min(b.len());
        let hi = (data + end).min(b.len()).max(lo);
        out.push((name, &b[lo..hi]));
    }
    Ok(out)
}

/// Build a little-endian SARC archive of `files`, for tests.
pub fn build(files: &[(&str, &[u8])]) -> Vec<u8> {
    let mut names = Vec::new();
    let mut offs = Vec::new();
    for (n, _) in files {
        offs.push(names.len() / 4);
        names.extend_from_slice(n.as_bytes());
        names.push(0);
        while names.len() % 4 != 0 {
            names.push(0);
        }
    }
    let header = 0x14usize;
    let data_off = (header + 12 + files.len() * 16 + 8 + names.len()).div_ceil(0x100) * 0x100;
    let mut body = Vec::new();
    let mut nodes = Vec::new();
    for (i, (_, d)) in files.iter().enumerate() {
        while body.len() % 0x10 != 0 {
            body.push(0);
        }
        let s = body.len();
        body.extend_from_slice(d);
        nodes.extend_from_slice(&(i as u32 + 1).to_le_bytes());
        nodes.extend_from_slice(&(0x0100_0000u32 | offs[i] as u32).to_le_bytes());
        nodes.extend_from_slice(&(s as u32).to_le_bytes());
        nodes.extend_from_slice(&(body.len() as u32).to_le_bytes());
    }
    let mut out = Vec::new();
    out.extend_from_slice(b"SARC");
    out.extend_from_slice(&(header as u16).to_le_bytes());
    out.extend_from_slice(&[0xFF, 0xFE]);
    out.extend_from_slice(&((data_off + body.len()) as u32).to_le_bytes());
    out.extend_from_slice(&(data_off as u32).to_le_bytes());
    out.extend_from_slice(&0x100u32.to_le_bytes());
    out.extend_from_slice(b"SFAT");
    out.extend_from_slice(&12u16.to_le_bytes());
    out.extend_from_slice(&(files.len() as u16).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&nodes);
    out.extend_from_slice(b"SFNT");
    out.extend_from_slice(&8u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&names);
    out.resize(data_off, 0);
    out.extend_from_slice(&body);
    out
}
