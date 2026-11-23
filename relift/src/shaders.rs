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

mod mesh;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::{Read as _, Write as _};
use std::path::{Path, PathBuf};

use relift_gpu_nv::translate::build::{Frame, Input, compressed_head, translate_set};
use relift_gpu_nv::translate::{DeviceCaps, HostProfile, Interface, MeshPath};
use relift_switch_shaders::{ImportSource, Options as ScanOptions, scan};

use crate::import::BoxError;

/// The zstd level of the translation table: near the ratio of the highest
/// levels on SPIR-V at a fraction of their time.
const ZSTD_LEVEL: i32 = 9;

/// The folder in the build directory.
pub const DIR: &str = "shaders";

/// The table's file in the folder.
const TABLE: &str = "table.rlspv";

/// The scan's per-file results in the folder.
const SCAN_CACHE: &str = "scan";

/// The translations' frames while the table is written (the table's index
/// comes before them and needs every frame's length).
const SPOOL: &str = "table.rlspv.frames";

/// The interface table's file in the folder.
const INTERFACES: &str = "table.rlspi";

/// The stamp file in the folder: the translator that made the tables.
pub const STAMP_FILE: &str = "table.stamp";

/// The identity of this relift's shader translator: a hash of the sources of
/// the `gpus` crates and of this module (`build.rs`), so it changes whenever
/// the translation of any program could. Every table carries it in
/// [`STAMP_FILE`], and every program this relift builds expects it there (the
/// runtime stops on a table with another).
pub const STAMP: &str = env!("RELIFT_SHADER_STAMP");

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
/// The program stores through, or does an atomic read-modify-write on, 64-bit global addresses; the second word is 1
/// when every such write also marks its pages in the written-page bitmap (`Interface::marks_written_pages`).
pub const RECORD_GLOBAL_STORE: u32 = 7;
/// An interface record: the program reads or writes a texture or image whose handle comes from a register.
pub const RECORD_BINDLESS: u32 = 8;
/// An interface record: the program reads a texture handle from a register,
/// so the runtime keeps every registered texture in the slot of its id.
pub const RECORD_REGISTER_HANDLES: u32 = 10;
/// An interface record: handles in registers are loaded from the word of
/// the bank at the byte offset, or with the last field 1 from any word of
/// the bank from that offset on.
pub const RECORD_HANDLE_SOURCE: u32 = 9;
/// An interface record: the vertex program loads through the storage
/// buffer descriptor of the vertex storage buffer slot in the second
/// field (set 1, binding 64 plus the slot).
pub const RECORD_STORAGE_SLOT: u32 = 11;

/// The environment variable that names the folder the runtime reads the
/// tables from.
pub const TABLES_ENV: &str = "RELIFT_NVN_TABLES";

/// The environment variable that names the folder the runtime records the
/// programs the tables lack and the device capabilities in.
pub const SHADERS_ENV: &str = "RELIFT_NVN_SHADERS";

/// What a build's shader table holds.
pub struct Built {
    /// Programs found: by the scan of the import and recorded by runs.
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

/// The keys of the programs in `dir`/programs (found by the scan or
/// recorded by a run), sorted.
fn program_keys(dir: &Path) -> Result<Vec<[u8; 32]>, BoxError> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(dir.join("programs")) else {
        return Ok(out);
    };
    for e in rd {
        if let Some(key) = e?.file_name().to_str().and_then(hex_key) {
            out.push(key);
        }
    }
    out.sort_unstable();
    Ok(out)
}

/// What the scan of an import found.
pub struct Discovered {
    /// Programs found (unique by content).
    pub programs: usize,
    /// What went wrong in the scan, by message and count.
    pub errors: BTreeMap<String, u64>,
}

