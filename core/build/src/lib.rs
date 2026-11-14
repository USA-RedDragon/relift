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
    /// The objects.
    pub compiles: Vec<Compile>,
    /// The linked executable.
    pub output: PathBuf,
    /// Link flags after the objects (libraries).
    pub link_flags: Vec<String>,
    /// Ninja pools by name, with their depth.
    pub pools: Vec<(String, u32)>,
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

/// The text of `build.ninja` for `plan`. The link reads its objects from a
/// response file: ninja runs each command as one `sh -c` string, which
/// Linux caps at 128 KiB, and a game's thousands of shard objects pass it.
pub fn ninja_file(plan: &Plan) -> String {
    let cc: Vec<String> = plan.cc.iter().map(|s| sh(s)).collect();
    let mut t = format!(
        "ninja_required_version = 1.3\ncc = {}\n\nrule cc\n  command = $cc $flags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = cc $in\n\nrule link\n  command = $cc @$out.rsp -o $out $libs\n  rspfile = $out.rsp\n  rspfile_content = $in\n  description = link $out\n\n",
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
            .map(|c| nj(&c.obj))
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
        let flags: Vec<String> = c.flags.iter().map(|s| sh(s)).collect();
        let after = if c.stage > 0 {
            format!(" || stage{}", c.stage - 1)
        } else {
            String::new()
        };
        let _ = writeln!(
            t,
            "build {}: cc {}{after}\n  flags = {}",
            nj(&c.obj),
            nj(&c.src),
            flags.join(" ")
        );
        if let Some(p) = &c.pool {
            let _ = writeln!(t, "  pool = {p}");
        }
    }
    let objs: Vec<String> = plan.compiles.iter().map(|c| nj(&c.obj)).collect();
    let libs: Vec<String> = plan.link_flags.iter().map(|s| sh(s)).collect();
    let _ = writeln!(
        t,
        "\nbuild {}: link {}\n  libs = {}\n\ndefault {}",
        nj(&plan.output),
        objs.join(" "),
        libs.join(" "),
        nj(&plan.output)
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
        readers.push(std::thread::spawn(move || {
            for l in io::BufReader::new(out).lines().map_while(Result::ok) {
                if tx.send(l).is_err() {
                    break;
                }
            }
        }));
    }
    if let Some(err) = child.stderr.take() {
        let tx = tx.clone();
        readers.push(std::thread::spawn(move || {
            for l in io::BufReader::new(err).lines().map_while(Result::ok) {
                if tx.send(l).is_err() {
                    break;
                }
            }
        }));
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
    std::thread::scope(|sc| {
        for _ in 0..workers.clamp(1, stale.len().max(1)) {
            let tx = tx.clone();
            let (next, stop) = (&next, &stop);
            sc.spawn(move || {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(c) = stale.get(i) else { break };
                    if stop.load(Ordering::Relaxed) {
                        break;
                    }
                    let r = relift_proc::command(prog)
                        .args(pre)
                        .args(&c.flags)
                        .arg("-c")
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
            });
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
        relift_cache::write_if_changed(&dir.join("build.ninja"), ninja_file(plan).as_bytes())?;
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
    compile_parallel(prog, pre, &stale, job_count(jobs) as usize, total, on)?;
    if relink {
        let mut cmd = relift_proc::command(prog);
        cmd.args(pre)
            .args(plan.compiles.iter().map(|c| &c.obj))
            .arg("-o")
            .arg(&plan.output)
            .args(&plan.link_flags);
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
        }
    }

    #[test]
    fn pools_are_declared_and_used() {
        let mut p = plan();
        p.pools = vec![("heavy".into(), 2)];
        p.compiles[0].pool = Some("heavy".into());
        let t = ninja_file(&p);
        assert!(t.contains("pool heavy\n  depth = 2\n"), "{t}");
        assert!(t.contains("  pool = heavy\n"), "{t}");
        let mut b = p.compiles[0].clone();
        b.obj = "obj/b.o".into();
        b.stage = 1;
        p.compiles.push(b);
        let t = ninja_file(&p);
        assert!(t.contains("build stage0: phony obj/a.o\n"), "{t}");
        assert!(t.contains("build obj/b.o: cc c/a$ b.c || stage0\n"), "{t}");
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
        let t = ninja_file(&plan());
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
