//! The Switch 2 GPU programs of a build: the runtime records each program a
//! game sets that the build has no translation of, with the capabilities of
//! the device it ran on, and the next build translates every recorded program
//! for that device into the table the program embeds.
//!
//! The folder is `shaders/` in the build directory:
//!
//! * `device.json`, the runtime's dump of its Vulkan device (the shape
//!   `DeviceCaps::from_json` reads);
//! * `programs/<key>.bin`, each program the runtime met (its shader program
//!   header, if any, and its code), named by the hex SHA-256 of those bytes;
//! * `failed.txt`, written here: one line per program the translator
//!   refused, the key and the reason;
//! * `table.rlspv`, written here: the translations, each module a zstd frame
//!   (`Table::to_compressed_bytes` at [`ZSTD_LEVEL`]);
//! * `table.rlspi`, written here: what the runtime must know of each
//!   translation beyond its SPIR-V (see [`interfaces`]).
//!
//! The tables stay files the runtime reads a module at a time (one
//! program's translation runs to megabytes, a game's to gigabytes); the
//! build's `c/shaders.c` gives the runtime the folder's path before `main`.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_gpu_sm86::translate::build::{Input, Table, translate_set};
use relift_gpu_sm86::translate::{DeviceCaps, HostProfile, Options};

use crate::import::BoxError;

/// The zstd level of the translation table: near the ratio of the highest
/// levels on SPIR-V at a fraction of their time.
const ZSTD_LEVEL: i32 = 9;

/// The folder in the build directory.
pub const DIR: &str = "shaders";

/// The table's file in the folder.
const TABLE: &str = "table.rlspv";

/// The interface table's file in the folder.
const INTERFACES: &str = "table.rlspi";

/// An interface record: the specialization constant that picks the numeric
/// kind of the texture a bound handle names.
pub const RECORD_TEXTURE_KIND: u32 = 0;
/// An interface record: the specialization constant that gives the
/// dimension of the texture a bound handle names.
pub const RECORD_TEXTURE_DIM: u32 = 1;
/// An interface record: the program dereferences guest GPU addresses.
pub const RECORD_GLOBAL_MEMORY: u32 = 2;
/// An interface record: the program reads a bound texture handle.
pub const RECORD_TEXTURE: u32 = 3;

/// What a build's shader table holds.
pub struct Built {
    /// The C file that names the folder to the runtime.
    pub c_file: PathBuf,
    /// Programs recorded.
    pub programs: usize,
    /// Programs translated.
    pub translated: usize,
    /// The refusals by kind: the reason up to its first detail, and how many.
    pub refused: BTreeMap<String, usize>,
}

fn hex_key(name: &str) -> Option<[u8; 32]> {
    let s = name.strip_suffix(".bin")?;
    if s.len() != 64 {
        return None;
    }
    let mut k = [0u8; 32];
    for (i, b) in k.iter_mut().enumerate() {
        *b = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()?;
    }
    Some(k)
}

fn hex(k: &[u8; 32]) -> String {
    k.iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    })
}

/// The recorded programs of `dir`, sorted by key; a file whose bytes do not
/// hash to its name is left out.
fn programs(dir: &Path) -> Result<Vec<Input>, BoxError> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(dir.join("programs")) else {
        return Ok(out);
    };
    for e in rd {
        let e = e?;
        let Some(key) = e.file_name().to_str().and_then(hex_key) else {
            continue;
        };
        let code = std::fs::read(e.path())?;
        if relift_switch_formats::sha256(&code) != key {
            continue;
        }
        out.push(Input {
            key,
            code,
            options: Options::default(),
        });
    }
    out.sort_by_key(|i| i.key);
    Ok(out)
}

