//! A Switch 2 game's shader table: every recorded SM86 program translated
//! for one device, in the files the runtime reads.
//!
//! relift makes the table when it builds a game (for the build machine's
//! GPU, or for a target device's captured capabilities), and the on-device
//! translator (`gpus/xlate`) makes it on the player's device; both
//! run [`translate`], so a table depends only on the programs, the device
//! description and the translator ([`STAMP`]).
//!
//! The table folder holds:
//!
//! * [`TABLE`], the translations, each module a zstd frame
//!   (`Table::to_compressed_bytes` at [`ZSTD_LEVEL`]);
//! * [`INTERFACES`], what the runtime must know of each translation beyond
//!   its SPIR-V (see [`interface_table`]);
//! * [`STAMP_FILE`], the translator that made them;
//! * [`DEVICE_FILE`], the device description they were made for (the bytes
//!   of the runtime's `device.json`), so a runtime whose device or driver
//!   now reports something else knows the tables are stale;
//! * `failed.txt`: one line per program the translator refused, the key and
//!   the reason.
//!
//! The tables stay files the runtime reads a module at a time (one
//! program's translation runs to megabytes, a game's to gigabytes).

mod mesh;
pub mod pack;

use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::io::{Read as _, Write as _};
use std::path::Path;

use relift_gpu_nv::translate::build::{Frame, Input, compressed_head, translate_set};
use relift_gpu_nv::translate::{
    DeviceCaps, HostProfile, ImageDim, ImageKind, Interface, Math, MeshPath, Options,
    XfbGlslcBuffer, XfbLayout, image_binding,
};

pub use pack::{KERNELS, MESHES, PACK, Programs, SECTIONS, XFB};

/// An error with its message.
pub type BoxError = Box<dyn std::error::Error + Send + Sync>;

/// The zstd level of the translation table: near the ratio of the highest
/// levels on SPIR-V at a fraction of their time.
pub const ZSTD_LEVEL: i32 = 9;

/// The table's file in the folder.
pub const TABLE: &str = "table.rlspv";

/// The translations' frames while the table is written (the table's index
/// comes before them and needs every frame's length).
pub const SPOOL: &str = "table.rlspv.frames";

/// The interface table's file in the folder.
pub const INTERFACES: &str = "table.rlspi";

/// The stamp file in the folder: the translator that made the tables.
pub const STAMP_FILE: &str = "table.stamp";

/// The device file in the folder: the device description the tables were
/// made for.
pub const DEVICE_FILE: &str = "table.device";

/// The refusals' file in the folder.
pub const FAILED: &str = "failed.txt";

/// Both float math modes, exact first: what a package or a phone target
/// carries, so that the player can pick either on the device.
pub const BOTH: [Math; 2] = [Math::Exact, Math::Native];

/// The file name of a table file of `math`: `table.<ext>` for exact math,
/// `table-native.<ext>` for native math. The runtime names them the same way
/// (nvn_shader.c, pick_tables).
#[must_use]
pub fn table_file(math: Math, ext: &str) -> String {
    match math {
        Math::Exact => format!("table.{ext}"),
        m => format!("table-{}.{ext}", m.name()),
    }
}

/// The stamp the tables of `math` carry and a program expects: [`STAMP`]
/// for exact math, [`STAMP`] and `-native` for native math, so that a table
/// of one mode is never read as the other.
#[must_use]
pub fn stamp(math: Math) -> String {
    match math {
        Math::Exact => STAMP.to_string(),
        m => format!("{STAMP}-{}", m.name()),
    }
}

/// The identity of this shader translator: a hash of the sources of the
/// `gpus` crates (`build.rs`), so it changes whenever the translation of any
/// program could. Every table carries it in [`STAMP_FILE`], and every
/// program relift builds expects it there.
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
/// An interface record: the program reads or writes the storage image array
/// at the binding in the second field (set 2, from binding 32 on).
pub const RECORD_IMAGE_BINDING: u32 = 12;
/// An interface record: the translation is in trace mode and writes a trace
/// buffer of the word count in the second field (set 3, binding
/// `TRACE_BINDING`; see `relift_gpu_nv::translate::Trace`).
pub const RECORD_TRACE: u32 = 13;
/// An interface record: the CUDA kernel declares the static shared memory
/// bytes in the second field (its cubin's `.nv.shared` section); a launch
/// gives it these plus the launch's dynamic shared bytes. The third field
/// is the bytes its constant shared accesses reach past them
/// (`Interface::shared_beyond_declared`; 0 when they fit).
pub const RECORD_SHARED_DECLARED: u32 = 14;

