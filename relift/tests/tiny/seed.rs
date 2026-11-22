//! A shared seed for the build directories of the end-to-end tests.
//!
//! Every test builds its program in a cache directory of its own, and the
//! C runtime (about 180 files) is most of what a build compiles, so a suite of
//! builds spends its time compiling the same runtime again and again. The first
//! successful `build`, `run` or `diff` of a project saves its build directory (the
//! object files, the runtime copy and ninja's state, not the C of the program
//! itself) under the target's scratch directory; every later build of a
//! project of that kind starts from a copy, and one that starts while the
//! first is still building waits for it instead of compiling the runtime
//! again. Ninja still decides what is out of date, so a changed flag, source
//! or compiler rebuilds exactly what changed and the test sees the same build
//! it would from nothing.
//!
//! `RELIFT_TEST_SEED=off` in the environment turns it off, and a test can
//! pass that pair to `tiny::relift_env` to build without a seed. Only the
//! end-to-end test binaries (`e2e*`) seed: the others check what a build from
//! nothing prints and does. Copies keep the modification times of the
//! originals, which is what makes ninja treat them as up to date.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Output;
use std::time::{Duration, SystemTime};

/// Directories of a project's build that belong to one program, not to the
/// runtime: they are rebuilt for every test.
const PROGRAM: [&str; 3] = ["c", "analysis", "emit"];

fn enabled() -> bool {
    std::env::var_os("RELIFT_TEST_SEED").is_none_or(|v| v != "off")
        && std::env::current_exe()
            .ok()
            .and_then(|p| {
                p.file_name()
                    .map(|n| n.to_string_lossy().starts_with("e2e"))
            })
            .unwrap_or(false)
}

/// The directory holding the seeds of the relift binary under test: a new
/// binary (a rebuilt compiler or tool) starts from nothing.
fn root(bin: &Path) -> PathBuf {
    let meta = fs::metadata(bin).ok();
    let size = meta.as_ref().map_or(0, fs::Metadata::len);
    let age = meta
        .and_then(|m| m.modified().ok())
        .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_secs());
    let base = std::env::var_os("RELIFT_TEST_TMP")
        .map_or_else(|| PathBuf::from(env!("CARGO_TARGET_TMPDIR")), PathBuf::from);
    base.join(format!("relift-seed-{size:x}-{age:x}"))
}

/// The cache directory a relift invocation was given, taken from the working
/// directory `dir` when it is relative.
fn cache_dir(dir: &Path, args: &[&str]) -> Option<PathBuf> {
    let at = args.iter().position(|a| *a == "--cache-dir")?;
    Some(dir.join(args.get(at + 1)?))
}

fn projects(cache: &Path) -> Vec<(String, PathBuf)> {
    fs::read_dir(cache.join("projects"))
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            e.path()
                .is_dir()
                .then(|| (name, e.path().join("build").join("host")))
        })
        .collect()
}

/// Copies the tree `from` to `to`, keeping modification times and links,
/// leaving out the top-level entries named in `skip`.
fn copy_tree(from: &Path, to: &Path, skip: &[&str]) -> std::io::Result<()> {
    fs::create_dir_all(to)?;
    for e in fs::read_dir(from)? {
        let e = e?;
        let name = e.file_name();
        if skip.iter().any(|s| name == *s) {
            continue;
        }
        let (src, dst) = (e.path(), to.join(&name));
        let kind = e.file_type()?;
        if kind.is_dir() {
            copy_tree(&src, &dst, &[])?;
        } else if kind.is_symlink() {
            #[cfg(unix)]
            std::os::unix::fs::symlink(fs::read_link(&src)?, &dst)?;
        } else {
            relift_cache::write_if_changed(&dst, &fs::read(&src)?)?;
            fs::File::open(&dst)?.set_modified(fs::metadata(&src)?.modified()?)?;
            fs::set_permissions(&dst, fs::metadata(&src)?.permissions())?;
        }
    }
    let times = fs::metadata(from)?.modified()?;
    fs::File::open(to)?.set_modified(times)
}

/// Removes seeds of other binaries that no test has used for an hour.
fn prune(root: &Path) {
    let Some(base) = root.parent() else { return };
    let hour = Duration::from_secs(3600);
    for e in fs::read_dir(base).into_iter().flatten().flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let old = e
            .metadata()
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| t.elapsed().ok())
            .is_some_and(|age| age > hour);
        if old && name.starts_with("relift-seed-") && e.path() != root {
            let _ = fs::remove_dir_all(e.path());
        }
    }
}