/// Finds every shader program of the Switch 2 game in `import`, by its
/// content alone (see `relift_switch_shaders`): `out`/shaders/programs
/// gets one `<sha256>.bin` per program, and the scan keeps its per-file
/// results in `out`/shaders/scan so that a rebuild reads only what
/// changed. Nothing a run recorded is needed.
///
/// # Errors
///
/// The import or the folder cannot be read or written.
pub fn discover(import: &Path, out: &Path) -> Result<Discovered, BoxError> {
    let dir = out.join(DIR);
    let programs = dir.join("programs");
    std::fs::create_dir_all(&programs)?;
    let source = ImportSource::new(import)?;
    let report = scan(
        &source,
        &programs,
        &ScanOptions {
            cache: Some(dir.join(SCAN_CACHE)),
            ..ScanOptions::default()
        },
    )?;
    relift_cache::write_if_changed(
        &dir.join(mesh::FILE),
        &serde_json::to_vec(&report.meshes_json())?,
    )?;
    Ok(Discovered {
        programs: report.programs.len(),
        errors: report
            .errors
            .iter()
            .map(|(k, v)| (k.clone(), v.count))
            .collect(),
    })
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

/// One program translated and compressed, or why not.
enum Item {
    Skipped,
    Failed(String),
    Done {
        frame: Vec<u8>,
        words: u32,
        records: Vec<[u32; 4]>,
        /// The helper modules of a mesh program's compute back end, with
        /// their frames and lengths in words.
        helpers: Vec<([u8; 32], Vec<u8>, u32)>,
    },
}

fn translate_one(
    host: &HostProfile,
    dir: &Path,
    key: [u8; 32],
    pipes: &mesh::Pipelines,
) -> Result<Item, BoxError> {
    let code = std::fs::read(dir.join("programs").join(format!("{}.bin", hex(&key))))?;
    if relift_switch_formats::sha256(&code) != key {
        return Ok(Item::Skipped);
    }
    let t = translate_set(
        host,
        &[Input {
            key,
            code,
            options: pipes.options(&key),
        }],
        relift_gpu_sm86::program::parse,
    );
    if let Some(why) = t.failed.get(&key) {
        return Ok(Item::Failed(why.clone()));
    }
    let Some((_, words, interface)) = t.shaders.get(&key) else {
        return Ok(Item::Skipped);
    };
    let raw: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    let mut records = records(interface);
    let mut helpers = Vec::new();
    if let Some(m) = pipes.member(&key) {
        records.extend(mesh::records(m, interface.host.mesh, interface.mesh_slots));
        if m.role == mesh::Role::Mesh && interface.host.mesh == MeshPath::Compute {
            match mesh::helpers(&key, m, host, interface.mesh_slots) {
                Ok(list) => {
                    for (k, w) in list {
                        let raw: Vec<u8> = w.iter().flat_map(|x| x.to_le_bytes()).collect();
                        helpers.push((k, zstd::bulk::compress(&raw, ZSTD_LEVEL)?, w.len() as u32));
                    }
                }
                Err(why) => return Ok(Item::Failed(why)),
            }
        }
    }
    Ok(Item::Done {
        frame: zstd::bulk::compress(&raw, ZSTD_LEVEL)?,
        words: words.len() as u32,
        records,
        helpers,
    })
}

/// Translates the programs in `out`/shaders/programs for the device of its
/// `device.json` and writes the tables beside them, a batch of programs at
/// a time: a game's translations run to gigabytes, more than memory holds.
/// Without a device file the table is empty: the game has not run on a
/// device yet.
///
/// # Errors
///
/// A file cannot be read or written, or `device.json` is not capabilities.
pub fn build(out: &Path, jobs: usize) -> Result<Built, BoxError> {
    let dir = out.join(DIR);
    build_in(&dir, &dir, jobs)
}

/// The folder of the tables a build for `target` (an `android-*` or `ios-*`
/// name) carries: `out`/`target`/shaders. Its `device.json` is the
/// capabilities of the target device, which the app writes at its first start
/// to its own shader folder; the host's table never goes to another device.
#[must_use]
pub fn target_dir(out: &Path, target: &str) -> PathBuf {
    out.join(target).join(DIR)
}

/// What [`build_target`] did.
pub enum TargetBuilt {
    /// The game has no GPU programs (not a Switch 2 game).
    NoPrograms,
    /// No `device.json` for the target: the path it is wanted at.
    NoDevice(PathBuf),
    /// The table for the target's device.
    Built(Built),
}

/// Translates the programs of the build in `out` for `target`'s device
/// (the `device.json` in [`target_dir`]) into that folder's tables.
///
/// # Errors
///
/// As [`build`].
pub fn build_target(out: &Path, target: &str, jobs: usize) -> Result<TargetBuilt, BoxError> {
    let programs = out.join(DIR);
    if program_keys(&programs)?.is_empty() {
        return Ok(TargetBuilt::NoPrograms);
    }
    let dir = target_dir(out, target);
    let device = dir.join("device.json");
    if !device.is_file() {
        for f in [TABLE, INTERFACES, STAMP_FILE] {
            let _ = std::fs::remove_file(dir.join(f));
        }
        return Ok(TargetBuilt::NoDevice(device));
    }
    Ok(TargetBuilt::Built(build_in(&programs, &dir, jobs)?))
}

/// Translates the programs in `programs`/programs (with the scan's
/// `meshes.json` there) for the device of `dir`/device.json, writing the
/// tables and `failed.txt` in `dir`.
fn build_in(programs: &Path, dir: &Path, jobs: usize) -> Result<Built, BoxError> {
    std::fs::create_dir_all(programs.join("programs"))?;
    std::fs::create_dir_all(dir)?;
    let keys = program_keys(programs)?;
    let device = dir.join("device.json");
    let mut failed = String::new();
    let mut refused = BTreeMap::new();
    let mut index: Vec<Frame> = Vec::new();
    let mut interface: Vec<([u8; 32], Vec<[u32; 4]>)> = Vec::new();
    let mut extras: BTreeMap<[u8; 32], (Vec<u8>, u32)> = BTreeMap::new();
    let spool = dir.join(SPOOL);
    if !keys.is_empty() && device.is_file() {
        let text = std::fs::read_to_string(&device)?;
        let caps =
            DeviceCaps::from_json(&text).map_err(|e| format!("{}: {e}", device.display()))?;
        let host = HostProfile::from_device("device", &caps);
        let pipes = mesh::Pipelines::read(programs, &host)?;
        let mut spool_file = relift_cache::StreamWriter::new(&spool)?;
        for batch in keys.chunks(jobs.max(1) * 4) {
            let items = relift_proc::workers::parallel_with(jobs, batch, |&k| {
                translate_one(&host, programs, k, &pipes).map_err(|e| e.to_string())
            });
            for (&key, item) in batch.iter().zip(items) {
                match item? {
                    Item::Skipped => {}
                    Item::Failed(why) => {
                        let _ = writeln!(failed, "{}\t{}", hex(&key), why.replace('\n', " "));
                        *refused.entry(reason_kind(&why)).or_insert(0) += 1;
                    }
                    Item::Done {
                        frame,
                        words,
                        records,
                        helpers,
                    } => {
                        for (k, f, w) in helpers {
                            extras.insert(k, (f, w));
                        }
                        index.push(Frame {
                            key,
                            frame_len: frame.len() as u32,
                            words,
                        });
                        spool_file.write_all(&frame)?;
                        spool_file.write_all(&[0u8; 3][..frame.len().wrapping_neg() % 4])?;
                        interface.push((key, records));
                    }
                }
            }
        }
        spool_file.finish()?;
    }
    relift_cache::write_if_changed(&dir.join("failed.txt"), failed.as_bytes())?;
    let translated = index.len();
    write_table(&dir.join(TABLE), &spool, &index, &extras)?;
    let _ = std::fs::remove_file(&spool);
    relift_cache::write_if_changed(&dir.join(INTERFACES), &interface_table(&interface))?;
    write_stamp(dir)?;
    Ok(Built {
        programs: keys.len(),
        translated,
        refused,
    })
}

/// Writes this relift's [`STAMP`] in `dir`, beside the tables it made.
fn write_stamp(dir: &Path) -> std::io::Result<()> {
    relift_cache::write_if_changed(&dir.join(STAMP_FILE), format!("{STAMP}\n").as_bytes()).map(drop)
}

/// The stamp of the tables in `dir`, or none when they have no stamp (made
/// by a relift that wrote none).
#[must_use]
pub fn stamp_of(dir: &Path) -> Option<String> {
    let s = std::fs::read_to_string(dir.join(STAMP_FILE)).ok()?;
    let s = s.trim();
    (!s.is_empty()).then(|| s.to_string())
}

/// Writes the table at `path`: the frames of `index` that `spool` holds,
/// in order, with the helper modules `extras` (frame and length in words,
/// by key) among them in key order.
fn write_table(
    path: &Path,
    spool: &Path,
    index: &[Frame],
    extras: &BTreeMap<[u8; 32], (Vec<u8>, u32)>,
) -> Result<(), BoxError> {
    enum Source<'a> {
        Spool(u64),
        Extra(&'a [u8]),
    }
    let mut all: Vec<(Frame, Source)> = Vec::with_capacity(index.len() + extras.len());
    for f in index {
        all.push((
            *f,
            Source::Spool(u64::from(f.frame_len).next_multiple_of(4)),
        ));
    }
    for (key, (frame, words)) in extras {
        all.push((
            Frame {
                key: *key,
                frame_len: frame.len() as u32,
                words: *words,
            },
            Source::Extra(frame),
        ));
    }
    all.sort_by_key(|(f, _)| f.key);
    let heads: Vec<Frame> = all.iter().map(|(f, _)| *f).collect();
    let mut table = relift_cache::StreamWriter::new(path)?;
    table.write_all(&compressed_head(&heads))?;
    let mut from = spool
        .is_file()
        .then(|| std::fs::File::open(spool))
        .transpose()?;
    let mut buf = vec![0u8; 1 << 20];
    for (_, source) in &all {
        match source {
            Source::Spool(len) => {
                let Some(f) = from.as_mut() else {
                    continue;
                };
                let mut left = *len;
                while left > 0 {
                    let n = (left as usize).min(buf.len());
                    f.read_exact(&mut buf[..n])?;
                    table.write_all(&buf[..n])?;
                    left -= n as u64;
                }
            }
            Source::Extra(frame) => {
                table.write_all(frame)?;
                table.write_all(&[0u8; 3][..frame.len().wrapping_neg() % 4])?;
            }
        }
    }
    table.finish()?;
    Ok(())
}

/// `relift shaders`: translates the programs recorded in `out`/shaders again
/// and says how many came out and how long it took.
///
/// # Errors
///
/// As [`build`].
pub fn run(out: &Path, ui: &mut crate::ui::Ui) -> Result<(), BoxError> {
    let jobs = relift_proc::workers::jobs().max(1);
    let start = std::time::Instant::now();
    let built = build(out, jobs)?;
    ui.note(&relift_progress::Line::new().text(format!(
        "{} of {} programs translated on {jobs} threads in {:.1} s",
        built.translated,
        built.programs,
        start.elapsed().as_secs_f64()
    )));
    for (why, n) in &built.refused {
        ui.note(&relift_progress::Line::new().text(format!("{n} refused: {why}")));
    }
    Ok(())
}

/// What the runtime needs of each translation's interface, all
/// little-endian: the magic `RLSPI001`, the entry count (u32), per entry
/// sorted by key the key (32 bytes), the index of its first record and its
/// record count (u32 each), then the records, four u32 each: the record kind
/// (`RECORD_*`), the constant bank and byte offset of the bound handle, and
/// the specialization constant id (zero where a kind has none).
pub fn interface_table(entries: &[([u8; 32], Vec<[u32; 4]>)]) -> Vec<u8> {
    let mut head = Vec::new();
    let mut recs: Vec<[u32; 4]> = Vec::new();
    head.extend(b"RLSPI001");
    head.extend((entries.len() as u32).to_le_bytes());
    for (key, rs) in entries {
        head.extend(key);
        head.extend((recs.len() as u32).to_le_bytes());
        head.extend((rs.len() as u32).to_le_bytes());
        recs.extend_from_slice(rs);
    }
    head.extend(recs.iter().flatten().flat_map(|w| w.to_le_bytes()));
    head
}

/// The interface records of one translation (see [`interface_table`]).
fn records(i: &Interface) -> Vec<[u32; 4]> {
    let mut recs = Vec::new();
    for &(bank, off) in &i.textures {
        recs.push([RECORD_TEXTURE, bank, off, 0]);
    }
    for (&(bank, off), &spec) in i.texture_kinds.iter().chain(&i.texture_fetch_kinds) {
        recs.push([RECORD_TEXTURE_KIND, bank, off, spec]);
    }
    for (&(bank, off), &spec) in &i.texture_dims {
        recs.push([RECORD_TEXTURE_DIM, bank, off, spec]);
    }
    if i.global_memory {
        recs.push([RECORD_GLOBAL_MEMORY, 0, 0, 0]);
    }
    if i.global_store {
        recs.push([RECORD_GLOBAL_STORE, u32::from(i.marks_written_pages), 0, 0]);
    }
    if i.bindless {
        recs.push([RECORD_BINDLESS, 0, 0, 0]);
    }
    if i.register_handles {
        recs.push([RECORD_REGISTER_HANDLES, 0, 0, 0]);
    }
    for h in &i.handle_sources {
        recs.push([RECORD_HANDLE_SOURCE, h.bank, h.offset, u32::from(h.indexed)]);
    }
    for &k in &i.storage_slots {
        recs.push([RECORD_STORAGE_SLOT, k, 0, 0]);
    }
    recs
}

/// The table files of the build in `out` that a package carries: both
/// tables and their stamp when the build translated at least one program,
/// else none.
#[must_use]
pub fn tables(out: &Path) -> Vec<PathBuf> {
    tables_in(&out.join(DIR))
}

/// The table files a package for `target` carries: the host's for `None`,
/// else only those translated for the target's own device ([`target_dir`]),
/// never the host's.
#[must_use]
pub fn tables_for(out: &Path, target: Option<&str>) -> Vec<PathBuf> {
    match target {
        None => tables(out),
        Some(t) => tables_in(&target_dir(out, t)),
    }
}

fn tables_in(dir: &Path) -> Vec<PathBuf> {
    let files = [dir.join(TABLE), dir.join(INTERFACES)];
    let head = std::fs::metadata(&files[0]).map_or(0, |m| m.len());
    if head > TABLE_HEAD_BYTES && files[1].is_file() {
        let mut files: Vec<PathBuf> = files.into();
        let stamp = dir.join(STAMP_FILE);
        if stamp.is_file() {
            files.push(stamp);
        }
        files
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
    use relift_gpu_nv::translate::Options;

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
    fn a_register_handle_is_recorded_as_bindless() {
        let mut i = Interface::default();
        assert!(records(&i).iter().all(|r| r[0] != RECORD_BINDLESS));
        i.bindless = true;
        assert!(records(&i).contains(&[RECORD_BINDLESS, 0, 0, 0]));
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
    fn a_package_carries_the_stamp_beside_the_tables() {
        let out = scratch("stamp-package");
        let head = b"RLSPV002\0\0\0\0";
        relift_cache::write_if_changed(
            &out.join(DIR).join(TABLE),
            &[head.as_slice(), b"x"].concat(),
        )
        .unwrap();
        relift_cache::write_if_changed(&out.join(DIR).join(INTERFACES), b"RLSPI001").unwrap();
        assert_eq!(tables(&out).len(), 2);
        write_stamp(&out.join(DIR)).unwrap();
        let t = tables(&out);
        assert_eq!(t.last().unwrap(), &out.join(DIR).join(STAMP_FILE));
        let dest = out.join("pkg").join("shaders");
        install(&t, &dest).unwrap();
        assert_eq!(stamp_of(&dest).as_deref(), Some(STAMP));
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn the_stamp_is_the_hash_the_script_computes_for_the_tree() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let Ok(run) = relift_proc::command("bash")
            .arg(root.join("scripts/shader-stamp.sh"))
            .arg(&root)
            .output()
        else {
            return;
        };
        if !run.status.success() {
            return;
        }
        assert_eq!(String::from_utf8(run.stdout).unwrap().trim(), STAMP);
    }

    #[test]
    fn a_table_without_a_stamp_reads_as_unstamped() {
        let out = scratch("stamp-read");
        let dir = out.join(DIR);
        assert_eq!(stamp_of(&dir), None);
        relift_cache::write_if_changed(&dir.join(STAMP_FILE), b"\n").unwrap();
        assert_eq!(stamp_of(&dir), None);
        relift_cache::write_if_changed(&dir.join(STAMP_FILE), b"0123456789abcdef\n").unwrap();
        assert_eq!(stamp_of(&dir).as_deref(), Some("0123456789abcdef"));
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

    const CAPS: &str =
        r#"{"capabilities": {"device": {"extensions": {}, "features": {}, "properties": {}}}}"#;

    fn words(ws: &[u128]) -> Vec<u8> {
        ws.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    fn exit() -> u128 {
        0x14d | (4 << 9) | (7 << 12) | (7 << 87) | (7 << 110) | (7 << 113)
    }

    fn bra_self() -> u128 {
        0x147 | (4 << 9) | (7 << 12) | (0xffff_ffff_fffc << 34) | (7 << 87)
    }

    fn adler32(b: &[u8]) -> u32 {
        let (mut a, mut s) = (1u32, 0u32);
        for &x in b {
            a = (a + u32::from(x)) % 65521;
            s = (s + a) % 65521;
        }
        (s << 16) | a
    }

    fn zlib_stored(b: &[u8]) -> Vec<u8> {
        let n = u16::try_from(b.len()).unwrap();
        let mut v = vec![0x78, 0x01, 1];
        v.extend_from_slice(&n.to_le_bytes());
        v.extend_from_slice(&(!n).to_le_bytes());
        v.extend_from_slice(b);
        v.extend_from_slice(&adler32(b).to_be_bytes());
        v
    }

    #[test]
    fn the_table_is_what_translating_the_whole_set_in_memory_gives() {
        use relift_gpu_nv::translate::build::translate_set;
        let out = scratch("table");
        let dir = out.join(DIR);
        relift_cache::write_if_changed(&dir.join("device.json"), CAPS.as_bytes()).unwrap();
        std::fs::create_dir_all(dir.join("programs")).unwrap();
        let good = words(&[exit(), bra_self()]);
        let longer = words(&[exit(), exit(), bra_self()]);
        let bad = words(&[0x1ff, exit(), bra_self()]);
        let mut inputs = Vec::new();
        for code in [&good, &longer, &bad] {
            let key = relift_switch_formats::sha256(code);
            relift_cache::write_if_changed(
                &dir.join("programs").join(format!("{}.bin", hex(&key))),
                code,
            )
            .unwrap();
            inputs.push(relift_gpu_nv::translate::build::Input {
                key,
                code: code.clone(),
                options: Options::default(),
            });
        }
        let built = build(&out, 3).unwrap();
        assert_eq!((built.programs, built.translated), (3, 2));
        assert_eq!(built.refused.values().sum::<usize>(), 1);
        let host = HostProfile::from_device("device", &DeviceCaps::from_json(CAPS).unwrap());
        let whole = translate_set(&host, &inputs, relift_gpu_sm86::program::parse);
        assert_eq!(
            std::fs::read(dir.join(TABLE)).unwrap(),
            whole.to_compressed_bytes(ZSTD_LEVEL).unwrap()
        );
        let entries: Vec<_> = whole
            .shaders
            .iter()
            .map(|(k, (_, _, i))| (*k, records(i)))
            .collect();
        assert_eq!(
            std::fs::read(dir.join(INTERFACES)).unwrap(),
            interface_table(&entries)
        );
        assert_eq!(
            std::fs::read_to_string(dir.join("failed.txt"))
                .unwrap()
                .lines()
                .count(),
            1
        );
        assert!(!dir.join(SPOOL).exists());
        assert_eq!(stamp_of(&dir).as_deref(), Some(STAMP));
        relift_cache::write_if_changed(&dir.join(STAMP_FILE), b"0000000000000000\n").unwrap();
        build(&out, 1).unwrap();
        assert_eq!(
            stamp_of(&dir).as_deref(),
            Some(STAMP),
            "a rebuild refreshes an old translator's stamp"
        );
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_target_gets_a_table_for_its_own_device_and_never_the_hosts() {
        let out = scratch("target");
        let dir = out.join(DIR);
        std::fs::create_dir_all(dir.join("programs")).unwrap();
        let t = "android-arm64";
        assert!(matches!(
            build_target(&out, t, 1).unwrap(),
            TargetBuilt::NoPrograms
        ));
        let code = words(&[exit(), bra_self()]);
        let key = relift_switch_formats::sha256(&code);
        relift_cache::write_if_changed(
            &dir.join("programs").join(format!("{}.bin", hex(&key))),
            &code,
        )
        .unwrap();
        relift_cache::write_if_changed(&dir.join("device.json"), CAPS.as_bytes()).unwrap();
        assert_eq!(build(&out, 1).unwrap().translated, 1);
        assert_eq!(tables_for(&out, None).len(), 3);
        assert_eq!(stamp_of(&dir).as_deref(), Some(STAMP));
        let want = target_dir(&out, t).join("device.json");
        match build_target(&out, t, 1).unwrap() {
            TargetBuilt::NoDevice(p) => assert_eq!(p, want),
            _ => panic!("a target without its device file has no table"),
        }
        assert!(tables_for(&out, Some(t)).is_empty());
        std::fs::create_dir_all(target_dir(&out, t)).unwrap();
        relift_cache::write_if_changed(&want, CAPS.as_bytes()).unwrap();
        match build_target(&out, t, 1).unwrap() {
            TargetBuilt::Built(b) => assert_eq!((b.programs, b.translated), (1, 1)),
            _ => panic!("a target with its device file gets a table"),
        }
        let files = tables_for(&out, Some(t));
        assert_eq!(
            files,
            [
                target_dir(&out, t).join(TABLE),
                target_dir(&out, t).join(INTERFACES),
                target_dir(&out, t).join(STAMP_FILE)
            ]
        );
        std::fs::remove_file(&want).unwrap();
        assert!(matches!(
            build_target(&out, t, 1).unwrap(),
            TargetBuilt::NoDevice(_)
        ));
        assert!(
            tables_for(&out, Some(t)).is_empty(),
            "a stale target table is removed"
        );
        assert!(
            stamp_of(&target_dir(&out, t)).is_none(),
            "and so is its stamp"
        );
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_build_without_a_device_has_an_empty_table() {
        let out = scratch("nodevice");
        let key = relift_switch_formats::sha256(b"x");
        std::fs::create_dir_all(out.join(DIR).join("programs")).unwrap();
        relift_cache::write_if_changed(
            &out.join(DIR)
                .join("programs")
                .join(format!("{}.bin", hex(&key))),
            b"x",
        )
        .unwrap();
        let built = build(&out, 2).unwrap();
        assert_eq!((built.programs, built.translated), (1, 0));
        assert!(tables(&out).is_empty());
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn the_scan_of_an_import_needs_nothing_a_run_recorded() {
        let out = scratch("discover");
        let import = out.join("import");
        let program = words(&[exit(), bra_self()]);
        let mut data = vec![0x80u8; 100];
        data.extend(zlib_stored(&program));
        let img = relift_switch_formats::romfs::build(&[("any/file.dat", &data)]);
        std::fs::create_dir_all(&import).unwrap();
        relift_cache::write_if_changed(&import.join("romfs.img"), &img).unwrap();
        let found = discover(&import, &out).unwrap();
        assert_eq!(found.programs, 1);
        let key = relift_switch_formats::sha256(&program);
        let bin = out
            .join(DIR)
            .join("programs")
            .join(format!("{}.bin", hex(&key)));
        assert_eq!(std::fs::read(bin).unwrap(), program);
        assert_eq!(program_keys(&out.join(DIR)).unwrap(), [key]);
        std::fs::remove_dir_all(&out).unwrap();
    }

    #[test]
    fn a_c_string_escapes_quotes_and_bytes_outside_ascii() {
        assert_eq!(c_string("a\"b\\é"), "\"a\\\"b\\\\\\303\\251\"");
    }
}
