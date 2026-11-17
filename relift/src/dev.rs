//! The dev server of `relift run` (docs/abi.md section 6, dev builds): the
//! program connects over `RELIFT_DEV_SOCKET` when it misses code, and for
//! each missed address relift lifts the target and everything newly
//! reachable from it, past the entries the build already has, compiles one
//! shared library, adds the target to the hint list in `game.toml` and
//! replies with the library's path.

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

use relift_analyze::{EntryKind, Insn, discover_more};
use relift_build::{Compile, Plan};
use relift_cpu_arm64::cpu::Cpu;
use relift_cpu_arm64::frontend::Arm64;
use relift_guest::Module;
use relift_switch::program::{self, Placed};

use crate::build::BuildTools;
use crate::hints::Hint;
use crate::import::BoxError;

/// Frame magic, "RLDV" little-endian (`RL_DEV_MAGIC`).
pub const MAGIC: u32 = 0x5644_4C52;
/// `RL_DEV_HELLO`.
pub const HELLO: u16 = 1;
/// `RL_DEV_HELLO_REPLY`.
pub const HELLO_REPLY: u16 = 2;
/// `RL_DEV_LIFT`.
pub const LIFT: u16 = 3;
/// `RL_DEV_LIFT_REPLY`.
pub const LIFT_REPLY: u16 = 4;
/// `RL_DEV_OK`.
pub const OK: i32 = 0;
/// `RL_DEV_ERR_INTERNAL`.
pub const ERR_INTERNAL: i32 = 1;
/// `RL_DEV_ERR_HELLO`.
pub const ERR_HELLO: i32 = 2;
/// `RL_DEV_ERR_COMPILE`.
pub const ERR_COMPILE: i32 = 3;

/// The environment variable that names the socket (`RL_DEV_SOCKET_ENV`).
pub const SOCKET_ENV: &str = "RELIFT_DEV_SOCKET";

/// The longest socket path the runtime takes: `sun_path` holds 104 bytes
/// on macOS (108 on Linux), the NUL included.
pub const SOCKET_PATH_MAX: usize = 103;

/// The `RL_ABI_VERSION` a runtime header defines.
///
/// # Errors
///
/// A header that cannot be read or names no version.
pub fn abi_version(header: &Path) -> Result<u32, BoxError> {
    let text = std::fs::read_to_string(header).map_err(|e| format!("{}: {e}", header.display()))?;
    text.lines()
        .find_map(|l| {
            let v = l.trim().strip_prefix("#define RL_ABI_VERSION ")?;
            v.trim().trim_end_matches('u').parse().ok()
        })
        .ok_or_else(|| format!("{} defines no RL_ABI_VERSION", header.display()).into())
}

/// One frame header (`rl_dev_hdr`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    /// The message kind.
    pub kind: u16,
    /// The request's sequence number.
    pub seq: u32,
    /// Payload bytes after the header.
    pub len: u32,
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from(u32_at(b, at)) | (u64::from(u32_at(b, at + 4)) << 32)
}

impl Header {
    /// Parses 16 header bytes.
    ///
    /// # Errors
    ///
    /// The wrong magic.
    pub fn parse(b: &[u8; 16]) -> Result<Header, BoxError> {
        if u32_at(b, 0) != MAGIC {
            return Err(format!("bad frame magic {:#x}", u32_at(b, 0)).into());
        }
        Ok(Header {
            kind: u16_at(b, 4),
            seq: u32_at(b, 8),
            len: u32_at(b, 12),
        })
    }
}

/// A reply frame: `rl_dev_hdr`, `rl_dev_reply`, the path, the message.
pub fn reply(kind: u16, seq: u32, status: i32, path: &str, msg: &str) -> Vec<u8> {
    let payload = 16 + path.len() + msg.len();
    let mut b = Vec::with_capacity(16 + payload);
    b.extend_from_slice(&MAGIC.to_le_bytes());
    b.extend_from_slice(&kind.to_le_bytes());
    b.extend_from_slice(&0u16.to_le_bytes());
    b.extend_from_slice(&seq.to_le_bytes());
    b.extend_from_slice(&u32::try_from(payload).unwrap_or(u32::MAX).to_le_bytes());
    b.extend_from_slice(&status.to_le_bytes());
    b.extend_from_slice(&u32::try_from(path.len()).unwrap_or(0).to_le_bytes());
    b.extend_from_slice(&u32::try_from(msg.len()).unwrap_or(0).to_le_bytes());
    b.extend_from_slice(&0u32.to_le_bytes());
    b.extend_from_slice(path.as_bytes());
    b.extend_from_slice(msg.as_bytes());
    b
}