/// The static shared bytes of each CUDA kernel's program, from the scan's
/// kernels JSON (empty for none).
fn kernel_shared(json: &[u8]) -> Result<HashMap<[u8; 32], u32>, BoxError> {
    let mut out = HashMap::new();
    if json.is_empty() {
        return Ok(out);
    }
    let v: serde_json::Value =
        serde_json::from_slice(json).map_err(|e| format!("{}: {e}", pack::KERNELS))?;
    for k in v.as_array().into_iter().flatten() {
        let key = k
            .get("program")
            .and_then(|p| p.as_str())
            .and_then(|s| hex_key(&format!("{s}.bin")));
        let shared = k
            .get("shared")
            .and_then(serde_json::Value::as_u64)
            .and_then(|n| u32::try_from(n).ok());
        if let (Some(key), Some(shared)) = (key, shared) {
            out.insert(key, shared);
        }
    }
    Ok(out)
}

/// Trace mode for some programs of a table (`relift shaders --trace`): the
/// programs whose key starts with one of `keys`, translated with `spec`.
#[derive(Clone, Debug)]
pub struct TraceSel {
    pub keys: Vec<String>,
    pub spec: relift_gpu_nv::translate::Trace,
}

impl TraceSel {
    /// Parses `<key prefix>[,<key prefix>...]:<spec>` (see `Trace::parse`
    /// for the spec); the prefix `*` selects every program.
    ///
    /// # Errors
    ///
    /// The text is not of that form.
    pub fn parse(s: &str) -> Result<TraceSel, String> {
        let (keys, spec) = s
            .split_once(':')
            .ok_or_else(|| format!("trace {s:?}: <key prefix>[,...]:<condition>"))?;
        // `*`: every program
        let keys: Vec<String> = keys
            .split(',')
            .map(|k| {
                if k == "*" {
                    String::new()
                } else {
                    k.to_ascii_lowercase()
                }
            })
            .collect();
        if keys
            .iter()
            .any(|k| !k.bytes().all(|b| b.is_ascii_hexdigit()))
        {
            return Err(format!("trace {s:?}: a key prefix is not hex"));
        }
        Ok(TraceSel {
            keys,
            spec: relift_gpu_nv::translate::Trace::parse(spec)?,
        })
    }

    /// The trace spec for program `key`, if it is selected.
    #[must_use]
    pub fn of(&self, key: &[u8; 32]) -> Option<relift_gpu_nv::translate::Trace> {
        let h = hex(key);
        self.keys
            .iter()
            .any(|k| h.starts_with(k.as_str()))
            .then(|| self.spec.clone())
    }
}
/// An interface record: the translation captures transform feedback buffer
/// (second field) with the stride in bytes in the third field and the
/// vertex stream in the fourth (`Interface::xfb`). 13 and 14 are taken by
/// other records.
pub const RECORD_XFB: u32 = 15;
/// An interface record: the translation captures through memory
/// (`Interface::xfb_memory`): the bytes of a record in the second field and
/// the records of one input primitive in the third.
pub const RECORD_XFB_MEMORY: u32 = 16;
/// An interface record of a memory path capture: buffer (second field)
/// words from the third field's on take the four layout select bytes of
/// the fourth (the first in its low byte).
pub const RECORD_XFB_SELECT: u32 = 17;

