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
//! program's translation runs to megabytes, a game's to gigabytes). The
//! program does not name the folder: the runtime looks in `shaders/` beside
//! its executable, which is this folder for the program `relift build` leaves
//! in the build directory, and a package carries [`tables`] with the program
//! and points the runtime at them (see [`env`]).

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::{Read as _, Write as _};
use std::path::{Path, PathBuf};

use relift_gpu_nv::translate::build::{Input, Table, translate_set};
use relift_gpu_nv::translate::{DeviceCaps, HostProfile, Options};

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

/// The environment variable that names the folder the runtime reads the
/// tables from.
pub const TABLES_ENV: &str = "RELIFT_NVN_TABLES";

/// The environment variable that names the folder the runtime records the
/// programs the tables lack and the device capabilities in.
pub const SHADERS_ENV: &str = "RELIFT_NVN_SHADERS";

/// What a build's shader table holds.
pub struct Built {
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
            .map(|c| s.spawn(move || translate_set(host, c, relift_gpu_sm86::program::parse)))
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
/// `device.json` and writes the tables beside them. Without a
/// device file the table is empty: the game has not run on a device yet.
///
/// # Errors
///
/// A file cannot be read or written, or `device.json` is not capabilities.
pub fn build(out: &Path, jobs: usize) -> Result<Built, BoxError> {
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
    Ok(Built {
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

/// The table files of the build in `out` that a package carries: both when
/// the build translated at least one program, else none.
#[must_use]
pub fn tables(out: &Path) -> Vec<PathBuf> {
    let dir = out.join(DIR);
    let files = [dir.join(TABLE), dir.join(INTERFACES)];
    let head = std::fs::metadata(&files[0]).map_or(0, |m| m.len());
    if head > TABLE_HEAD_BYTES && files[1].is_file() {
        files.into()
    } else {
        Vec::new()
    }
}

/// The length of a table with no module: its magic and its count.
const TABLE_HEAD_BYTES: u64 = 12;

/// Copies the table `files` into `dir`, which is made, streaming them (a
/// table runs to gigabytes) and leaving a file that already holds the same
/// bytes alone.
///
/// # Errors
///
/// A folder cannot be made or a file cannot be copied.
pub fn install(files: &[PathBuf], dir: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(dir)?;
    for f in files {
        let Some(name) = f.file_name() else {
            continue;
        };
        let mut from = std::fs::File::open(f)?;
        let mut to = relift_cache::StreamWriter::new(&dir.join(name))?;
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = from.read(&mut buf)?;
            if n == 0 {
                break;
            }
            to.write_all(&buf[..n])?;
        }
        to.finish()?;
    }
    Ok(())
}

/// Points a run of the program built in `out` at the build's shader folder,
/// the one the next build reads, where the program built elsewhere (the dev
/// build, whose folder is below `out`) would not look.
pub fn set_env(cmd: &mut std::process::Command, out: &Path) {
    cmd.env(SHADERS_ENV, out.join(DIR));
}

/// `s` as a C string literal.
pub(crate) fn c_string(s: &str) -> String {
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

    fn scratch(tag: &str) -> PathBuf {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("target")
            .join(format!("relift-shaders-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join(DIR)).unwrap();
        d
    }

    #[test]
    fn a_package_carries_the_tables_only_when_something_was_translated() {
        let out = scratch("tables");
        assert!(tables(&out).is_empty());
        let head = b"RLSPV002\0\0\0\0";
        relift_cache::write_if_changed(&out.join(DIR).join(TABLE), head).unwrap();
        relift_cache::write_if_changed(&out.join(DIR).join(INTERFACES), b"RLSPI001").unwrap();
        assert!(tables(&out).is_empty());
        relift_cache::write_if_changed(
            &out.join(DIR).join(TABLE),
            &[head.as_slice(), b"x"].concat(),
        )
        .unwrap();
        let t = tables(&out);
        assert_eq!(
            t,
            [out.join(DIR).join(TABLE), out.join(DIR).join(INTERFACES)]
        );
        let dest = out.join("pkg").join("shaders");
        install(&t, &dest).unwrap();
        assert_eq!(std::fs::read(dest.join(TABLE)).unwrap().len(), 13);
        assert!(dest.join(INTERFACES).is_file());
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_run_is_pointed_at_the_build_folder() {
        let mut cmd = relift_proc::command("true");
        set_env(&mut cmd, Path::new("/b/host"));
        let env: Vec<_> = cmd.get_envs().collect();
        assert_eq!(
            env,
            [(
                std::ffi::OsStr::new(SHADERS_ENV),
                Some(Path::new("/b/host/shaders").as_os_str())
            )]
        );
    }

    #[test]
    fn a_c_string_escapes_quotes_and_bytes_outside_ascii() {
        assert_eq!(c_string("a\"b\\é"), "\"a\\\"b\\\\\\303\\251\"");
    }
}
