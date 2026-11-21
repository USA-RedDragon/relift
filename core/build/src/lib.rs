//! Toolchain discovery and the Ninja file writer that drive the C compiler.
//!
//! A [`Plan`] is a set of C files to compile, each with its own flags, and
//! one link. [`build`] writes it as `build.ninja` (through relift-cache, so
//! an unchanged plan leaves the file alone) and runs Ninja, which rebuilds
//! only what changed. Without Ninja it compiles every object whose source
//! is newer, one at a time, then links.

use std::fmt::Write as _;
use std::io;
use std::path::{Path, PathBuf};

/// One C translation unit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Compile {
    /// The source file.
    pub src: PathBuf,
    /// The object file.
    pub obj: PathBuf,
    /// The compiler flags, besides `-c`, `-o` and the dependency flags.
    pub flags: Vec<String>,
    /// The Ninja pool that limits how many of these run at once (`None`:
    /// only the job count limits it).
    pub pool: Option<String>,
    /// The stage it belongs to: it starts only when every compile of a
    /// lower stage is done (0: the first stage).
    pub stage: u32,
}

/// What to build.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    /// The C compiler driver (clang, or zig with `cc` as its first flag).
    pub cc: Vec<String>,
    /// The NASM-syntax assembler (its command and leading arguments), which assembles the compiles whose
    /// source is a `.asm` file with their flags; `None` when the plan has none.
    pub asm: Option<Vec<String>>,
    /// The objects.
    pub compiles: Vec<Compile>,
    /// The linked executable.
    pub output: PathBuf,
    /// Link flags after the objects (libraries).
    pub link_flags: Vec<String>,
    /// Ninja pools by name, with their depth.
    pub pools: Vec<(String, u32)>,
    /// The directory the build may move with: paths under it are written
    /// to `build.ninja` relative to the build directory, so the file, the
    /// command lines Ninja hashes and the depfiles read the same wherever
    /// the directory lives. `None` writes every path as given.
    pub root: Option<PathBuf>,
}

/// The memory a compiler needs for a source file, estimated from its size:
/// a fixed start-up cost plus a cost per byte of C, measured with clang 23
/// at `-O2 -gline-tables-only` on lifted code (about 100 bytes of memory per
/// byte of C, rounded up).
pub fn compile_memory(source_bytes: u64) -> u64 {
    const BASE: u64 = 96 << 20;
    const PER_BYTE: u64 = 120;
    BASE.saturating_add(source_bytes.saturating_mul(PER_BYTE))
}

/// The memory the system can give new processes now (`MemAvailable` on
/// Linux), or `None` where it is not known.
pub fn available_memory() -> Option<u64> {
    let t = std::fs::read_to_string("/proc/meminfo").ok()?;
    let line = t.lines().find(|l| l.starts_with("MemAvailable:"))?;
    let kb: u64 = line.split_whitespace().nth(1)?.parse().ok()?;
    Some(kb.saturating_mul(1024))
}

/// One stage of a [`Schedule`]: compiles that run in a pool together.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stage {
    /// Indices into the `needs` given to [`schedule`].
    pub members: Vec<usize>,
    /// How many of them run at once.
    pub depth: u32,
}

/// How to run compiles of estimated memory `needs` in a memory budget.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Schedule {
    /// The jobs to run at once.
    pub jobs: u32,
    /// The stages after the light compiles, smallest first. Stage `k`
    /// starts when the light compiles and stages before it are done.
    pub stages: Vec<Stage>,
}

/// Splits compiles so their memory stays within `budget`. Light compiles
/// (each at most `budget / jobs`) run first, all at once. The rest go in
/// stages by size, each stage holding compiles within a factor of two of
/// each other, run one stage after another with a pool deep enough for the
/// budget to hold its largest members together (at least one, so a compile
/// larger than the budget still runs, alone).
pub fn schedule(needs: &[u64], jobs: u32, budget: u64) -> Schedule {
    let jobs = jobs.max(1);
    let light_max = (budget / u64::from(jobs)).max(1);
    let mut rest: Vec<usize> = (0..needs.len()).filter(|&i| needs[i] > light_max).collect();
    rest.sort_by_key(|&i| needs[i]);
    let mut stages = Vec::new();
    let mut top = light_max;
    while !rest.is_empty() {
        top = top.saturating_mul(2);
        let n = rest.iter().take_while(|&&i| needs[i] <= top).count();
        if n == 0 {
            continue;
        }
        let members: Vec<usize> = rest.drain(..n).collect();
        let largest = members.iter().map(|&i| needs[i]).max().unwrap_or(1).max(1);
        let depth = u32::try_from(budget / largest)
            .unwrap_or(u32::MAX)
            .clamp(1, jobs);
        stages.push(Stage { members, depth });
    }
    Schedule { jobs, stages }
}