/// The transform feedback layout of each program the scan's xfb JSON names
/// (empty for none), as the driver programs it ([`XfbLayout::from_glslc`]).
fn xfb_layouts(json: &[u8]) -> Result<HashMap<[u8; 32], XfbLayout>, BoxError> {
    let mut out = HashMap::new();
    if json.is_empty() {
        return Ok(out);
    }
    let v: serde_json::Value =
        serde_json::from_slice(json).map_err(|e| format!("{}: {e}", pack::XFB))?;
    let bytes = |b: &serde_json::Value, k: &str| -> Vec<u8> {
        b.get(k)
            .and_then(serde_json::Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(serde_json::Value::as_i64)
            .map(|x| x as u8)
            .collect()
    };
    for x in v.as_array().into_iter().flatten() {
        let Some(key) = x
            .get("program")
            .and_then(serde_json::Value::as_str)
            .and_then(|s| hex_key(&format!("{s}.bin")))
        else {
            continue;
        };
        let raw = x
            .get("raw")
            .and_then(serde_json::Value::as_bool)
            .unwrap_or(false);
        let buffers: Vec<XfbGlslcBuffer> = x
            .get("buffers")
            .and_then(serde_json::Value::as_array)
            .into_iter()
            .flatten()
            .map(|b| XfbGlslcBuffer {
                stride: b
                    .get("stride")
                    .and_then(serde_json::Value::as_u64)
                    .unwrap_or(0) as u16,
                stream: b
                    .get("stream")
                    .and_then(serde_json::Value::as_u64)
                    .unwrap_or(0) as u8,
                attr: bytes(b, "attr").into_iter().map(|a| a as i8).collect(),
                first: bytes(b, "first"),
                ncomp: bytes(b, "ncomp"),
            })
            .collect();
        out.insert(key, XfbLayout::from_glslc(raw, &buffers));
    }
    Ok(out)
}

/// The SHA-256 of `b`, the key a program goes by.
#[must_use]
pub fn sha256(b: &[u8]) -> [u8; 32] {
    use sha2::{Digest, Sha256};
    Sha256::digest(b).into()
}

/// The key a program file `<64 hex digits>.bin` is named by.
#[must_use]
pub fn hex_key(name: &str) -> Option<[u8; 32]> {
    let s = name.strip_suffix(".bin")?;
    if s.len() != 64 {
        return None;
    }
    let mut k = [0u8; 32];
    for (i, b) in k.iter_mut().enumerate() {
        *b = u8::from_str_radix(s.get(2 * i..2 * i + 2)?, 16).ok()?;
    }
    Some(k)
}

/// A key in hex.
#[must_use]
pub fn hex(k: &[u8; 32]) -> String {
    k.iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    })
}

/// The kind of a refusal: its text with addresses and instruction words
/// (hex numbers) left out.
#[must_use]
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

/// The translator's profile of the device `device` describes (the
/// runtime's `device.json`).
///
/// # Errors
///
/// `device` is not capabilities JSON.
pub fn host_of(device: &[u8]) -> Result<HostProfile, BoxError> {
    let text = std::str::from_utf8(device).map_err(|e| format!("device.json: {e}"))?;
    let caps = DeviceCaps::from_json(text).map_err(|e| format!("device.json: {e}"))?;
    Ok(HostProfile::from_device("device", &caps))
}

/// What a table holds.
#[derive(Debug, Default)]
pub struct Built {
    /// Programs found: by the scan of the import and recorded by runs.
    pub programs: usize,
    /// Programs translated.
    pub translated: usize,
    /// The refusals by kind: the reason up to its first detail, and how many.
    pub refused: BTreeMap<String, usize>,
}

