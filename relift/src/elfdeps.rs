//! What a 64-bit little-endian ELF file asks of the system it runs on: the
//! shared libraries it needs, where it looks for them, and the newest glibc
//! symbol version it binds. The AppImage step bundles the libraries and
//! reports the glibc a machine must have.

/// The dynamic section facts of one ELF file.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Deps {
    /// `DT_NEEDED`, in order.
    pub needed: Vec<String>,
    /// `DT_RUNPATH` or `DT_RPATH`, split at `:`.
    pub search: Vec<String>,
    /// The newest `GLIBC_x.y` version the file binds, as (major, minor).
    pub glibc: Option<(u32, u32)>,
}

/// Why a file is not read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// Not a 64-bit little-endian ELF file.
    NotElf,
    /// A table points outside the file.
    Malformed(&'static str),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::NotElf => f.write_str("not a 64-bit little-endian ELF file"),
            Error::Malformed(w) => write!(f, "a malformed ELF file: {w}"),
        }
    }
}

impl std::error::Error for Error {}

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const DT_NEEDED: u64 = 1;
const DT_STRTAB: u64 = 5;
const DT_RPATH: u64 = 15;
const DT_RUNPATH: u64 = 29;
const DT_VERNEED: u64 = 0x6fff_fffe;
const DT_VERNEEDNUM: u64 = 0x6fff_ffff;

fn arr<const N: usize>(b: &[u8], at: usize) -> Option<[u8; N]> {
    b.get(at..at.checked_add(N)?)?.try_into().ok()
}

fn u16_at(b: &[u8], at: usize) -> Option<u16> {
    arr(b, at).map(u16::from_le_bytes)
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    arr(b, at).map(u32::from_le_bytes)
}

fn u64_at(b: &[u8], at: usize) -> Option<u64> {
    arr(b, at).map(u64::from_le_bytes)
}

fn cstr(b: &[u8], at: usize) -> Option<&str> {
    let tail = b.get(at..)?;
    let end = tail.iter().position(|&c| c == 0)?;
    std::str::from_utf8(&tail[..end]).ok()
}

struct Load {
    vaddr: u64,
    offset: u64,
    filesz: u64,
}

fn file_offset(loads: &[Load], vaddr: u64) -> Option<usize> {
    loads
        .iter()
        .find(|l| vaddr >= l.vaddr && vaddr - l.vaddr < l.filesz)
        .and_then(|l| usize::try_from(l.offset + (vaddr - l.vaddr)).ok())
}

fn glibc_version(name: &str) -> Option<(u32, u32)> {
    let v = name.strip_prefix("GLIBC_")?;
    let mut it = v.split('.');
    let major = it.next()?.parse().ok()?;
    let minor = it.next().map_or(Some(0), |m| m.parse().ok())?;
    Some((major, minor))
}