/// Why a build failed.
#[derive(Debug)]
pub enum Error {
    /// A file could not be written or a tool could not be started.
    Io(io::Error),
    /// A tool ran and failed.
    Failed(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Io(e) => write!(f, "{e}"),
            Error::Failed(m) => write!(f, "{m}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Error {
        Error::Io(e)
    }
}

/// Quotes `s` for a Ninja command line (POSIX shell rules).
fn sh(s: &str) -> String {
    if !s.is_empty()
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"-_./=+,:@%".contains(&b))
    {
        return s.to_string();
    }
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// Escapes a path for a Ninja build line.
fn nj(p: &Path) -> String {
    p.display()
        .to_string()
        .replace('$', "$$")
        .replace(' ', "$ ")
        .replace(':', "$:")
}

/// `p` relative to `dir` when both are under `root`, else `p` as given.
fn relative(root: &Path, dir: &Path, p: &Path) -> PathBuf {
    if !p.is_absolute() || !p.starts_with(root) || !dir.starts_with(root) {
        return p.to_path_buf();
    }
    let mut a = dir.components().peekable();
    let mut b = p.components().peekable();
    while let (Some(x), Some(y)) = (a.peek(), b.peek()) {
        if x != y {
            break;
        }
        a.next();
        b.next();
    }
    let mut out = PathBuf::new();
    for _ in a {
        out.push("..");
    }
    out.extend(b);
    if out.as_os_str().is_empty() {
        out.push(".");
    }
    out
}

/// Whether a folder from `root` down to `dir` is a symbolic link, so `dir`
/// really sits somewhere else. A folder that does not exist yet is no link.
fn linked(root: &Path, dir: &Path) -> bool {
    let Ok(rest) = dir.strip_prefix(root) else {
        return false;
    };
    let mut at = root.to_path_buf();
    for c in rest.components() {
        at.push(c);
        if std::fs::symlink_metadata(&at).is_ok_and(|m| m.file_type().is_symlink()) {
            return true;
        }
    }
    false
}

/// A flag whose value is a path: the path moved by `relative`.
fn relative_flag(root: &Path, dir: &Path, flag: &str) -> String {
    for prefix in ["", "-I", "-L"] {
        if let Some(rest) = flag.strip_prefix(prefix) {
            let p = Path::new(rest);
            if p.is_absolute() && p.starts_with(root) {
                return format!("{prefix}{}", relative(root, dir, p).display());
            }
        }
    }
    flag.to_string()
}

/// How the paths of a plan are written: moved to be relative to the build
/// directory when it has a root.
struct Paths<'a> {
    base: Option<(&'a Path, &'a Path)>,
}

impl<'a> Paths<'a> {
    /// The paths of a plan with `root` run in `dir`. A `..` climbs the
    /// folders `dir` really sits in, so when one between `root` and `dir` is
    /// a symbolic link, the paths stay absolute.
    fn new(root: Option<&'a Path>, dir: &'a Path) -> Self {
        Paths {
            base: root.filter(|r| !linked(r, dir)).map(|r| (r, dir)),
        }
    }

    fn path(&self, p: &Path) -> PathBuf {
        match self.base {
            Some((root, dir)) => relative(root, dir, p),
            None => p.to_path_buf(),
        }
    }

    fn flag(&self, f: &str) -> String {
        match self.base {
            Some((root, dir)) => relative_flag(root, dir, f),
            None => f.to_string(),
        }
    }
}

/// Escapes `$` in a Ninja variable value.
fn nv(s: &str) -> String {
    s.replace('$', "$$")
}