/// A lift request (`rl_dev_lift_req`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LiftRequest {
    /// The guest address that missed.
    pub target: u64,
    /// The call site, 0 when unknown.
    pub site: u64,
    /// The host thread id.
    pub thread: u64,
    /// The CPU layer's floating-point control value.
    pub fp_mode: u32,
}

impl LiftRequest {
    /// Parses a 32-byte payload.
    pub fn parse(b: &[u8; 32]) -> LiftRequest {
        LiftRequest {
            target: u64_at(b, 0),
            site: u64_at(b, 8),
            thread: u64_at(b, 16),
            fp_mode: u32_at(b, 28),
        }
    }
}

/// What one hot load did, for the log.
#[derive(Clone, Debug)]
pub struct Loaded {
    /// The missed address.
    pub target: u64,
    /// The module and offset.
    pub hint: Hint,
    /// Functions in the library.
    pub functions: usize,
    /// Instructions in them that did not lift.
    pub unlifted: usize,
    /// Milliseconds to lift and emit.
    pub lift_ms: f64,
    /// Milliseconds to compile and link.
    pub compile_ms: f64,
}

/// The dev server's state: the program's modules, every entry lifted so
/// far, and how to build a library.
pub struct Server {
    cpu: Cpu,
    placed: Vec<Placed>,
    modules: Vec<Module>,
    known: Vec<BTreeMap<u64, EntryKind>>,
    image_id: [u8; 32],
    abi: u32,
    tools: BuildTools,
    dir: PathBuf,
    game_toml: PathBuf,
    seq: u32,
    /// Every load so far.
    pub loads: Vec<Loaded>,
}

impl Server {
    /// A server for the build of `placed` for the CPU `cpu` whose entries
    /// are `known` (per module), writing its libraries under `dir`.
    pub fn new(
        cpu: Cpu,
        placed: Vec<Placed>,
        known: Vec<BTreeMap<u64, EntryKind>>,
        abi: u32,
        tools: BuildTools,
        dir: PathBuf,
        game_toml: PathBuf,
    ) -> Server {
        let image_id = crate::build::image_id(&placed);
        let modules = placed.iter().map(program::module).collect();
        Server {
            cpu,
            placed,
            modules,
            known,
            image_id,
            abi,
            tools,
            dir,
            game_toml,
            seq: 0,
            loads: Vec::new(),
        }
    }

    /// Answers a hello payload: `None` when accepted, else why not.
    pub fn hello(&self, b: &[u8; 48]) -> Option<String> {
        let abi = u32_at(b, 0);
        if abi != self.abi {
            return Some(format!(
                "the runtime has ABI version {abi}, relift built for {}",
                self.abi
            ));
        }
        if b[8..40] != self.image_id {
            return Some("the program was built from another image set".into());
        }
        None
    }