/// Reads the dependencies of the ELF file `b`.
///
/// # Errors
///
/// [`Error::NotElf`] for another format, [`Error::Malformed`] when a table
/// points outside the file. A static file has no dynamic section and gives
/// an empty [`Deps`].
pub fn read(b: &[u8]) -> Result<Deps, Error> {
    if !b.starts_with(b"\x7fELF") || b.get(4) != Some(&2) || b.get(5) != Some(&1) {
        return Err(Error::NotElf);
    }
    let bad = |w| Error::Malformed(w);
    let phoff =
        usize::try_from(u64_at(b, 0x20).ok_or(bad("header"))?).map_err(|_| bad("header"))?;
    let phentsize = usize::from(u16_at(b, 0x36).ok_or(bad("header"))?);
    let phnum = usize::from(u16_at(b, 0x38).ok_or(bad("header"))?);
    let mut loads = Vec::new();
    let mut dynamic = None;
    for i in 0..phnum {
        let h = phoff + i * phentsize;
        let kind = u32_at(b, h).ok_or(bad("program header"))?;
        let field = |o| u64_at(b, h + o).ok_or(bad("program header"));
        match kind {
            PT_LOAD => loads.push(Load {
                offset: field(0x08)?,
                vaddr: field(0x10)?,
                filesz: field(0x20)?,
            }),
            PT_DYNAMIC => dynamic = Some((field(0x08)?, field(0x20)?)),
            _ => {}
        }
    }
    let Some((dyn_off, dyn_size)) = dynamic else {
        return Ok(Deps::default());
    };
    let dyn_off = usize::try_from(dyn_off).map_err(|_| bad("dynamic"))?;
    let dyn_end = dyn_off + usize::try_from(dyn_size).map_err(|_| bad("dynamic"))?;
    let (mut strtab, mut verneed, mut verneednum) = (0, 0, 0);
    let (mut needed, mut run, mut rpath) = (Vec::new(), None, None);
    let mut at = dyn_off;
    while at + 16 <= dyn_end {
        let tag = u64_at(b, at).ok_or(bad("dynamic"))?;
        let val = u64_at(b, at + 8).ok_or(bad("dynamic"))?;
        match tag {
            0 => break,
            DT_NEEDED => needed.push(val),
            DT_STRTAB => strtab = val,
            DT_RUNPATH => run = Some(val),
            DT_RPATH => rpath = Some(val),
            DT_VERNEED => verneed = val,
            DT_VERNEEDNUM => verneednum = val,
            _ => {}
        }
        at += 16;
    }
    let str_off = file_offset(&loads, strtab).ok_or(bad("string table"))?;
    let name = |o: u64| {
        usize::try_from(o)
            .ok()
            .and_then(|o| cstr(b, str_off + o))
            .ok_or(bad("string"))
    };
    let mut deps = Deps::default();
    for n in needed {
        deps.needed.push(name(n)?.to_string());
    }
    if let Some(p) = run.or(rpath) {
        deps.search = name(p)?
            .split(':')
            .filter(|s| !s.is_empty())
            .map(String::from)
            .collect();
    }
    if verneed != 0 {
        let mut entry = file_offset(&loads, verneed).ok_or(bad("version table"))?;
        for _ in 0..verneednum {
            let cnt = usize::from(u16_at(b, entry + 2).ok_or(bad("version table"))?);
            let aux = u32_at(b, entry + 8).ok_or(bad("version table"))? as usize;
            let next = u32_at(b, entry + 12).ok_or(bad("version table"))? as usize;
            let mut a = entry + aux;
            for _ in 0..cnt {
                let n = u32_at(b, a + 8).ok_or(bad("version table"))?;
                if let Some(v) = glibc_version(name(u64::from(n))?) {
                    deps.glibc = deps.glibc.max(Some(v));
                }
                let step = u32_at(b, a + 12).ok_or(bad("version table"))? as usize;
                if step == 0 {
                    break;
                }
                a += step;
            }
            if next == 0 {
                break;
            }
            entry += next;
        }
    }
    Ok(deps)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(target_os = "linux")]
    fn exe() -> Vec<u8> {
        std::fs::read(std::env::current_exe().unwrap()).unwrap()
    }

    #[test]
    fn a_glibc_symbol_version_name_parses() {
        assert_eq!(glibc_version("GLIBC_2.34"), Some((2, 34)));
        assert_eq!(glibc_version("GLIBC_2.2.5"), Some((2, 2)));
        assert_eq!(glibc_version("GLIBC_PRIVATE"), None);
        assert_eq!(glibc_version("GCC_3.0"), None);
    }

    #[test]
    fn something_that_is_not_an_elf_is_refused() {
        assert_eq!(read(b"MZ\0\0"), Err(Error::NotElf));
        assert_eq!(read(b""), Err(Error::NotElf));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_cut_header_is_malformed_not_a_panic() {
        let b = exe();
        for n in [8, 0x30, 0x40, 0x100] {
            let _ = read(&b[..n]);
        }
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn the_test_binary_needs_libc_and_a_glibc_version() {
        if !cfg!(target_env = "gnu") {
            return;
        }
        let d = read(&exe()).unwrap();
        assert!(d.needed.iter().any(|n| n.starts_with("libc.so")), "{d:?}");
        assert!(d.glibc.is_some_and(|(m, _)| m == 2), "{d:?}");
    }
}