/// Whether the link of `cc` writes a Mach-O file (macOS or iOS): by the
/// triple the compiler is given with `-target` or `--target=`, else by one
/// in the compiler's own name (an NDK `<triple>-clang`), else by the host.
fn links_mach_o(cc: &[String]) -> bool {
    let apple = |t: &str| {
        ["apple", "macos", "ios", "darwin"]
            .iter()
            .any(|k| t.contains(k))
    };
    let mut target = None;
    for (i, a) in cc.iter().enumerate() {
        if let Some(t) = a.strip_prefix("--target=") {
            target = Some(t);
        } else if a == "-target" {
            target = cc.get(i + 1).map(String::as_str);
        }
    }
    if let Some(t) = target {
        return apple(t);
    }
    let name = cc
        .first()
        .and_then(|c| Path::new(c).file_name())
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    if ["linux", "android", "windows", "mingw"]
        .iter()
        .any(|k| name.contains(k))
    {
        return false;
    }
    cfg!(target_vendor = "apple")
}

/// The link flag that keeps the build directory out of a Mach-O program:
/// the linker records every object's absolute path in the program's debug
/// map (`N_OSO`), and `-oso_prefix` takes `prefix` off them, leaving paths
/// relative to the build directory. None for other formats and for a plan
/// without a root, whose paths are absolute as given.
fn debug_map_flag(plan: &Plan, prefix: &str) -> Option<String> {
    (plan.root.is_some() && links_mach_o(&plan.cc)).then(|| format!("-Wl,-oso_prefix,{prefix}"))
}

/// Whether `src` is NASM-syntax assembly, assembled by the plan's assembler instead of the C compiler.
fn is_asm(src: &Path) -> bool {
    src.extension().is_some_and(|x| x == "asm")
}

/// An include flag of NASM, which joins the directory and a file name without a separator, with the
/// trailing `/` it needs; any other flag as it is.
fn nasm_dir(flag: &str) -> String {
    match flag.strip_prefix("-I") {
        Some(d) if !d.is_empty() && !d.ends_with('/') => format!("-I{d}/"),
        _ => flag.to_string(),
    }
}

/// The text of `build.ninja` for `plan` as run in `dir`. The link reads its objects from a
/// response file: ninja runs each command as one `sh -c` string, which
/// Linux caps at 128 KiB, and a game's thousands of shard objects pass it.
pub fn ninja_file(plan: &Plan, dir: &Path) -> String {
    let at = Paths::new(plan.root.as_deref(), dir);
    let p = |x: &Path| at.path(x);
    let f = |x: &String| at.flag(x);
    let debug_dir = if plan.root.is_some() {
        " -fdebug-compilation-dir=."
    } else {
        ""
    };
    let oso = debug_map_flag(plan, ".").map_or(String::new(), |f| format!(" {}", sh(&f)));
    let cc: Vec<String> = plan.cc.iter().map(|s| sh(s)).collect();
    let asm_rule = plan.asm.as_ref().map_or(String::new(), |a| {
        let a: Vec<String> = a.iter().map(|s| sh(s)).collect();
        format!(
            "asm = {}\n\nrule asm\n  command = $asm $flags -MQ $out -MF $out.d -o $out $in\n  depfile = $out.d\n  deps = gcc\n  description = asm $in\n\n",
            a.join(" ")
        )
    });
    let mut t = format!(
        "ninja_required_version = 1.3\ncc = {}\n\nrule cc\n  command = $cc $flags{debug_dir} -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = cc $in\n\nrule link\n  command = $cc @$out.rsp{oso} -o $out $libs\n  rspfile = $out.rsp\n  rspfile_content = $in\n  description = link $out\n\n{asm_rule}",
        cc.join(" ")
    );
    // Stage `k` waits (order-only) on the phony `stage<k-1>`, which stands
    // for every object of stages up to `k - 1`.
    let last = plan.compiles.iter().map(|c| c.stage).max().unwrap_or(0);
    for k in 0..last {
        let mut objs: Vec<String> = plan
            .compiles
            .iter()
            .filter(|c| c.stage == k)
            .map(|c| nj(&p(&c.obj)))
            .collect();
        if k > 0 {
            objs.push(format!("stage{}", k - 1));
        }
        let _ = writeln!(t, "build stage{k}: phony {}\n", objs.join(" "));
    }
    for (name, depth) in &plan.pools {
        let _ = writeln!(t, "pool {name}\n  depth = {depth}\n");
    }
    for c in &plan.compiles {
        let asm = is_asm(&c.src);
        let flags: Vec<String> = c
            .flags
            .iter()
            .map(|s| {
                let s = f(s);
                nv(&sh(&if asm { nasm_dir(&s) } else { s }))
            })
            .collect();
        let after = if c.stage > 0 {
            format!(" || stage{}", c.stage - 1)
        } else {
            String::new()
        };
        let rule = if is_asm(&c.src) { "asm" } else { "cc" };
        let _ = writeln!(
            t,
            "build {}: {rule} {}{after}\n  flags = {}",
            nj(&p(&c.obj)),
            nj(&p(&c.src)),
            flags.join(" ")
        );
        if let Some(p) = &c.pool {
            let _ = writeln!(t, "  pool = {p}");
        }
    }
    let objs: Vec<String> = plan.compiles.iter().map(|c| nj(&p(&c.obj))).collect();
    let libs: Vec<String> = plan.link_flags.iter().map(|s| nv(&sh(&f(s)))).collect();
    let _ = writeln!(
        t,
        "\nbuild {}: link {}\n  libs = {}\n\ndefault {}",
        nj(&p(&plan.output)),
        objs.join(" "),
        libs.join(" "),
        nj(&p(&plan.output))
    );
    t
}

