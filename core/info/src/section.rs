//! Reads the document out of a game binary without running it: an ELF
//! section, a Mach-O section or a PE section, all little endian and 64-bit.

use crate::embed::{ELF_SECTION, MACHO_SECTION, PE_SECTION};

/// Why no document was found.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// The file is not an ELF, Mach-O or PE image this reader knows.
    UnknownFormat,
    /// The image is cut short or its tables point outside it.
    Malformed(&'static str),
    /// The image has no relift section.
    NoSection,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::UnknownFormat => f.write_str("not an ELF, Mach-O or PE image"),
            Error::Malformed(w) => write!(f, "a malformed image: {w}"),
            Error::NoSection => f.write_str("no relift self-description in the image"),
        }
    }
}

impl std::error::Error for Error {}

fn le<const N: usize>(b: &[u8], at: usize) -> Option<[u8; N]> {
    b.get(at..at.checked_add(N)?)?.try_into().ok()
}

fn u16_at(b: &[u8], at: usize) -> Option<usize> {
    le(b, at).map(|x| usize::from(u16::from_le_bytes(x)))
}

fn u32_at(b: &[u8], at: usize) -> Option<usize> {
    le(b, at).and_then(|x| usize::try_from(u32::from_le_bytes(x)).ok())
}

fn u64_at(b: &[u8], at: usize) -> Option<usize> {
    le(b, at).and_then(|x| usize::try_from(u64::from_le_bytes(x)).ok())
}

fn name_at(b: &[u8], at: usize) -> Option<&[u8]> {
    let tail = b.get(at..)?;
    let end = tail.iter().position(|&c| c == 0)?;
    Some(&tail[..end])
}

fn fixed_name(b: &[u8], at: usize, n: usize) -> Option<&[u8]> {
    let f = b.get(at..at.checked_add(n)?)?;
    let end = f.iter().position(|&c| c == 0).unwrap_or(n);
    Some(&f[..end])
}

fn slice(b: &[u8], off: usize, len: usize) -> Result<&[u8], Error> {
    off.checked_add(len)
        .and_then(|e| b.get(off..e))
        .ok_or(Error::Malformed("a section outside the file"))
}

/// The document bytes of an image.
///
/// # Errors
///
/// [`Error::UnknownFormat`] for a file of another format,
/// [`Error::Malformed`] for a damaged image and [`Error::NoSection`] for an
/// image without a document.
pub fn read(image: &[u8]) -> Result<&[u8], Error> {
    if image.starts_with(b"\x7fELF") {
        elf(image)
    } else if image.starts_with(&[0xcf, 0xfa, 0xed, 0xfe]) {
        macho(image)
    } else if image.starts_with(b"MZ") {
        pe(image)
    } else {
        Err(Error::UnknownFormat)
    }
}

fn elf(b: &[u8]) -> Result<&[u8], Error> {
    let bad = Error::Malformed("an ELF header");
    if b.get(4) != Some(&2) || b.get(5) != Some(&1) {
        return Err(Error::UnknownFormat);
    }
    let shoff = u64_at(b, 0x28).ok_or(bad.clone())?;
    let shentsize = u16_at(b, 0x3a).ok_or(bad.clone())?;
    let shnum = u16_at(b, 0x3c).ok_or(bad.clone())?;
    let shstrndx = u16_at(b, 0x3e).ok_or(bad)?;
    let bad = Error::Malformed("an ELF section table");
    let header = |i: usize| shoff.checked_add(i.checked_mul(shentsize)?);
    let strtab = header(shstrndx).ok_or(bad.clone())?;
    let str_off = u64_at(b, strtab + 0x18).ok_or(bad.clone())?;
    for i in 0..shnum {
        let h = header(i).ok_or(bad.clone())?;
        let name = u32_at(b, h).ok_or(bad.clone())?;
        if name_at(b, str_off.saturating_add(name)) == Some(ELF_SECTION.as_bytes()) {
            let off = u64_at(b, h + 0x18).ok_or(bad.clone())?;
            let size = u64_at(b, h + 0x20).ok_or(bad)?;
            return slice(b, off, size);
        }
    }
    Err(Error::NoSection)
}

fn macho(b: &[u8]) -> Result<&[u8], Error> {
    let bad = Error::Malformed("a Mach-O load command");
    let ncmds = u32_at(b, 0x10).ok_or(bad.clone())?;
    let mut at = 0x20;
    for _ in 0..ncmds {
        let cmd = u32_at(b, at).ok_or(bad.clone())?;
        let size = u32_at(b, at + 4).ok_or(bad.clone())?;
        if cmd == 0x19 {
            let nsects = u32_at(b, at + 0x40).ok_or(bad.clone())?;
            for s in 0..nsects {
                let h = at + 0x48 + s * 0x50;
                if fixed_name(b, h, 16) == Some(MACHO_SECTION.as_bytes()) {
                    let len = u64_at(b, h + 0x28).ok_or(bad.clone())?;
                    let off = u32_at(b, h + 0x30).ok_or(bad)?;
                    return slice(b, off, len);
                }
            }
        }
        if size < 8 {
            return Err(bad);
        }
        at += size;
    }
    Err(Error::NoSection)
}

