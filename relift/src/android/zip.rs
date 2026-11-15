//! A zip archive of stored (uncompressed) entries, which is all an APK of
//! relift needs: `zipalign` then page-aligns the native libraries.

const LOCAL: u32 = 0x0403_4b50;
const CENTRAL: u32 = 0x0201_4b50;
const END: u32 = 0x0605_4b50;
const VERSION: u16 = 20;
const DOS_TIME: u16 = 0;
const DOS_DATE: u16 = 0x0021;

fn crc32(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (i, slot) in table.iter_mut().enumerate() {
        let mut c = u32::try_from(i).unwrap_or(0);
        for _ in 0..8 {
            c = if c & 1 != 0 {
                0xEDB8_8320 ^ (c >> 1)
            } else {
                c >> 1
            };
        }
        *slot = c;
    }
    !data.iter().fold(!0u32, |c, &b| {
        table[usize::from(u8::try_from((c ^ u32::from(b)) & 0xFF).unwrap_or(0))] ^ (c >> 8)
    })
}

/// Entries to write, in order.
#[derive(Default)]
pub struct Zip {
    entries: Vec<(String, Vec<u8>)>,
}

impl Zip {
    /// Adds a file named `name` (forward slashes) holding `data`.
    pub fn add(&mut self, name: &str, data: Vec<u8>) {
        self.entries.push((name.to_string(), data));
    }

    /// The archive's bytes.
    ///
    /// # Errors
    ///
    /// An entry or the archive is larger than a zip without the 64-bit
    /// extension holds.
    pub fn finish(&self) -> Result<Vec<u8>, String> {
        let too_big = |what: &str| format!("{what} is too large for a zip archive");
        let mut out = Vec::new();
        let mut central = Vec::new();
        for (name, data) in &self.entries {
            let offset = u32::try_from(out.len()).map_err(|_| too_big("the archive"))?;
            let size = u32::try_from(data.len()).map_err(|_| too_big(name))?;
            let name_len = u16::try_from(name.len()).map_err(|_| too_big(name))?;
            let crc = crc32(data);
            let common = |v: &mut Vec<u8>| {
                v.extend_from_slice(&VERSION.to_le_bytes());
                v.extend_from_slice(&0u16.to_le_bytes());
                v.extend_from_slice(&0u16.to_le_bytes());
                v.extend_from_slice(&DOS_TIME.to_le_bytes());
                v.extend_from_slice(&DOS_DATE.to_le_bytes());
                v.extend_from_slice(&crc.to_le_bytes());
                v.extend_from_slice(&size.to_le_bytes());
                v.extend_from_slice(&size.to_le_bytes());
                v.extend_from_slice(&name_len.to_le_bytes());
                v.extend_from_slice(&0u16.to_le_bytes());
            };
            out.extend_from_slice(&LOCAL.to_le_bytes());
            common(&mut out);
            out.extend_from_slice(name.as_bytes());
            out.extend_from_slice(data);
            central.extend_from_slice(&CENTRAL.to_le_bytes());
            central.extend_from_slice(&VERSION.to_le_bytes());
            common(&mut central);
            central.extend_from_slice(&[0; 2 + 2 + 2 + 4]);
            central.extend_from_slice(&offset.to_le_bytes());
            central.extend_from_slice(name.as_bytes());
        }
        let start = u32::try_from(out.len()).map_err(|_| too_big("the archive"))?;
        let central_len = u32::try_from(central.len()).map_err(|_| too_big("the directory"))?;
        let count = u16::try_from(self.entries.len()).map_err(|_| too_big("the entry list"))?;
        out.extend_from_slice(&central);
        out.extend_from_slice(&END.to_le_bytes());
        out.extend_from_slice(&[0; 4]);
        out.extend_from_slice(&count.to_le_bytes());
        out.extend_from_slice(&count.to_le_bytes());
        out.extend_from_slice(&central_len.to_le_bytes());
        out.extend_from_slice(&start.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        Ok(out)
    }
}

/// One entry of an existing archive.
pub struct Entry<'a> {
    pub name: String,
    /// Zip compression method; 0 is stored.
    pub method: u16,
    /// Where the entry's data starts in the archive.
    pub offset: usize,
    pub data: &'a [u8],
}

fn le16(b: &[u8], at: usize) -> Option<usize> {
    Some(usize::from(u16::from_le_bytes(
        b.get(at..at + 2)?.try_into().ok()?,
    )))
}

fn le32(b: &[u8], at: usize) -> Option<usize> {
    usize::try_from(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?)).ok()
}

/// The entries of the archive `bytes`, read through its central directory.
///
/// # Errors
///
/// The bytes are not a zip archive this reader understands.
pub fn entries(bytes: &[u8]) -> Result<Vec<Entry<'_>>, String> {
    let bad = || "not a readable zip archive".to_string();
    let end = bytes
        .windows(4)
        .rposition(|w| w == END.to_le_bytes())
        .ok_or_else(bad)?;
    let count = le16(bytes, end + 10).ok_or_else(bad)?;
    let mut at = le32(bytes, end + 16).ok_or_else(bad)?;
    let mut out = Vec::new();
    for _ in 0..count {
        if le32(bytes, at).ok_or_else(bad)? != usize::try_from(CENTRAL).unwrap_or(0) {
            return Err(bad());
        }
        let method = u16::try_from(le16(bytes, at + 10).ok_or_else(bad)?).map_err(|_| bad())?;
        let size = le32(bytes, at + 20).ok_or_else(bad)?;
        let name_len = le16(bytes, at + 28).ok_or_else(bad)?;
        let extra_len = le16(bytes, at + 30).ok_or_else(bad)?;
        let comment_len = le16(bytes, at + 32).ok_or_else(bad)?;
        let local = le32(bytes, at + 42).ok_or_else(bad)?;
        let name = String::from_utf8_lossy(bytes.get(at + 46..at + 46 + name_len).ok_or_else(bad)?)
            .into_owned();
        let data_at = local
            + 30
            + le16(bytes, local + 26).ok_or_else(bad)?
            + le16(bytes, local + 28).ok_or_else(bad)?;
        let data = bytes.get(data_at..data_at + size).ok_or_else(bad)?;
        out.push(Entry {
            name,
            method,
            offset: data_at,
            data,
        });
        at += 46 + name_len + extra_len + comment_len;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_crc_matches_the_standard_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn entries_come_back_with_their_data_offsets() {
        let mut z = Zip::default();
        z.add("a", b"xyz".to_vec());
        z.add("b/c", b"hello".to_vec());
        let bytes = z.finish().unwrap();
        let e = entries(&bytes).unwrap();
        assert_eq!(e.len(), 2);
        assert_eq!(e[1].name, "b/c");
        assert_eq!(e[1].method, 0);
        assert_eq!(e[1].data, b"hello");
        assert_eq!(&bytes[e[1].offset..e[1].offset + 5], b"hello");
    }

    #[test]
    fn an_archive_lists_its_entries_and_ends_with_the_directory() {
        let mut z = Zip::default();
        z.add("a/b.txt", b"hello".to_vec());
        z.add("c", Vec::new());
        let bytes = z.finish().unwrap();
        assert_eq!(&bytes[..4], &LOCAL.to_le_bytes());
        let end = bytes.len() - 22;
        assert_eq!(&bytes[end..end + 4], &END.to_le_bytes());
        assert_eq!(&bytes[end + 10..end + 12], &2u16.to_le_bytes());
    }
}