/// What a running build reports.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event<'a> {
    /// Time passed (about every 200 ms) with nothing finished.
    Tick,
    /// A step finished: `done` of `total` steps of this build so far,
    /// where `what` is the step's description, such as `cc path/shard0001.c`.
    Step {
        /// Steps finished.
        done: usize,
        /// Steps in this build.
        total: usize,
        /// The step's description.
        what: &'a str,
    },
}

/// How many steps run at once for the `jobs` setting (0: one per CPU).
pub fn job_count(jobs: u32) -> u32 {
    if jobs > 0 {
        return jobs;
    }
    std::thread::available_parallelism().map_or(1, |n| u32::try_from(n.get()).unwrap_or(u32::MAX))
}

const POLL: std::time::Duration = std::time::Duration::from_millis(200);
const TAIL_LINES: usize = 40;

/// The Ninja status line `@done/total/running text`, split.
fn status(line: &str) -> Option<(usize, usize, &str)> {
    let rest = line.strip_prefix('@')?;
    let (head, what) = rest.split_once(' ')?;
    let mut nums = head.split('/');
    let done = nums.next()?.parse().ok()?;
    let total = nums.next()?.parse().ok()?;
    nums.next()?.parse::<usize>().ok()?;
    Some((done, total, what))
}

/// Runs `cmd`, feeding each output line to `line` (which returns whether it
/// used the line) and telling `on` about time passing. The unused lines are
/// the failure report.
fn run(
    cmd: &mut std::process::Command,
    what: &str,
    on: &mut dyn FnMut(Event),
    mut line: impl FnMut(&str, &mut dyn FnMut(Event)) -> bool,
) -> Result<(), Error> {
    use std::io::BufRead as _;
    use std::process::Stdio;
    use std::sync::mpsc::{RecvTimeoutError, channel};

    let mut child = cmd
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    let (tx, rx) = channel::<String>();
    let mut readers = Vec::new();
    if let Some(out) = child.stdout.take() {
        let tx = tx.clone();
        readers.push(std::thread::Builder::new().spawn(move || {
            for l in io::BufReader::new(out).lines().map_while(Result::ok) {
                if tx.send(l).is_err() {
                    break;
                }
            }
        })?);
    }
    if let Some(err) = child.stderr.take() {
        let tx = tx.clone();
        readers.push(std::thread::Builder::new().spawn(move || {
            for l in io::BufReader::new(err).lines().map_while(Result::ok) {
                if tx.send(l).is_err() {
                    break;
                }
            }
        })?);
    }
    drop(tx);
    let mut tail: std::collections::VecDeque<String> = std::collections::VecDeque::new();
    loop {
        match rx.recv_timeout(POLL) {
            Ok(l) => {
                if !line(&l, on) {
                    if tail.len() == TAIL_LINES {
                        tail.pop_front();
                    }
                    tail.push_back(l);
                }
            }
            Err(RecvTimeoutError::Timeout) => on(Event::Tick),
            Err(RecvTimeoutError::Disconnected) => break,
        }
    }
    for r in readers {
        let _ = r.join();
    }
    let status = child.wait()?;
    if status.success() {
        return Ok(());
    }
    let mut msg = format!("{what} failed ({status})");
    for l in tail {
        msg.push('\n');
        msg.push_str(&l);
    }
    Err(Error::Failed(msg))
}