/// What decides that two projects can share a build directory: the project
/// name, the kind of input it was imported from (the guest in `relift.toml`,
/// else the files in its `import` directory) and the settings given on the
/// command line.
fn key(dir: &Path, project: &Path, name: &str, args: &[&str]) -> String {
    let guest = fs::read_to_string(dir.join("relift.toml"))
        .ok()
        .and_then(|t| {
            t.lines()
                .find_map(|l| l.strip_prefix("guest = ").map(str::to_owned))
        });
    let mut input: Vec<String> = fs::read_dir(project.join("import"))
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    input.sort();
    let input = guest.map_or(input, |g| vec![g]);
    let mut settings: Vec<&str> = Vec::new();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match *a {
            "--cache-dir" | "-j" | "--jobs" => {
                it.next();
            }
            a if a.starts_with("--") => settings.push(a),
            _ => {}
        }
    }
    let mut h = std::collections::hash_map::DefaultHasher::new();
    std::hash::Hash::hash(&settings, &mut h);
    let plain = |s: &str| s.replace(|c: char| !c.is_ascii_alphanumeric(), "_");
    format!(
        "{}-{}-{:x}",
        plain(name),
        plain(&input.join("+")),
        std::hash::Hasher::finish(&h)
    )
}

/// How long a build may hold the lock of its seed before the others stop
/// waiting for it.
const WAIT: Duration = Duration::from_secs(300);

/// The directories of a cache besides its projects: what the build keeps
/// for every project, such as a guest's device library.
fn shared(cache: &Path) -> Vec<PathBuf> {
    fs::read_dir(cache)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_name() != "projects" && e.path().is_dir())
        .map(|e| e.path())
        .collect()
}

/// A build that will save the seed `key` and holds its lock.
struct Builder {
    seed: PathBuf,
    lock: PathBuf,
    host: PathBuf,
    cache: PathBuf,
}

/// Starts the build directory of the project `host` in `cache` from the seed
/// `key`, or waits for the build that is making it, or becomes that build.
fn begin(root: &Path, key: &str, cache: &Path, host: &Path) -> Option<Builder> {
    let seed = root.join(key);
    let lock = root.join(format!(".{key}.lock"));
    let _ = fs::create_dir_all(root);
    let waited = std::time::Instant::now();
    loop {
        if seed.is_dir() {
            let _ = copy_tree(&seed.join("host"), host, &[]);
            for d in fs::read_dir(seed.join("shared"))
                .into_iter()
                .flatten()
                .flatten()
            {
                let to = cache.join(d.file_name());
                if !to.exists() {
                    let _ = copy_tree(&d.path(), &to, &[]);
                }
            }
            return None;
        }
        if fs::create_dir(&lock).is_ok() {
            return Some(Builder {
                seed,
                lock,
                host: host.to_path_buf(),
                cache: cache.to_path_buf(),
            });
        }
        let stale = fs::metadata(&lock)
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| t.elapsed().ok())
            .is_some_and(|age| age > WAIT);
        if stale {
            let _ = fs::remove_dir(&lock);
        } else if waited.elapsed() > WAIT {
            return None;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

impl Builder {
    fn finish(self, built: bool) {
        if built && self.host.is_dir() && !self.seed.exists() {
            let part = self
                .seed
                .with_extension(format!("part{}", std::process::id()));
            let _ = fs::remove_dir_all(&part);
            let copied = copy_tree(&self.host, &part.join("host"), &PROGRAM).is_ok()
                && shared(&self.cache).iter().all(|d| {
                    copy_tree(
                        d,
                        &part.join("shared").join(d.file_name().unwrap_or_default()),
                        &[],
                    )
                    .is_ok()
                });
            if copied {
                let _ = fs::rename(&part, &self.seed);
            }
            let _ = fs::remove_dir_all(&part);
        }
        let _ = fs::remove_dir(&self.lock);
    }
}

/// Runs `run`, the relift invocation of `args` in `dir`, with the seed: a
/// `build`, `run` or `diff` of a project whose build directory does not exist yet
/// starts from the saved one, or saves the one it makes.
pub fn seeded(dir: &Path, args: &[&str], bin: &Path, run: impl FnOnce() -> Output) -> Output {
    let building = ["build", "run", "diff"].iter().any(|a| args.contains(a));
    let cache = cache_dir(dir, args).filter(|_| building && enabled());
    let Some(cache) = cache else {
        return run();
    };
    let root = root(bin);
    prune(&root);
    let mut builders = Vec::new();
    for (name, host) in projects(&cache) {
        if host.exists() {
            continue;
        }
        let project = host.ancestors().nth(2).map(Path::to_path_buf);
        if let Some(project) = project {
            let key = key(dir, &project, &name, args);
            builders.extend(begin(&root, &key, &cache, &host));
        }
    }
    let out = run();
    for b in builders {
        b.finish(out.status.success());
    }
    out
}
