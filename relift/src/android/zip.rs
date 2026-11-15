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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_crc_matches_the_standard_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
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