/// Compiles `stale` with up to `workers` compiler processes at once and
/// tells `on` about each finished file; the first failure stops the
/// rest and is the error. This is the build without Ninja.
fn compile_parallel(
    prog: &str,
    pre: &[String],
    asm: Option<&[String]>,
    stale: &[&Compile],
    workers: usize,
    total: usize,
    on: &mut dyn FnMut(Event),
) -> Result<(), Error> {
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::sync::mpsc::{RecvTimeoutError, channel};

    let next = AtomicUsize::new(0);
    let stop = AtomicBool::new(false);
    let (tx, rx) = channel::<Result<usize, Error>>();
    let work = |tx: std::sync::mpsc::Sender<Result<usize, Error>>| {
        loop {
            let i = next.fetch_add(1, Ordering::Relaxed);
            let Some(c) = stale.get(i) else { break };
            if stop.load(Ordering::Relaxed) {
                break;
            }
            let mut cmd = match (asm, is_asm(&c.src)) {
                (Some([a, rest @ ..]), true) => {
                    let mut cmd = relift_proc::command(a);
                    cmd.args(rest).args(&c.flags);
                    cmd
                }
                _ => {
                    let mut cmd = relift_proc::command(prog);
                    cmd.args(pre).args(&c.flags).arg("-c");
                    cmd
                }
            };
            let r = cmd
                .arg(&c.src)
                .arg("-o")
                .arg(&c.obj)
                .stdin(std::process::Stdio::null())
                .output();
            let r = match r {
                Ok(o) if o.status.success() => Ok(i),
                Ok(o) => {
                    let mut msg = format!("cc {} failed ({})", c.src.display(), o.status);
                    for l in String::from_utf8_lossy(&o.stderr)
                        .lines()
                        .chain(String::from_utf8_lossy(&o.stdout).lines())
                        .rev()
                        .take(TAIL_LINES)
                        .collect::<Vec<_>>()
                        .into_iter()
                        .rev()
                    {
                        msg.push('\n');
                        msg.push_str(l);
                    }
                    Err(Error::Failed(msg))
                }
                Err(e) => Err(e.into()),
            };
            let failed = r.is_err();
            if tx.send(r).is_err() || failed {
                break;
            }
        }
    };
    std::thread::scope(|sc| {
        let work = &work;
        let mut started = 0;
        for _ in 0..workers.clamp(1, stale.len().max(1)) {
            let tx = tx.clone();
            if std::thread::Builder::new()
                .spawn_scoped(sc, move || work(tx))
                .is_err()
            {
                break;
            }
            started += 1;
        }
        if started == 0 {
            work(tx.clone());
        }
        drop(tx);
        let mut done = 0;
        let mut result = Ok(());
        loop {
            match rx.recv_timeout(POLL) {
                Ok(Ok(i)) => {
                    done += 1;
                    on(Event::Step {
                        done,
                        total,
                        what: &format!("cc {}", stale[i].src.display()),
                    });
                }
                Ok(Err(e)) => {
                    stop.store(true, Ordering::Relaxed);
                    if result.is_ok() {
                        result = Err(e);
                    }
                }
                Err(RecvTimeoutError::Timeout) => on(Event::Tick),
                Err(RecvTimeoutError::Disconnected) => break,
            }
        }
        result
    })
}

fn newer(a: &Path, b: &Path) -> bool {
    let m = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
    match (m(a), m(b)) {
        (Some(x), Some(y)) => x > y,
        _ => true,
    }
}

/// Builds `plan` in `dir` with `jobs` parallel jobs (0: one per CPU),
/// through Ninja at `ninja` when given, else directly.
///
/// # Errors
///
/// A write error, a tool that cannot start, or a failed compile or link
/// (with the end of its output).
pub fn build(plan: &Plan, dir: &Path, ninja: Option<&Path>, jobs: u32) -> Result<(), Error> {
    build_with(plan, dir, ninja, jobs, &mut |_| {})
}

