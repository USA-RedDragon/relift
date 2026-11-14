//! IPSwitch `.pchtxt` text patches.
//!
//! A `.pchtxt` names its module with `@nsobid-<build id>` and lists
//! `OFFSET VALUE` lines: a hex offset and either hex bytes, written in the
//! order given, or a quoted string. `@enabled` and `@disabled` turn the
//! lines that follow on and off, `@stop` ends the file, and
//! `@flag offset_shift N` adds `N` to every offset. Lines starting with
//! `//`, `#` or `[` are comments or titles. `@little-endian` keeps hex
//! values in the written order; after `@big-endian` each value's bytes are
//! reversed. Offsets, once shifted, count like IPS32 offsets: from the
//! start of the NSO header, so image offset 0 is offset 0x100.

use std::fmt;

/// One write: the offset (IPS32 convention) and the bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Record {
    /// The offset, from the start of the NSO header.
    pub offset: u64,
    /// The bytes written there.
    pub bytes: Vec<u8>,
}

/// A parsed `.pchtxt`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Pchtxt {
    /// The build id from `@nsobid-`, lowercase hex as written.
    pub build_id: String,
    /// The enabled writes, in file order.
    pub records: Vec<Record>,
}

/// Why a `.pchtxt` did not parse.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    /// The 1-based line.
    pub line: usize,
    /// What is wrong.
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "line {}: {}", self.line, self.message)
    }
}

impl std::error::Error for Error {}

fn hex_u64(s: &str) -> Option<u64> {
    let s = s
        .strip_prefix("0x")
        .or_else(|| s.strip_prefix("0X"))
        .unwrap_or(s);
    u64::from_str_radix(s, 16).ok()
}

fn hex_bytes(s: &str) -> Option<Vec<u8>> {
    if s.is_empty() || !s.len().is_multiple_of(2) {
        return None;
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok())
        .collect()
}

fn string_bytes(s: &str) -> Option<Vec<u8>> {
    let inner = s.strip_prefix('"')?.strip_suffix('"')?;
    let mut out = Vec::new();
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            let mut b = [0u8; 4];
            out.extend_from_slice(c.encode_utf8(&mut b).as_bytes());
            continue;
        }
        out.push(match chars.next()? {
            'n' => b'\n',
            'r' => b'\r',
            't' => b'\t',
            '0' => 0,
            '\\' => b'\\',
            '"' => b'"',
            _ => return None,
        });
    }
    Some(out)
}

/// Parses a `.pchtxt`.
///
/// # Errors
///
/// A line is neither a directive, a comment nor an `OFFSET VALUE` pair, or
/// the file names no build id.
pub fn parse(text: &str) -> Result<Pchtxt, Error> {
    let mut out = Pchtxt::default();
    let mut enabled = true;
    let mut shift: u64 = 0;
    let mut big = false;
    for (n, raw) in text.lines().enumerate() {
        let line = n + 1;
        let err = |m: &str| Error {
            line,
            message: m.to_string(),
        };
        let l = raw.trim();
        if l.is_empty() || l.starts_with("//") || l.starts_with('#') || l.starts_with('[') {
            continue;
        }
        if let Some(d) = l.strip_prefix('@') {
            let mut w = d.split_whitespace();
            match w.next().unwrap_or_default() {
                id if id.starts_with("nsobid-") => {
                    out.build_id = id["nsobid-".len()..].to_ascii_lowercase();
                }
                "enabled" => enabled = true,
                "disabled" => enabled = false,
                "stop" => break,
                "little-endian" => big = false,
                "big-endian" => big = true,
                "flag" => {
                    if w.next() == Some("offset_shift") {
                        shift = w
                            .next()
                            .and_then(hex_u64)
                            .ok_or_else(|| err("offset_shift needs a hex value"))?;
                    }
                }
                other => return Err(err(&format!("unknown directive @{other}"))),
            }
            continue;
        }
        let (off, value) = l
            .split_once(char::is_whitespace)
            .ok_or_else(|| err("expected OFFSET VALUE"))?;
        let value = value.trim();
        let value = if value.starts_with('"') {
            value
        } else {
            value.split_once("//").map_or(value, |(v, _)| v.trim_end())
        };
        let offset = hex_u64(off).ok_or_else(|| err("the offset is not hex"))?;
        let bytes = if value.starts_with('"') {
            string_bytes(value).ok_or_else(|| err("a bad string value"))?
        } else {
            let v = hex_bytes(value).ok_or_else(|| err("the value is not hex bytes"))?;
            if big {
                v.into_iter().rev().collect()
            } else {
                v
            }
        };
        if !enabled {
            continue;
        }
        out.records.push(Record {
            offset: offset
                .checked_add(shift)
                .ok_or_else(|| err("the offset overflows"))?,
            bytes,
        });
    }
    if out.build_id.is_empty() {
        return Err(Error {
            line: 0,
            message: "no @nsobid- line".into(),
        });
    }
    Ok(out)
}

/// Whether a patch file named for `name` (hex, as in
/// `exefs_patches/<mod>/<name>.ips`) is for the module with build id `id`:
/// the name is a prefix of the id's hex and the rest of the id is zero.
pub fn build_id_matches(name: &str, id: &[u8; 32]) -> bool {
    let full: String = id.iter().map(|b| format!("{b:02x}")).collect();
    let name = name.to_ascii_lowercase();
    !name.is_empty()
        && name.len() <= full.len()
        && name.chars().all(|c| c.is_ascii_hexdigit())
        && full.starts_with(&name)
        && full[name.len()..].chars().all(|c| c == '0')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_pchtxt_gives_its_enabled_records() {
        let t = "@nsobid-ABCDEF00\n\n# a title\n@flag offset_shift 0x100\n[60 fps]\n@enabled\n00001000 1F2003D5 // nop\n00002000 \"hi\\0\"\n@disabled\n00003000 00\n@enabled\n@big-endian\n00004000 D503201F\n@stop\n00005000 zz\n";
        let p = parse(t).expect("parses");
        assert_eq!(p.build_id, "abcdef00");
        assert_eq!(
            p.records,
            [
                Record {
                    offset: 0x1100,
                    bytes: vec![0x1f, 0x20, 0x03, 0xd5]
                },
                Record {
                    offset: 0x2100,
                    bytes: b"hi\0".to_vec()
                },
                Record {
                    offset: 0x4100,
                    bytes: vec![0x1f, 0x20, 0x03, 0xd5]
                },
            ]
        );
    }

    #[test]
    fn bad_lines_are_errors_with_their_line() {
        let e = parse("@nsobid-00\n1000 xyz\n").expect_err("bad value");
        assert_eq!(e.line, 2);
        assert!(parse("1000 00\n").is_err());
        assert!(parse("@nsobid-00\n@what\n").is_err());
    }

    #[test]
    fn build_ids_match_by_prefix_with_zero_padding() {
        let mut id = [0u8; 32];
        id[..4].copy_from_slice(&[0xab, 0xcd, 0x01, 0x02]);
        assert!(build_id_matches("ABCD0102", &id));
        assert!(build_id_matches("abcd010200000000", &id));
        assert!(!build_id_matches("abcd01", &[0xff; 32]));
        assert!(!build_id_matches("abcd0103", &id));
        assert!(!build_id_matches("", &id));
    }
}