/// The kind of a refusal: its text with addresses and instruction words
/// (hex numbers) left out.
pub fn reason_kind(reason: &str) -> String {
    reason
        .split_whitespace()
        .map(|w| {
            let t = w.trim_end_matches([':', ',', ';']);
            let hex = t.strip_prefix("0x").is_some_and(|h| !h.is_empty())
                || (t.len() >= 8 && t.bytes().all(|b| b.is_ascii_hexdigit()));
            if hex { "_" } else { w }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// Translates `inputs` for `host` on `jobs` threads.
fn translate_all(host: &HostProfile, inputs: &[Input], jobs: usize) -> Table {
    let chunk = inputs.len().div_ceil(jobs.max(1)).max(1);
    let parts: Vec<Table> = std::thread::scope(|s| {
        let hs: Vec<_> = inputs
            .chunks(chunk)
            .map(|c| s.spawn(move || translate_set(host, c)))
            .collect();
        hs.into_iter()
            .map(|h| h.join().unwrap_or_else(|p| std::panic::resume_unwind(p)))
            .collect()
    });
    let mut t = Table::default();
    for p in parts {
        t.shaders.extend(p.shaders);
        t.failed.extend(p.failed);
    }
    t
}

/// Translates the programs recorded in `out`/shaders for the device of its
/// `device.json` and writes the table and its C file into `cdir`. Without a
/// device file the table is empty: the game has not run on a device yet.
///
/// # Errors
///
/// A file cannot be read or written, or `device.json` is not capabilities.
pub fn build(out: &Path, cdir: &Path, jobs: usize) -> Result<Built, BoxError> {
    let dir = out.join(DIR);
    std::fs::create_dir_all(dir.join("programs"))?;
    let inputs = programs(&dir)?;
    let device = dir.join("device.json");
    let table = if inputs.is_empty() || !device.is_file() {
        Table::default()
    } else {
        let text = std::fs::read_to_string(&device)?;
        let caps =
            DeviceCaps::from_json(&text).map_err(|e| format!("{}: {e}", device.display()))?;
        let host = HostProfile::from_device("device", &caps);
        translate_all(&host, &inputs, jobs)
    };
    let mut failed = String::new();
    let mut refused = BTreeMap::new();
    for (k, why) in &table.failed {
        let _ = writeln!(failed, "{}\t{}", hex(k), why.replace('\n', " "));
        *refused.entry(reason_kind(why)).or_insert(0) += 1;
    }
    relift_cache::write_if_changed(&dir.join("failed.txt"), failed.as_bytes())?;
    relift_cache::write_if_changed(&dir.join(TABLE), &table.to_compressed_bytes(ZSTD_LEVEL)?)?;
    relift_cache::write_if_changed(&dir.join(INTERFACES), &interfaces(&table))?;
    let c_file = cdir.join("shaders.c");
    relift_cache::write_if_changed(&c_file, c_text(&dir).as_bytes())?;
    Ok(Built {
        c_file,
        programs: inputs.len(),
        translated: table.shaders.len(),
        refused,
    })
}

/// What the runtime needs of each translation's interface, all
/// little-endian: the magic `RLSPI001`, the entry count (u32), per entry
/// sorted by key the key (32 bytes), the index of its first record and its
/// record count (u32 each), then the records, four u32 each: the record kind
/// (`RECORD_*`), the constant bank and byte offset of the bound handle, and
/// the specialization constant id (zero where a kind has none).
pub fn interfaces(t: &Table) -> Vec<u8> {
    let mut head = Vec::new();
    let mut recs: Vec<[u32; 4]> = Vec::new();
    head.extend(b"RLSPI001");
    head.extend((t.shaders.len() as u32).to_le_bytes());
    for (key, (_, _, i)) in &t.shaders {
        let first = recs.len();
        for &(bank, off) in &i.textures {
            recs.push([RECORD_TEXTURE, bank, off, 0]);
        }
        for (&(bank, off), &spec) in &i.texture_kinds {
            recs.push([RECORD_TEXTURE_KIND, bank, off, spec]);
        }
        for (&(bank, off), &spec) in &i.texture_dims {
            recs.push([RECORD_TEXTURE_DIM, bank, off, spec]);
        }
        if i.global_memory {
            recs.push([RECORD_GLOBAL_MEMORY, 0, 0, 0]);
        }
        head.extend(key);
        head.extend((first as u32).to_le_bytes());
        head.extend(((recs.len() - first) as u32).to_le_bytes());
    }
    head.extend(recs.iter().flatten().flat_map(|w| w.to_le_bytes()));
    head
}

fn c_text(dir: &Path) -> String {
    format!(
        "void rl_switch_set_shaders(const char *dir);\n\
         __attribute__((constructor)) static void rl_shaders_init(void)\n\
         {{\n    rl_switch_set_shaders({});\n}}\n",
        c_string(&dir.display().to_string())
    )
}

/// `s` as a C string literal.
fn c_string(s: &str) -> String {
    let mut t = String::from("\"");
    for b in s.bytes() {
        match b {
            b'"' | b'\\' => {
                t.push('\\');
                t.push(char::from(b));
            }
            0x20..=0x7e => t.push(char::from(b)),
            _ => {
                let _ = write!(t, "\\{b:03o}");
            }
        }
    }
    t.push('"');
    t
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_reason_kind_drops_its_details() {
        assert_eq!(
            reason_kind("BAR at 0x1640 in non-uniform control flow"),
            "BAR at _ in non-uniform control flow"
        );
        assert_eq!(
            reason_kind("decode at 0x3a0: 0011e4000d00403f0000000003ff7f8c: form"),
            "decode at _ _ form"
        );
        assert_eq!(reason_kind("no end"), "no end");
    }

    #[test]
    fn a_key_is_the_hex_name_of_a_program() {
        let k = hex_key(&format!("{}.bin", "ab".repeat(32))).unwrap();
        assert_eq!(k, [0xab; 32]);
        assert_eq!(hex(&k), "ab".repeat(32));
        assert!(hex_key("ab.bin").is_none());
    }

    #[test]
    fn a_c_string_escapes_quotes_and_bytes_outside_ascii() {
        assert_eq!(c_string("a\"b\\é"), "\"a\\\"b\\\\\\303\\251\"");
    }
}