    /// Lifts, emits and compiles the code at `target`; returns the
    /// library path.
    ///
    /// # Errors
    ///
    /// A status and why: the target is in no module's code or already
    /// lifted, or the compile failed.
    pub fn lift(&mut self, target: u64) -> Result<PathBuf, (i32, String)> {
        let t0 = Instant::now();
        let i = self
            .modules
            .iter()
            .position(|m| m.is_code(target))
            .ok_or_else(|| {
                (
                    ERR_COMPILE,
                    format!("{target:#x} is not in the code of any module"),
                )
            })?;
        while self.known.len() <= i {
            self.known.push(BTreeMap::new());
        }
        if self.known[i].contains_key(&target) {
            return Err((
                ERR_INTERNAL,
                format!("{target:#x} is already a lifted entry"),
            ));
        }
        let cpu = Arm64 { cpu: self.cpu };
        let module = &self.modules[i];
        let p = &self.placed[i];
        let analysis = discover_more(&cpu, module, &self.known[i], &[target]);
        if !analysis.functions.contains_key(&target) {
            return Err((
                ERR_COMPILE,
                format!("no function could be lifted at {target:#x}"),
            ));
        }
        let raw = |pc: u64| {
            module.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
                u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
            })
        };
        let fns: Vec<relift_emit::Function<'_>> = analysis
            .functions
            .values()
            .map(|f| relift_emit::Function {
                entry: f.entry,
                name: program::host_name(p, f.name.as_deref(), f.entry),
                guest_name: f.name.clone(),
                span: f.span(4),
                insts: f
                    .insts
                    .iter()
                    .map(|(&pc, i)| {
                        let e = match i {
                            Insn::Lifted(b) => relift_emit::Insn::Lifted {
                                block: b,
                                raw: raw(pc),
                            },
                            Insn::Failed(e) => relift_emit::Insn::Failed(e),
                        };
                        (pc, e)
                    })
                    .collect::<BTreeMap<_, _>>(),
                tables: f.tables.clone(),
                pads: f.pads.clone(),
                guesses: Default::default(),
                entries: BTreeMap::new(),
            })
            .collect();
        let overrides: Vec<relift_emit::Override> = analysis
            .functions
            .keys()
            .filter_map(|&a| {
                let k = program::known(module.bytes_at(a)?)?;
                Some(relift_emit::Override {
                    entry: a,
                    host_fn: k.host_fn.into(),
                    may_unwind: k.may_unwind,
                    resume_point: k.resume_point,
                    declare: false,
                })
            })
            .collect();
        self.seq += 1;
        let seq = self.seq;
        let symbol = format!("rl_hot_unit_{seq}");
        let out = relift_emit::emit(
            &cpu,
            &fns,
            &overrides,
            &relift_emit::Options {
                unit_name: format!("{}+hot{seq}", p.name),
                unit_symbol: symbol.clone(),
                module_base: p.base,
                image_id: self.image_id,
                ..relift_emit::Options::default()
            },
        );
        let unlifted = analysis.failures();
        let functions = analysis.functions.len();
        let lift_ms = t0.elapsed().as_secs_f64() * 1e3;
        let t1 = Instant::now();
        let dir = self.dir.join(format!("{seq}"));
        let internal = |e: std::io::Error| (ERR_INTERNAL, format!("{}: {e}", dir.display()));
        std::fs::create_dir_all(&dir).map_err(internal)?;
        let mut srcs = Vec::new();
        for f in &out.files {
            let path = dir.join(&f.name);
            relift_cache::write_if_changed(&path, f.text.as_bytes()).map_err(internal)?;
            if f.name.ends_with(".c") {
                srcs.push(path);
            }
        }
        let export = dir.join("export.c");
        relift_cache::write_if_changed(&export, export_c(&symbol).as_bytes()).map_err(internal)?;
        srcs.push(export);
        let lib = dir.join(format!("hot{seq}{}", lib_suffix()));
        let plan = Plan {
            cc: self.tools.cc.clone(),
            compiles: srcs
                .iter()
                .map(|s| Compile {
                    src: s.clone(),
                    obj: s.with_extension("o"),
                    flags: self.tools.hot_flags.clone(),
                    pool: None,
                    stage: 0,
                })
                .collect(),
            output: lib.clone(),
            link_flags: self.tools.hot_link.clone(),
            pools: Vec::new(),
            root: None,
        };
        relift_build::build_with(
            &plan,
            &dir,
            self.tools.ninja.as_deref(),
            self.tools.jobs,
            &mut |_| {},
        )
        .map_err(|e| (ERR_COMPILE, e.to_string()))?;
        let compile_ms = t1.elapsed().as_secs_f64() * 1e3;
        for f in analysis.functions.values() {
            self.known[i].insert(f.entry, f.kind);
        }
        let hint = Hint {
            module: p.name.clone(),
            offset: target - p.base,
        };
        crate::hints::add(&self.game_toml, std::slice::from_ref(&hint))
            .map_err(|e| (ERR_INTERNAL, e.to_string()))?;
        self.loads.push(Loaded {
            target,
            hint,
            functions,
            unlifted,
            lift_ms,
            compile_ms,
        });
        Ok(lib)
    }

    /// Serves one connection until the program closes it.
    ///
    /// # Errors
    ///
    /// A malformed frame or a broken connection.
    pub fn serve<S: Read + Write>(
        &mut self,
        s: &mut S,
        on: &mut dyn FnMut(&Loaded),
    ) -> Result<(), BoxError> {
        let mut hello = false;
        loop {
            let mut h = [0u8; 16];
            match s.read_exact(&mut h) {
                Ok(()) => {}
                Err(e)
                    if matches!(
                        e.kind(),
                        std::io::ErrorKind::UnexpectedEof | std::io::ErrorKind::ConnectionReset
                    ) =>
                {
                    return Ok(());
                }
                Err(e) => return Err(e.into()),
            }
            let h = Header::parse(&h)?;
            match (h.kind, h.len) {
                (HELLO, 48) => {
                    let mut b = [0u8; 48];
                    s.read_exact(&mut b)?;
                    let why = self.hello(&b);
                    hello = why.is_none();
                    let (st, msg) = why.map_or((OK, String::new()), |w| (ERR_HELLO, w));
                    s.write_all(&reply(HELLO_REPLY, h.seq, st, "", &msg))?;
                }
                (LIFT, 32) => {
                    let mut b = [0u8; 32];
                    s.read_exact(&mut b)?;
                    let r = LiftRequest::parse(&b);
                    let res = if hello {
                        self.lift(r.target)
                    } else {
                        Err((ERR_HELLO, "no hello before the lift request".into()))
                    };
                    let frame = match res {
                        Ok(lib) => {
                            if let Some(l) = self.loads.last() {
                                on(l);
                            }
                            let msg = self
                                .loads
                                .last()
                                .map_or_else(String::new, |l| format!("{} functions", l.functions));
                            reply(LIFT_REPLY, h.seq, OK, &lib.display().to_string(), &msg)
                        }
                        Err((st, msg)) => reply(LIFT_REPLY, h.seq, st, "", &msg),
                    };
                    s.write_all(&frame)?;
                }
                (k, n) => return Err(format!("unexpected frame kind {k} with {n} bytes").into()),
            }
        }
    }
}