/// As [`build`], telling `on` about every finished step. With Ninja the
/// counts are Ninja's own (`NINJA_STATUS`); without it they count the
/// objects compiled and the link.
///
/// # Errors
///
/// As [`build`].
pub fn build_with(
    plan: &Plan,
    dir: &Path,
    ninja: Option<&Path>,
    jobs: u32,
    on: &mut dyn FnMut(Event),
) -> Result<(), Error> {
    std::fs::create_dir_all(dir)?;
    for c in &plan.compiles {
        if let Some(p) = c.obj.parent() {
            std::fs::create_dir_all(p)?;
        }
    }
    if let Some(n) = ninja {
        relift_cache::write_if_changed(&dir.join("build.ninja"), ninja_file(plan, dir).as_bytes())?;
        let mut cmd = relift_proc::command(n);
        cmd.current_dir(dir)
            .env("NINJA_STATUS", "@%f/%t/%r ")
            .arg("-j")
            .arg(job_count(jobs).to_string());
        return run(&mut cmd, "ninja", on, |l, on| match status(l) {
            Some((done, total, what)) => {
                on(Event::Step { done, total, what });
                true
            }
            None => false,
        });
    }
    let (prog, pre) = plan
        .cc
        .split_first()
        .ok_or_else(|| Error::Failed("no C compiler".into()))?;
    let stale: Vec<&Compile> = plan
        .compiles
        .iter()
        .filter(|c| newer(&c.src, &c.obj))
        .collect();
    let relink = !stale.is_empty() || !plan.output.exists();
    let total = stale.len() + usize::from(relink);
    compile_parallel(
        prog,
        pre,
        plan.asm.as_deref(),
        &stale,
        job_count(jobs) as usize,
        total,
        on,
    )?;
    if relink {
        let mut cmd = relift_proc::command(prog);
        cmd.args(pre)
            .args(plan.compiles.iter().map(|c| &c.obj))
            .arg("-o")
            .arg(&plan.output)
            .args(&plan.link_flags)
            .args(debug_map_flag(plan, &dir.display().to_string()));
        run(&mut cmd, "link", on, |_, _| false)?;
        on(Event::Step {
            done: total,
            total,
            what: &format!("link {}", plan.output.display()),
        });
    }
    Ok(())
}