fn pe(b: &[u8]) -> Result<&[u8], Error> {
    let bad = Error::Malformed("a PE header");
    let pe = u32_at(b, 0x3c).ok_or(bad.clone())?;
    if b.get(pe..pe + 4) != Some(b"PE\0\0") {
        return Err(bad);
    }
    let nsects = u16_at(b, pe + 6).ok_or(bad.clone())?;
    let optional = u16_at(b, pe + 20).ok_or(bad.clone())?;
    let table = pe + 24 + optional;
    for s in 0..nsects {
        let h = table + s * 40;
        if fixed_name(b, h, 8) == Some(PE_SECTION.as_bytes()) {
            let virt = u32_at(b, h + 8).ok_or(bad.clone())?;
            let raw = u32_at(b, h + 16).ok_or(bad.clone())?;
            let off = u32_at(b, h + 20).ok_or(bad)?;
            return slice(b, off, virt.min(raw));
        }
    }
    Err(Error::NoSection)
}

#[cfg(test)]
mod tests {
    use super::*;

    const DOC: &[u8] = b"{\"relift\":{}}\n";

    fn put(v: &mut [u8], at: usize, x: &[u8]) {
        v[at..at + x.len()].copy_from_slice(x);
    }

    fn synthetic_elf() -> Vec<u8> {
        let mut v = vec![0u8; 0x400];
        put(&mut v, 0, b"\x7fELF\x02\x01\x01");
        put(&mut v, 0x28, &0x200u64.to_le_bytes());
        put(&mut v, 0x3a, &0x40u16.to_le_bytes());
        put(&mut v, 0x3c, &3u16.to_le_bytes());
        put(&mut v, 0x3e, &1u16.to_le_bytes());
        put(&mut v, 0x100, b"\0.shstrtab\0.relift_info\0");
        put(&mut v, 0x200 + 0x40 + 0x18, &0x100u64.to_le_bytes());
        put(&mut v, 0x200 + 0x80, &11u32.to_le_bytes());
        put(&mut v, 0x200 + 0x80 + 0x18, &0x300u64.to_le_bytes());
        put(
            &mut v,
            0x200 + 0x80 + 0x20,
            &(DOC.len() as u64).to_le_bytes(),
        );
        put(&mut v, 0x300, DOC);
        v
    }

    fn synthetic_macho() -> Vec<u8> {
        let mut v = vec![0u8; 0x400];
        put(&mut v, 0, &[0xcf, 0xfa, 0xed, 0xfe]);
        put(&mut v, 0x10, &2u32.to_le_bytes());
        put(&mut v, 0x20, &0x1bu32.to_le_bytes());
        put(&mut v, 0x24, &0x18u32.to_le_bytes());
        let seg = 0x38;
        put(&mut v, seg, &0x19u32.to_le_bytes());
        put(&mut v, seg + 4, &(0x48u32 + 0x50).to_le_bytes());
        put(&mut v, seg + 0x40, &1u32.to_le_bytes());
        let s = seg + 0x48;
        put(&mut v, s, b"__relift_info");
        put(&mut v, s + 0x10, b"__DATA");
        put(&mut v, s + 0x28, &(DOC.len() as u64).to_le_bytes());
        put(&mut v, s + 0x30, &0x300u32.to_le_bytes());
        put(&mut v, 0x300, DOC);
        v
    }

    fn synthetic_pe() -> Vec<u8> {
        let mut v = vec![0u8; 0x400];
        put(&mut v, 0, b"MZ");
        put(&mut v, 0x3c, &0x80u32.to_le_bytes());
        put(&mut v, 0x80, b"PE\0\0");
        put(&mut v, 0x80 + 6, &1u16.to_le_bytes());
        put(&mut v, 0x80 + 20, &0xf0u16.to_le_bytes());
        let h = 0x80 + 24 + 0xf0;
        put(&mut v, h, b".rlinfo");
        put(&mut v, h + 8, &(DOC.len() as u32).to_le_bytes());
        put(&mut v, h + 16, &0x200u32.to_le_bytes());
        put(&mut v, h + 20, &0x300u32.to_le_bytes());
        put(&mut v, 0x300, DOC);
        v
    }

    #[test]
    fn the_document_is_found_in_each_format() {
        for image in [synthetic_elf(), synthetic_macho(), synthetic_pe()] {
            assert_eq!(read(&image), Ok(DOC));
        }
    }

    #[test]
    fn other_files_and_cut_images_are_refused() {
        assert_eq!(read(b"#!/bin/sh\n"), Err(Error::UnknownFormat));
        let mut elf = synthetic_elf();
        elf.truncate(0x2f0);
        assert!(matches!(read(&elf), Err(Error::Malformed(_))));
        let mut elf = synthetic_elf();
        put(&mut elf, 0x100, b"\0.shstrtab\0.other_info\0");
        assert_eq!(read(&elf), Err(Error::NoSection));
    }
}