/// The file that exports the unit table of a hot-loaded library.
fn export_c(symbol: &str) -> String {
    format!(
        "#include <relift/abi_core.h>\nRL_TEXT_BOUNDS_DEFINE()\nextern const rl_unit {symbol};\nRL_UNIT_EXPORT const rl_unit *relift_unit(void);\nRL_UNIT_EXPORT const rl_unit *relift_unit(void) {{ return &{symbol}; }}\n"
    )
}

fn lib_suffix() -> &'static str {
    if cfg!(windows) {
        ".dll"
    } else if cfg!(target_os = "macos") {
        ".dylib"
    } else {
        ".so"
    }
}

/// A socket path for the dev server that fits `sun_path`: in
/// `XDG_RUNTIME_DIR` when it is set and short enough, else in the build
/// directory `out`, else in the host's temporary directory.
pub fn socket_path(out: &Path) -> PathBuf {
    let name = format!("relift-dev-{}.sock", std::process::id());
    let candidates = [
        std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from),
        Some(out.to_path_buf()),
        Some(std::env::temp_dir()),
    ];
    candidates
        .into_iter()
        .flatten()
        .map(|d| d.join(&name))
        .find(|p| p.as_os_str().len() <= SOCKET_PATH_MAX)
        .unwrap_or_else(|| PathBuf::from(&name))
}

/// Accepts connections on `listener` and serves each with `server` until
/// `stop` is set (the caller then connects once to wake the accept).
pub fn accept_loop(
    listener: &crate::devsock::Listener,
    server: &mut Server,
    stop: &AtomicBool,
    on: &mut dyn FnMut(&Loaded),
    err: &mut dyn FnMut(String),
) {
    while let Ok(mut s) = crate::devsock::accept(listener) {
        if stop.load(Ordering::Acquire) {
            return;
        }
        if let Err(e) = server.serve(&mut s, on) {
            err(e.to_string());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_reply_frame_has_the_abi_layout() {
        let f = reply(LIFT_REPLY, 7, ERR_COMPILE, "/p", "bad");
        assert_eq!(f.len(), 16 + 16 + 2 + 3);
        assert_eq!(&f[0..4], b"RLDV");
        let h = Header::parse(f[..16].try_into().unwrap()).unwrap();
        assert_eq!(
            h,
            Header {
                kind: LIFT_REPLY,
                seq: 7,
                len: 21
            }
        );
        assert_eq!(u32_at(&f, 16), 3);
        assert_eq!(u32_at(&f, 20), 2);
        assert_eq!(u32_at(&f, 24), 3);
        assert_eq!(&f[32..], b"/pbad");
    }

    #[test]
    fn a_lift_request_parses_its_fields() {
        let mut b = [0u8; 32];
        b[0..8].copy_from_slice(&0x0801_70c0_u64.to_le_bytes());
        b[16..24].copy_from_slice(&42u64.to_le_bytes());
        b[24..28].copy_from_slice(&1u32.to_le_bytes());
        b[28..32].copy_from_slice(&0x0300_0000u32.to_le_bytes());
        let r = LiftRequest::parse(&b);
        assert_eq!(
            r,
            LiftRequest {
                target: 0x0801_70c0,
                site: 0,
                thread: 42,
                fp_mode: 0x0300_0000
            }
        );
        assert!(Header::parse(&[0; 16]).is_err());
    }

    #[test]
    fn the_socket_path_fits_sun_path() {
        let deep = PathBuf::from("/").join("x".repeat(200));
        assert!(socket_path(&deep).as_os_str().len() <= SOCKET_PATH_MAX);
    }

    #[test]
    fn the_abi_version_comes_from_the_runtime_header() {
        let h =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/core/include/relift/abi_core.h");
        assert!(abi_version(&h).unwrap() >= 1);
    }
}