/// Finds `name` on `PATH`, with the host's executable suffix when it has
/// one (`.exe` on Windows).
pub fn find_tool(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    let file = format!("{name}{}", std::env::consts::EXE_SUFFIX);
    std::env::split_paths(&path)
        .map(|d| d.join(&file))
        .find(|p| p.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plan() -> Plan {
        Plan {
            cc: vec!["clang".into()],
            asm: None,
            compiles: vec![Compile {
                src: "c/a b.c".into(),
                obj: "obj/a.o".into(),
                flags: vec!["-O2".into(), "-DX=\"y\"".into()],
                pool: None,
                stage: 0,
            }],
            output: "game".into(),
            link_flags: vec!["-lm".into()],
            pools: Vec::new(),
            root: None,
        }
    }

    #[test]
    fn pools_are_declared_and_used() {
        let mut p = plan();
        p.pools = vec![("heavy".into(), 2)];
        p.compiles[0].pool = Some("heavy".into());
        let t = ninja_file(&p, Path::new("."));
        assert!(t.contains("pool heavy\n  depth = 2\n"), "{t}");
        assert!(t.contains("  pool = heavy\n"), "{t}");
        let mut b = p.compiles[0].clone();
        b.obj = "obj/b.o".into();
        b.stage = 1;
        p.compiles.push(b);
        let t = ninja_file(&p, Path::new("."));
        assert!(t.contains("build stage0: phony obj/a.o\n"), "{t}");
        assert!(t.contains("build obj/b.o: cc c/a$ b.c || stage0\n"), "{t}");
    }

    #[test]
    fn asm_sources_go_to_the_assembler() {
        let mut p = plan();
        p.asm = Some(vec!["nasm".into(), "-w-all".into()]);
        p.compiles.push(Compile {
            src: "s/x86/mc.asm".into(),
            obj: "obj/mc.o".into(),
            flags: vec!["-f".into(), "elf64".into(), "-Is/".into()],
            pool: None,
            stage: 0,
        });
        let t = ninja_file(&p, Path::new("."));
        assert!(t.contains("asm = nasm -w-all\n"), "{t}");
        assert!(
            t.contains("rule asm\n  command = $asm $flags -MQ $out -MF $out.d -o $out $in\n"),
            "{t}"
        );
        assert!(
            t.contains("build obj/mc.o: asm s/x86/mc.asm\n  flags = -f elf64 -Is/\n"),
            "{t}"
        );
        assert!(t.contains("build obj/a.o: cc c/a$ b.c"), "{t}");
        p.asm = None;
        assert!(!ninja_file(&p, Path::new(".")).contains("rule asm"));
    }

    fn moved(root: &str) -> (Plan, PathBuf) {
        let at = |p: &str| Path::new(root).join(p);
        let dir = at("projects/g/build/host");
        let plan = Plan {
            cc: vec!["clang".into()],
            asm: None,
            compiles: vec![Compile {
                src: dir.join("c/u.c"),
                obj: dir.join("obj/u.o"),
                flags: vec![
                    "-O2".into(),
                    format!(
                        "-I{}",
                        at("projects/g/build/host/runtime/core/include").display()
                    ),
                    "-I/usr/include/SDL3".into(),
                ],
                pool: None,
                stage: 0,
            }],
            output: dir.join("game"),
            link_flags: vec![
                dir.join("rt/librelift_rt.so").display().to_string(),
                "-Wl,-rpath,$ORIGIN/rt".into(),
            ],
            pools: Vec::new(),
            root: Some(root.into()),
        };
        (plan, dir)
    }

    #[test]
    fn mach_o_links_keep_the_build_directory_out_of_the_debug_map() {
        let cc = |a: &[&str]| a.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert!(links_mach_o(&cc(&[
            "xcrun",
            "-target",
            "arm64-apple-ios17.0"
        ])));
        assert!(links_mach_o(&cc(&[
            "clang",
            "--target=x86_64-apple-macos13"
        ])));
        assert!(!links_mach_o(&cc(&[
            "zig",
            "cc",
            "-target",
            "x86_64-windows-gnu"
        ])));
        assert!(!links_mach_o(&cc(&[
            "zig",
            "cc",
            "-target",
            "aarch64-linux-gnu.2.31"
        ])));
        assert!(!links_mach_o(&cc(&[
            "/ndk/bin/aarch64-linux-android24-clang"
        ])));
        assert_eq!(links_mach_o(&cc(&["clang"])), cfg!(target_vendor = "apple"));
        let (mut p, d) = moved("/c");
        p.cc = cc(&["clang", "-target", "arm64-apple-ios17.0"]);
        let t = ninja_file(&p, &d);
        assert!(
            t.contains("command = $cc @$out.rsp -Wl,-oso_prefix,. -o $out $libs"),
            "{t}"
        );
        assert_eq!(
            debug_map_flag(&p, "/c/b").as_deref(),
            Some("-Wl,-oso_prefix,/c/b")
        );
        p.root = None;
        assert!(!ninja_file(&p, &d).contains("oso_prefix"));
        let (mut p, d) = moved("/c");
        p.cc = cc(&["zig", "cc", "-target", "x86_64-linux-gnu"]);
        assert!(!ninja_file(&p, &d).contains("oso_prefix"));
    }

    #[test]
    fn paths_under_the_root_do_not_depend_on_where_it_is() {
        let (a, da) = moved("/home/a/cache");
        let (b, db) = moved("/mnt/other place/cache");
        let (ta, tb) = (ninja_file(&a, &da), ninja_file(&b, &db));
        assert_eq!(ta, tb);
        assert!(!ta.contains("/home/a"), "{ta}");
        assert!(ta.contains("build obj/u.o: cc c/u.c\n"), "{ta}");
        assert!(ta.contains("-Iruntime/core/include"), "{ta}");
        assert!(ta.contains("-I/usr/include/SDL3"), "{ta}");
        assert!(ta.contains("-fdebug-compilation-dir=."), "{ta}");
        assert!(
            ta.contains(
                "build game: link obj/u.o\n  libs = rt/librelift_rt.so '-Wl,-rpath,$$ORIGIN/rt'"
            ),
            "{ta}"
        );
    }

    #[test]
    fn relative_paths_climb_out_of_the_build_directory() {
        let root = Path::new("/c");
        let dir = Path::new("/c/projects/g/build/host/rt");
        assert_eq!(
            relative(root, dir, Path::new("/c/projects/g/build/host/obj/u.o")),
            Path::new("../obj/u.o")
        );
        assert_eq!(
            relative(root, dir, Path::new("/c/gb-runtime/src")),
            Path::new("../../../../../gb-runtime/src")
        );
        assert_eq!(relative(root, dir, dir), Path::new("."));
        assert_eq!(
            relative(root, dir, Path::new("/usr/lib")),
            Path::new("/usr/lib")
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_build_directory_behind_a_link_keeps_paths_outside_it_whole() {
        let base = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../target/tmp")
            .join(format!("relative-link-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let root = base.join("cache");
        let real = base.join("elsewhere").join("g");
        std::fs::create_dir_all(real.join("build/host")).unwrap();
        std::fs::create_dir_all(root.join("projects")).unwrap();
        std::fs::create_dir_all(root.join("sdk/lib")).unwrap();
        std::os::unix::fs::symlink(&real, root.join("projects/g")).unwrap();
        let dir = root.join("projects/g/build/host");
        let sdk = root.join("sdk/lib/a.c");
        assert_eq!(Paths::new(Some(&root), &dir).path(&sdk), sdk);
        let plain = root.join("projects/h/build/host");
        assert_eq!(
            Paths::new(Some(&root), &plain).path(&sdk),
            Path::new("../../../../sdk/lib/a.c")
        );
        std::fs::remove_dir_all(&base).unwrap();
    }

    #[test]
    fn schedule_stages_large_compiles_by_size() {
        const G: u64 = 1 << 30;
        // 16 jobs in 16 GiB: light compiles are at most 1 GiB.
        let needs = [G / 4, 3 * G, G / 8, 4 * G, G / 2, 40 * G, 3 * G / 2];
        let s = schedule(&needs, 16, 16 * G);
        assert_eq!(s.jobs, 16);
        let got: Vec<(Vec<usize>, u32)> = s
            .stages
            .iter()
            .map(|g| (g.members.clone(), g.depth))
            .collect();
        assert_eq!(
            got,
            vec![(vec![6], 10), (vec![1, 3], 4), (vec![5], 1)],
            "{s:?}"
        );
        assert!(schedule(&[G / 8; 4], 4, 16 * G).stages.is_empty());
    }

    #[test]
    fn compile_memory_grows_with_the_source() {
        assert!(compile_memory(8 << 20) > compile_memory(1 << 20));
        assert!(compile_memory(8 << 20) >= 900 << 20);
    }

    #[test]
    fn ninja_file_escapes_paths_and_quotes_flags() {
        let t = ninja_file(&plan(), Path::new("."));
        assert!(
            t.contains("command = $cc @$out.rsp -o $out $libs\n  rspfile = $out.rsp\n  rspfile_content = $in\n"),
            "{t}"
        );
        assert!(
            t.contains("build obj/a.o: cc c/a$ b.c\n  flags = -O2 '-DX=\"y\"'"),
            "{t}"
        );
        assert!(t.contains("build game: link obj/a.o\n  libs = -lm"), "{t}");
        assert!(t.contains("deps = gcc"));
    }

    #[test]
    fn ninja_status_lines_split_and_other_lines_do_not() {
        assert_eq!(status("@12/340/8 cc a b.c"), Some((12, 340, "cc a b.c")));
        assert_eq!(status("@1/2/3 link game"), Some((1, 2, "link game")));
        assert_eq!(status("FAILED: x.o"), None);
        assert_eq!(status("@x/2/3 cc"), None);
        assert_eq!(status("@1/2 cc"), None);
        assert_eq!(status("ninja: no work to do."), None);
    }

    #[test]
    fn explicit_jobs_are_kept_and_zero_means_the_cpus() {
        assert_eq!(job_count(7), 7);
        assert!(job_count(0) >= 1);
    }

    #[test]
    fn shell_quoting() {
        assert_eq!(sh("plain-1.c"), "plain-1.c");
        assert_eq!(sh("it's"), "'it'\\''s'");
        assert_eq!(sh(""), "''");
    }
}