/// `words` as one zstd frame at [`ZSTD_LEVEL`], with this thread's
/// compressor: a fresh one per module allocates its several megabytes of
/// tables each time, which on phones' allocators costs more than the
/// compression.
fn compress(words: &[u32]) -> std::io::Result<Vec<u8>> {
    thread_local! {
        static Z: std::cell::RefCell<Option<zstd::bulk::Compressor<'static>>> =
            const { std::cell::RefCell::new(None) };
    }
    let raw: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    Z.with(|z| {
        let mut z = z.borrow_mut();
        if z.is_none() {
            *z = Some(zstd::bulk::Compressor::new(ZSTD_LEVEL)?);
        }
        z.as_mut().expect("made above").compress(&raw)
    })
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

/// One program translated for a device as a table holds it: its SPIR-V
/// words and interface records, or why it did not translate.
pub enum One {
    /// The program translated.
    Done {
        /// The module.
        words: Vec<u32>,
        /// Its interface records (see [`interface_table`]).
        records: Vec<[u32; 4]>,
    },
    /// The translator refused it, and why.
    Failed(String),
}

/// A device's translator: its profile and the pipelines of the programs it
/// translates.
pub struct Translator {
    host: HostProfile,
    pipes: mesh::Pipelines,
    /// Each capturing program's transform feedback layout (the scan's
    /// `xfb.json`).
    xfb: HashMap<[u8; 32], XfbLayout>,
    /// Static shared bytes of each CUDA kernel's program (the scan's
    /// `kernels.json`).
    shared: HashMap<[u8; 32], u32>,
    math: Math,
    trace: Option<TraceSel>,
}

impl Translator {
    /// The translator of the device `device` describes, for `programs`.
    ///
    /// # Errors
    ///
    /// As [`host_of`], or the pipelines JSON is not JSON.
    pub fn new(programs: &Programs, device: &[u8]) -> Result<Translator, BoxError> {
        Self::with_math(programs, device, Math::Exact)
    }

    /// As [`Translator::new`], for the float math `math`.
    ///
    /// # Errors
    ///
    /// As [`Translator::new`].
    pub fn with_math(
        programs: &Programs,
        device: &[u8],
        math: Math,
    ) -> Result<Translator, BoxError> {
        let host = host_of(device)?;
        let pipes = mesh::Pipelines::read(programs, &host)?;
        let xfb = xfb_layouts(programs.xfb())?;
        let shared = kernel_shared(programs.kernels())?;
        Ok(Translator {
            host,
            pipes,
            xfb,
            shared,
            math,
            trace: None,
        })
    }

    /// This translator with the programs `sel` selects in trace mode.
    #[must_use]
    pub fn with_trace(mut self, sel: Option<TraceSel>) -> Translator {
        self.trace = sel;
        self
    }

    /// Program `key`, whose bytes are `code`, translated as a table holds
    /// it (without the helper modules of a mesh pipeline).
    #[must_use]
    pub fn one(&self, key: [u8; 32], code: Vec<u8>) -> One {
        match self.raw(key, code) {
            Err(why) => One::Failed(why),
            Ok((words, interface)) => {
                let mut records = records(&interface);
                if let Some(m) = self.pipes.member(&key) {
                    records.extend(mesh::records(m, interface.host.mesh, interface.mesh_slots));
                }
                One::Done { words, records }
            }
        }
    }

    fn raw(&self, key: [u8; 32], code: Vec<u8>) -> Result<(Vec<u32>, Interface), String> {
        let mut t = translate_set(
            &self.host,
            &[Input {
                key,
                code,
                options: Options {
                    math: self.math,
                    trace: self.trace.as_ref().and_then(|t| t.of(&key)),
                    xfb: self.xfb.get(&key).cloned(),
                    shared_declared: self.shared.get(&key).copied(),
                    ..self.pipes.options(&key)
                },
            }],
            relift_gpu_sm86::program::parse,
        );
        if let Some(why) = t.failed.remove(&key) {
            return Err(why);
        }
        t.shaders
            .remove(&key)
            .map(|(_, w, i)| (w, i))
            .ok_or_else(|| "no translation".to_string())
    }

    fn item(&self, programs: &Programs, key: [u8; 32]) -> Result<Item, BoxError> {
        let Some(code) = programs.code(&key)? else {
            return Ok(Item::Skipped);
        };
        let (words, interface) = match self.raw(key, code) {
            Ok(r) => r,
            Err(why) => return Ok(Item::Failed(why)),
        };
        let mut records = records(&interface);
        let mut helpers = Vec::new();
        if let Some(m) = self.pipes.member(&key) {
            records.extend(mesh::records(m, interface.host.mesh, interface.mesh_slots));
            if m.role == mesh::Role::Mesh && interface.host.mesh == MeshPath::Compute {
                match mesh::helpers(&key, m, &self.host, interface.mesh_slots) {
                    Ok(list) => {
                        for (k, w) in list {
                            helpers.push((k, compress(&w)?, w.len() as u32));
                        }
                    }
                    Err(why) => return Ok(Item::Failed(why)),
                }
            }
        }
        Ok(Item::Done {
            frame: compress(&words)?,
            words: words.len() as u32,
            records,
            helpers,
        })
    }
}

/// Translates `programs` for the device `device` describes (the bytes of
/// the runtime's `device.json`) and writes the tables in `dir`, a batch of
/// programs at a time on `jobs` threads: a game's translations run to
/// gigabytes, more than memory holds. Without a device the tables are
/// empty: the game has not run on a device yet.
///
/// `progress` hears the programs done and the programs in all after each
/// batch, and stops the work, leaving the old tables, by answering false.
///
/// # Errors
///
/// A file cannot be read or written, `device` is not capabilities, or
/// `progress` stopped the work.
pub fn translate(
    programs: &Programs,
    device: Option<&[u8]>,
    dir: &Path,
    jobs: usize,
    progress: &(dyn Fn(usize, usize) -> bool + Sync),
) -> Result<Built, BoxError> {
    translate_math(programs, device, dir, jobs, Math::Exact, progress)
}

/// [`translate`] for the float math `math`: the files are those of
/// [`table_file`] and the refusals go to `failed.txt` (exact) or
/// `failed-native.txt`.
///
/// # Errors
///
/// As [`translate`].
pub fn translate_math(
    programs: &Programs,
    device: Option<&[u8]>,
    dir: &Path,
    jobs: usize,
    math: Math,
    progress: &(dyn Fn(usize, usize) -> bool + Sync),
) -> Result<Built, BoxError> {
    translate_traced(programs, device, dir, jobs, math, None, progress)
}

/// [`translate_math`] with the programs `trace` selects in trace mode, a
/// debugging table (see [`TraceSel`]).
///
/// # Errors
///
/// As [`translate`].
pub fn translate_traced(
    programs: &Programs,
    device: Option<&[u8]>,
    dir: &Path,
    jobs: usize,
    math: Math,
    trace: Option<&TraceSel>,
    progress: &(dyn Fn(usize, usize) -> bool + Sync),
) -> Result<Built, BoxError> {
    std::fs::create_dir_all(dir)?;
    let keys = programs.keys();
    let mut failed = String::new();
    let mut refused = BTreeMap::new();
    let mut index: Vec<Frame> = Vec::new();
    let mut interface: Vec<([u8; 32], Vec<[u32; 4]>)> = Vec::new();
    let mut extras: BTreeMap<[u8; 32], (Vec<u8>, u32)> = BTreeMap::new();
    let spool = match math {
        Math::Exact => dir.join(SPOOL),
        m => dir.join(format!("{}.frames", table_file(m, "rlspv"))),
    };
    if let Some(device) = device.filter(|_| !keys.is_empty()) {
        let tr = Translator::with_math(programs, device, math)?.with_trace(trace.cloned());
        let mut spool_file = relift_cache::StreamWriter::new(&spool)?;
        let mut done = 0;
        for batch in keys.chunks(jobs.max(1) * 4) {
            let items = relift_proc::workers::parallel_with(jobs, batch, |&k| {
                tr.item(programs, k).map_err(|e| e.to_string())
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
            done += batch.len();
            if !progress(done, keys.len()) {
                drop(spool_file);
                return Err("stopped".into());
            }
        }
        spool_file.finish()?;
    }
    let failed_name = match math {
        Math::Exact => FAILED.to_string(),
        m => format!("failed-{}.txt", m.name()),
    };
    relift_cache::write_if_changed(&dir.join(failed_name), failed.as_bytes())?;
    let translated = index.len();
    write_table(
        &dir.join(table_file(math, "rlspv")),
        &spool,
        &index,
        &extras,
    )?;
    let _ = std::fs::remove_file(&spool);
    relift_cache::write_if_changed(
        &dir.join(table_file(math, "rlspi")),
        &interface_table(&interface),
    )?;
    match device {
        Some(d) => relift_cache::write_if_changed(&dir.join(DEVICE_FILE), d).map(drop)?,
        None => {
            let _ = std::fs::remove_file(dir.join(DEVICE_FILE));
        }
    }
    write_stamp_of(dir, math)?;
    Ok(Built {
        programs: keys.len(),
        translated,
        refused,
    })
}

/// Writes this translator's [`STAMP`] in `dir`, beside the tables it made.
///
/// # Errors
///
/// The file cannot be written.
pub fn write_stamp(dir: &Path) -> std::io::Result<()> {
    write_stamp_of(dir, Math::Exact)
}

/// Writes the [`stamp`] of `math` in `dir`, beside the tables of that mode.
///
/// # Errors
///
/// The file cannot be written.
pub fn write_stamp_of(dir: &Path, math: Math) -> std::io::Result<()> {
    relift_cache::write_if_changed(
        &dir.join(table_file(math, "stamp")),
        format!("{}\n", stamp(math)).as_bytes(),
    )
    .map(drop)
}

/// The stamp of the tables in `dir`, or none when they have no stamp (made
/// by a relift that wrote none).
#[must_use]
pub fn stamp_of(dir: &Path) -> Option<String> {
    stamp_of_mode(dir, Math::Exact)
}

/// The stamp of the tables of `math` in `dir`, or none without one.
#[must_use]
pub fn stamp_of_mode(dir: &Path, math: Math) -> Option<String> {
    let s = std::fs::read_to_string(dir.join(table_file(math, "stamp"))).ok()?;
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

/// The magic of the interface table: the one writer constant. The runtime
/// (`nvn_shader.c`) opens only this version; a test keeps the two equal.
pub const INTERFACE_MAGIC: &[u8; 8] = b"RLSPI002";

/// What the runtime needs of each translation's interface, all
/// little-endian: the magic `RLSPI002` (`RLSPI001` before storage image array
/// records), the entry count (u32), per entry
/// sorted by key the key (32 bytes), the index of its first record and its
/// record count (u32 each), then the records, four u32 each: the record kind
/// (`RECORD_*`), the constant bank and byte offset of the bound handle, and
/// the specialization constant id (zero where a kind has none).
#[must_use]
pub fn interface_table(entries: &[([u8; 32], Vec<[u32; 4]>)]) -> Vec<u8> {
    let mut head = Vec::new();
    let mut recs: Vec<[u32; 4]> = Vec::new();
    head.extend(INTERFACE_MAGIC);
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
#[must_use]
pub fn records(i: &Interface) -> Vec<[u32; 4]> {
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
    if i.trace_words != 0 {
        recs.push([RECORD_TRACE, i.trace_words, 0, 0]);
    }
    if let Some(d) = i.shared_declared {
        recs.push([RECORD_SHARED_DECLARED, d, i.shared_beyond_declared, 0]);
    }
    let first_image = image_binding(ImageKind::Float, ImageDim::D1);
    for &b in i.texture_bindings.range(first_image..) {
        recs.push([RECORD_IMAGE_BINDING, b, 0, 0]);
    }
    for (k, &(stride, stream)) in i.xfb.iter().enumerate() {
        recs.push([RECORD_XFB, k as u32, stride, stream]);
    }
    if let Some(m) = &i.xfb_memory {
        recs.push([RECORD_XFB_MEMORY, m.record_bytes, m.slots_per_primitive, 0]);
        for (b, sel) in m.select.iter().enumerate() {
            for (w, four) in sel.chunks(4).enumerate() {
                let mut v = [0xff_u8; 4];
                v[..four.len()].copy_from_slice(four);
                recs.push([
                    RECORD_XFB_SELECT,
                    b as u32,
                    4 * w as u32,
                    u32::from_le_bytes(v),
                ]);
            }
        }
    }
    recs
}

#[cfg(test)]
pub(crate) mod tests {
    #[test]
    fn the_interface_table_has_the_magic_the_runtime_opens() {
        assert!(super::interface_table(&[]).starts_with(super::INTERFACE_MAGIC));
        let c = std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../runtime/guests/switch/nvn/nvn_shader.c"
        ))
        .unwrap();
        let magic = std::str::from_utf8(super::INTERFACE_MAGIC).unwrap();
        assert!(
            c.contains(&format!("\"rlspi\", \"{magic}\", true")),
            "the runtime reads the interface table with magic {magic}"
        );
        assert!(!c.contains("\"rlspi\", \"RLSPI001\", true"));
    }

    use super::*;
    use std::path::PathBuf;

    /// A fresh folder for a test under the workspace's target folder.
    pub(crate) fn scratch(tag: &str) -> PathBuf {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../target")
            .join(format!("relift-table-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

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
    fn a_kernel_s_declared_shared_size_is_recorded() {
        let mut i = Interface::default();
        assert!(records(&i).iter().all(|r| r[0] != RECORD_SHARED_DECLARED));
        i.shared_declared = Some(0x6000);
        i.shared_beyond_declared = 4;
        assert!(records(&i).contains(&[RECORD_SHARED_DECLARED, 0x6000, 4, 0]));
        let key = [7u8; 32];
        let json = format!(
            r#"[{{"name": "k", "program": "{}", "shared": 24576}}, {{"name": "x"}}]"#,
            hex(&key)
        );
        let m = kernel_shared(json.as_bytes()).unwrap();
        assert_eq!(m.len(), 1);
        assert_eq!(m.get(&key), Some(&0x6000));
        assert!(kernel_shared(b"").unwrap().is_empty());
    }

    #[test]
    fn a_register_handle_is_recorded_as_bindless() {
        let mut i = Interface::default();
        assert!(records(&i).iter().all(|r| r[0] != RECORD_BINDLESS));
        i.bindless = true;
        assert!(records(&i).contains(&[RECORD_BINDLESS, 0, 0, 0]));
    }

    #[test]
    fn every_storage_image_array_a_program_uses_is_recorded() {
        let mut i = Interface::default();
        let image = image_binding(ImageKind::Raw32, ImageDim::D2);
        i.texture_bindings.extend([1, 31, image]);
        let recs: Vec<_> = records(&i)
            .into_iter()
            .filter(|r| r[0] == RECORD_IMAGE_BINDING)
            .collect();
        assert_eq!(recs, [[RECORD_IMAGE_BINDING, image, 0, 0]]);
        let x = Interface {
            xfb: vec![(16, 0), (24, 1)],
            ..Interface::default()
        };
        assert_eq!(
            records(&x),
            [[RECORD_XFB, 0, 16, 0], [RECORD_XFB, 1, 24, 1]]
        );
        let m = Interface {
            xfb: vec![(24, 0)],
            xfb_memory: Some(relift_gpu_nv::translate::XfbMemory {
                record_bytes: 28,
                slots_per_primitive: 1,
                select: vec![vec![32, 33, 34, 36, 40, 41]],
            }),
            ..Interface::default()
        };
        assert_eq!(
            records(&m)[1..],
            [
                [RECORD_XFB_MEMORY, 28, 1, 0],
                [RECORD_XFB_SELECT, 0, 0, 0x2422_2120],
                [RECORD_XFB_SELECT, 0, 4, 0xffff_2928]
            ]
        );
    }

    #[test]
    fn a_key_is_the_hex_name_of_a_program() {
        let k = hex_key(&format!("{}.bin", "ab".repeat(32))).unwrap();
        assert_eq!(k, [0xab; 32]);
        assert_eq!(hex(&k), "ab".repeat(32));
        assert!(hex_key("ab.bin").is_none());
    }

    const CAPS: &str =
        r#"{"capabilities": {"device": {"extensions": {}, "features": {}, "properties": {}}}}"#;

    fn exit() -> u128 {
        0x14d | (4 << 9) | (7 << 12) | (7 << 87) | (7 << 110) | (7 << 113)
    }

    fn bra_self() -> u128 {
        0x147 | (4 << 9) | (7 << 12) | (0xffff_ffff_fffc << 34) | (7 << 87)
    }

    fn words(ws: &[u128]) -> Vec<u8> {
        ws.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    #[test]
    fn a_pack_and_its_folder_give_the_same_tables_and_progress_can_stop() {
        let d = scratch("same");
        let src = d.join("src");
        std::fs::create_dir_all(src.join("programs")).unwrap();
        for code in [
            words(&[exit(), bra_self()]),
            words(&[exit(), exit(), bra_self()]),
            words(&[0x1ff, exit(), bra_self()]),
        ] {
            relift_cache::write_if_changed(
                &src.join("programs")
                    .join(format!("{}.bin", hex(&sha256(&code)))),
                &code,
            )
            .unwrap();
        }
        let dir = Programs::dir(&src).unwrap();
        let pack = Programs::pack(dir.to_pack(3).unwrap()).unwrap();
        let all = |_: usize, _: usize| true;
        let a = translate(&dir, Some(CAPS.as_bytes()), &d.join("a"), 2, &all).unwrap();
        let b = translate(&pack, Some(CAPS.as_bytes()), &d.join("b"), 1, &all).unwrap();
        assert_eq!((a.programs, a.translated), (3, 2));
        assert_eq!((b.programs, b.translated), (3, 2));
        for f in [TABLE, INTERFACES, STAMP_FILE, DEVICE_FILE, FAILED] {
            assert_eq!(
                std::fs::read(d.join("a").join(f)).unwrap(),
                std::fs::read(d.join("b").join(f)).unwrap(),
                "{f}"
            );
        }
        assert_eq!(
            std::fs::read(d.join("a").join(DEVICE_FILE)).unwrap(),
            CAPS.as_bytes()
        );
        let seen = std::sync::Mutex::new(Vec::new());
        let stop = |done: usize, all: usize| {
            seen.lock().unwrap().push((done, all));
            false
        };
        assert!(translate(&dir, Some(CAPS.as_bytes()), &d.join("c"), 1, &stop).is_err());
        assert_eq!(*seen.lock().unwrap(), [(3, 3)]);
        assert!(
            !d.join("c").join(TABLE).exists(),
            "a stopped run writes no table"
        );
        assert!(!d.join("c").join(SPOOL).exists(), "nor leaves its spool");
        std::fs::remove_dir_all(&d).unwrap();
    }
}
