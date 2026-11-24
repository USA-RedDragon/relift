//! Compiling a handheld or Linux guest's program: the lifted files, the
//! program files and the runtime layers the guest names ([`Layers`]),
//! with the runtime and, for a guest that shares them, every lifted
//! library kept as objects in the cache by content.

use std::path::{Path, PathBuf};

use relift_build::{Compile, Plan};
use relift_progress::{Line, noun};

use crate::cross;
use crate::import::{BoxError, cache_root};
use crate::settings::Settings;
use crate::ui::Ui;

fn c_files(dir: &Path) -> Result<Vec<PathBuf>, BoxError> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)
        .map_err(|e| format!("{}: {e}", dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "c" || x == "S"))
        .collect();
    v.sort();
    Ok(v)
}

/// What a handheld guest's program compiles: the runtime directories, the
/// include directories of lifted code and of the runtime and program files
/// (below the runtime root), the optimization of lifted code in a dev build
/// (a release build uses [`crate::build::RELEASE_LIFTED_OPT`]) and the name
/// of the shared runtime object's cache.
pub(crate) struct Layers {
    pub(crate) dirs: &'static [&'static str],
    pub(crate) lifted_inc: &'static [&'static str],
    pub(crate) rt_inc: &'static [&'static str],
    pub(crate) lifted_opt: &'static str,
    pub(crate) cache: &'static str,
    /// Whether the layers include the A64 header, which needs 16-byte
    /// compare-and-swap on an x86-64 host.
    pub(crate) cx16: bool,
    /// Whether the lifted files of one directory (but `unit.c`, which holds
    /// the program's identity) compile once into an object kept by content
    /// in the shared cache, so a program that lifts the same libraries
    /// again compiles none of them.
    pub(crate) share_lifted: bool,
}

/// Compiles the lifted files `lifted` and the program files `prog` of
/// `cdir` with the runtime layers `layers` and links `game` in `out`.
pub(crate) fn compile_layers(
    settings: &Settings,
    out: &Path,
    cdir: &Path,
    (lifted, prog): (&[PathBuf], &[PathBuf]),
    layers: &Layers,
    (release, cross): (bool, Option<&cross::Build>),
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let rt = crate::build::runtime_dir(settings);
    let croot = crate::paths::canonical(cache_root(settings)?)?;
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let cc = match cross {
        Some(c) => c.cc(),
        None if release => crate::build::release_compiler(settings)?,
        None => crate::build::c_compiler(&settings.toolchain)?,
    };
    let zig = cc.len() > 1;
    let cx16 = layers.cx16 && cross.is_none() && cfg!(target_arch = "x86_64");
    let base: Vec<String> = ["-std=c11", "-O2", "-ffp-contract=off"]
        .iter()
        .map(|s| s.to_string())
        .chain(cx16.then(|| "-mcx16".to_string()))
        .chain(zig.then_some("-fno-sanitize=undefined".to_string()))
        .chain(cross.filter(|c| c.pic()).map(|_| "-fPIC".to_string()))
        .collect();
    let static_rt = (cross.is_none() && cfg!(windows)).then_some("-DRL_STATIC_RUNTIME".to_string());
    let lifted_flags: Vec<String> = base
        .iter()
        .map(|f| {
            if f == "-O2" {
                crate::build::lifted_opt(release, layers.lifted_opt).into()
            } else {
                f.clone()
            }
        })
        .chain(
            ["-fno-omit-frame-pointer"]
                .into_iter()
                .chain(cross.is_none().then_some(if settings.emit.line_tables {
                    "-gline-tables-only"
                } else {
                    "-g0"
                }))
                .map(String::from),
        )
        .chain(layers.lifted_inc.iter().map(|p| inc(p)))
        .chain(static_rt.clone())
        .collect();
    let mut extras = match cross {
        Some(c) => c.extras(&rt, false)?,
        None => crate::runtime_link::extras(&rt, &croot)?,
    };
    extras.vk.clear();
    ui.note(&Line::new().text(extras.note()));
    let rt_flags: Vec<String> = base
        .iter()
        .cloned()
        .chain(
            cross
                .is_none()
                .then_some("-g")
                .into_iter()
                .chain([
                    "-fno-omit-frame-pointer",
                    "-D_GNU_SOURCE",
                    "-D_DARWIN_C_SOURCE",
                    "-DRL_BUILDING_RUNTIME",
                    "-DRL_CHECKS",
                ])
                .map(String::from),
        )
        .chain(layers.rt_inc.iter().map(|p| inc(p)))
        .chain(extras.defines())
        .chain(static_rt)
        .chain((!release && cross.is_none()).then(|| "-DRL_DEV_RUN".to_string()))
        .collect();
    let obj = |src: &Path, tag: &str| {
        let stem = src
            .strip_prefix(cdir)
            .or_else(|_| src.strip_prefix(&rt))
            .unwrap_or(src);
        out.join("obj").join(tag).join(stem).with_extension("o")
    };
    let shared = cross.is_some() || !cfg!(windows);
    let (kept, lifted): (Vec<&PathBuf>, Vec<&PathBuf>) = lifted.iter().partition(|f| {
        layers.share_lifted && shared && f.file_name().is_some_and(|n| n != "unit.c")
    });
    // The cold copy of a unit (the files under its `cold` directory) runs
    // only code that discovery missed, and a Switch build already compiles
    // it without debug info; line tables cost about 40% of the compile time
    // of a lifted file at -O0.
    let cold_flags: Vec<String> = lifted_flags
        .iter()
        .filter(|f| f.as_str() != "-gline-tables-only")
        .cloned()
        .chain(["-g0".to_string()])
        .collect();
    let mut compiles: Vec<Compile> = lifted
        .iter()
        .map(|s| Compile {
            src: (*s).clone(),
            obj: obj(s, "lifted"),
            flags: if s
                .strip_prefix(cdir)
                .is_ok_and(|r| r.components().any(|c| c.as_os_str() == "cold"))
            {
                cold_flags.clone()
            } else {
                lifted_flags.clone()
            },
            pool: None,
            stage: 0,
        })
        .collect();
    let mut rt_compiles = Vec::new();
    for layer in layers.dirs {
        rt_compiles.extend(c_files(&rt.join(layer))?);
    }
    let (prog, resources) = crate::winres::split(prog);
    for s in &prog {
        compiles.push(Compile {
            src: s.clone(),
            obj: obj(s, "prog"),
            flags: rt_flags
                .iter()
                .cloned()
                .chain([crate::build::build_dir_flag(out)])
                .collect(),
            pool: None,
            stage: 0,
        });
    }
    let jobs = relift_build::job_count(settings.jobs);
    let ninja = crate::build::tool(&settings.toolchain.ninja, "ninja");
    let key: String =
        relift_switch_formats::sha256(format!("{cc:?}\n{rt_flags:?}\n{}", rt.display()).as_bytes())
            [..8]
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
    let rt_dir = croot.join(layers.cache).join(&key);
    let rt_obj = |s: &Path| {
        let stem = s.strip_prefix(&rt).unwrap_or(s);
        rt_dir.join("obj").join(stem).with_extension("o")
    };
    let mut rt_plan_compiles: Vec<Compile> = rt_compiles
        .iter()
        .map(|s| Compile {
            src: s.clone(),
            obj: if shared { rt_obj(s) } else { obj(s, "rt") },
            flags: rt_flags.clone(),
            pool: None,
            stage: 0,
        })
        .collect();
    rt_plan_compiles.extend(extras.compiles(&rt, &rt_flags, &|s| {
        if shared { rt_obj(s) } else { obj(s, "rt") }
    }));
    let row = ui.row("");
    ui.begin(row, "compile");
    let mut link: Vec<String> = Vec::new();
    let mut n = 0;
    let exe = out.join(match cross {
        Some(c) => c.program(&crate::progname::stem()),
        None => crate::progname::program(),
    });
    if shared {
        std::fs::create_dir_all(&rt_dir)?;
        let lock_path = rt_dir.join("lock");
        relift_cache::write_if_changed(&lock_path, b"")?;
        let lock = std::fs::File::open(&lock_path)?;
        lock.lock()?;
        let rt_o = rt_dir.join("runtime.o");
        let pools = crate::build::memory_pools(&mut rt_plan_compiles, jobs);
        let plan = Plan {
            cc: cc.clone(),
            asm: extras.assembler(),
            compiles: rt_plan_compiles,
            output: rt_o.clone(),
            link_flags: ["-r", "-nostdlib"].map(String::from).into(),
            pools,
            root: None,
        };
        n += crate::build::build_plans(
            ui,
            row,
            &[(plan, rt_dir.clone())],
            ninja.as_deref(),
            jobs,
            "",
        )?;
        drop(lock);
        let time = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
        if time(&rt_o) > time(&exe) && exe.exists() {
            std::fs::remove_file(&exe)?;
        }
        link.push(rt_o.display().to_string());
        let mut groups: std::collections::BTreeMap<&Path, Vec<&PathBuf>> =
            std::collections::BTreeMap::new();
        for f in &kept {
            if let Some(dir) = f.parent() {
                groups.entry(dir).or_default().push(f);
            }
        }
        let store = croot.join(format!("{}-lifted", layers.cache));
        let mut entries = Vec::new();
        for (dir, files) in groups {
            let cold = dir
                .strip_prefix(cdir)
                .is_ok_and(|r| r.components().any(|c| c.as_os_str() == "cold"));
            let flags = if cold { &cold_flags } else { &lifted_flags };
            entries.push(shared_lifted(&cc, flags, &store, dir, files, jobs)?);
        }
        n += build_shared(&entries, (ninja.as_deref(), jobs), (ui, row))?;
        for e in &entries {
            if time(&e.output) > time(&exe) && exe.exists() {
                std::fs::remove_file(&exe)?;
            }
            link.push(e.output.display().to_string());
        }
    } else {
        compiles.extend(rt_plan_compiles);
    }
    let pools = crate::build::memory_pools(&mut compiles, jobs);
    let libs: Vec<String> = if let Some(c) = cross {
        c.libs(&crate::progname::stem())
    } else if cfg!(windows) {
        [
            "-lpthread",
            "-lm",
            "-lws2_32",
            "-luserenv",
            "-lntdll",
            "-lbcrypt",
        ]
        .map(String::from)
        .into()
    } else {
        let mut l: Vec<String> = ["-lpthread", "-lm", "-ldl"].map(String::from).into();
        if zig && cfg!(target_os = "linux") {
            l.push("-lunwind".into());
        }
        l
    };
    let plan = Plan {
        cc: cc.clone(),
        asm: None,
        compiles,
        output: exe,
        link_flags: link
            .into_iter()
            .chain(resources)
            .chain(libs)
            .chain(extras.libs())
            .collect(),
        pools,
        root: None,
    };
    n += crate::build::build_plans(
        ui,
        row,
        &[(plan, out.to_path_buf())],
        ninja.as_deref(),
        jobs,
        "",
    )?;
    ui.done(row, Line::new().text(noun(n as u64, "file")));
    Ok(())
}

/// One entry of the shared lifted store, ready to build: its plan, its
/// directory and the lock that keeps another build out of it until it is
/// built.
struct SharedEntry {
    plan: Plan,
    dir: PathBuf,
    output: PathBuf,
    _lock: std::fs::File,
}

/// The entry of the C files `files` of one lifted unit directory `dir` in
/// the store `store`, keyed by the content of the files, the compiler and
/// the flags: the sources are copied into the entry so the compile
/// commands do not name the project, and a later build of the same
/// libraries finds the object and compiles nothing. [`build_shared`]
/// builds it.
fn shared_lifted(
    cc: &[String],
    flags: &[String],
    store: &Path,
    dir: &Path,
    files: Vec<&PathBuf>,
    jobs: u32,
) -> Result<SharedEntry, BoxError> {
    let mut texts: Vec<(String, Vec<u8>)> = Vec::new();
    for f in files {
        let name = f
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        texts.push((name, std::fs::read(f)?));
    }
    let prelude = std::fs::read(dir.join(relift_emit::PRELUDE_NAME))?;
    let mut hashed = format!("{cc:?}\n{flags:?}\n").into_bytes();
    hashed.extend_from_slice(&prelude);
    for (n, t) in &texts {
        hashed.extend_from_slice(n.as_bytes());
        hashed.extend_from_slice(&relift_switch_formats::sha256(t));
    }
    let key: String = relift_switch_formats::sha256(&hashed)[..12]
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let entry = store.join(key);
    std::fs::create_dir_all(&entry)?;
    let lock_path = entry.join("lock");
    relift_cache::write_if_changed(&lock_path, b"")?;
    let lock = std::fs::File::open(&lock_path)?;
    lock.lock()?;
    std::fs::create_dir_all(entry.join("src"))?;
    relift_cache::write_if_changed(&entry.join("src").join(relift_emit::PRELUDE_NAME), &prelude)?;
    let mut compiles = Vec::new();
    for (n, t) in &texts {
        let src = entry.join("src").join(n);
        relift_cache::write_if_changed(&src, t)?;
        compiles.push(Compile {
            src: src.clone(),
            obj: entry.join("obj").join(n).with_extension("o"),
            flags: flags.to_vec(),
            pool: None,
            stage: 0,
        });
    }
    let output = entry.join("module.o");
    let pools = crate::build::memory_pools(&mut compiles, jobs);
    Ok(SharedEntry {
        plan: Plan {
            cc: cc.to_vec(),
            asm: None,
            compiles,
            output: output.clone(),
            link_flags: ["-r", "-nostdlib"].map(String::from).into(),
            pools,
            root: None,
        },
        dir: entry,
        output,
        _lock: lock,
    })
}

/// What one shared entry takes from the build's budget while it builds:
/// a job per file up to all `jobs`, and the memory of its largest
/// compiles that many of them run at once, at most `memory` (an entry
/// larger than the whole budget runs alone, its own pools bounding it).
fn entry_need(plan: &Plan, jobs: u32, memory: u64) -> (u32, u64) {
    let want = u32::try_from(plan.compiles.len())
        .unwrap_or(u32::MAX)
        .clamp(1, jobs.max(1));
    let mut needs: Vec<u64> = plan
        .compiles
        .iter()
        .map(|c| relift_build::compile_memory(std::fs::metadata(&c.src).map_or(0, |m| m.len())))
        .collect();
    needs.sort_unstable_by(|a, b| b.cmp(a));
    let mem: u64 = needs.iter().take(want as usize).sum();
    (want, mem.min(memory))
}

/// Runs `work` on every item of `items` side by side, never with more than
/// `jobs` jobs or `memory` bytes taken in all, as `need` says per item;
/// `on_done` hears each result on the calling thread (with `None` about
/// every 250 ms while nothing finishes).
fn side_by_side<T: Sync, R: Send>(
    items: &[T],
    (jobs, memory): (u32, u64),
    need: &(dyn Fn(&T) -> (u32, u64) + Sync),
    work: &(dyn Fn(&T, u32) -> R + Sync),
    on_done: &mut dyn FnMut(Option<R>),
) {
    use std::sync::{Condvar, Mutex, PoisonError, mpsc};
    let jobs = jobs.max(1);
    let free = Mutex::new((jobs, memory));
    let freed = Condvar::new();
    let next = Mutex::new(0usize);
    let (tx, rx) = mpsc::channel::<R>();
    std::thread::scope(|scope| {
        for _ in 0..(jobs as usize).min(items.len()) {
            let tx = tx.clone();
            let (free, freed, next) = (&free, &freed, &next);
            scope.spawn(move || {
                loop {
                    let i = {
                        let mut n = next.lock().unwrap_or_else(PoisonError::into_inner);
                        let i = *n;
                        *n += 1;
                        i
                    };
                    let Some(item) = items.get(i) else {
                        return;
                    };
                    let (want, mem) = need(item);
                    let (want, mem) = (want.clamp(1, jobs), mem.min(memory));
                    {
                        let mut f = free.lock().unwrap_or_else(PoisonError::into_inner);
                        while f.0 < want || f.1 < mem {
                            f = freed.wait(f).unwrap_or_else(PoisonError::into_inner);
                        }
                        f.0 -= want;
                        f.1 -= mem;
                    }
                    let r = work(item, want);
                    {
                        let mut f = free.lock().unwrap_or_else(PoisonError::into_inner);
                        f.0 += want;
                        f.1 += mem;
                    }
                    freed.notify_all();
                    if tx.send(r).is_err() {
                        return;
                    }
                }
            });
        }
        drop(tx);
        loop {
            match rx.recv_timeout(std::time::Duration::from_millis(250)) {
                Ok(r) => on_done(Some(r)),
                Err(mpsc::RecvTimeoutError::Timeout) => on_done(None),
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        }
    });
}

/// Builds the shared entries side by side (a program with hundreds of
/// libraries has hundreds of entries of one or two files each, which one
/// after another leave all but one core idle), within the build's jobs and
/// the memory budget of [`crate::build::memory_pools`] (three fifths of
/// the memory available now). Returns the number of steps run.
fn build_shared(
    entries: &[SharedEntry],
    (ninja, jobs): (Option<&Path>, u32),
    (ui, row): (&mut Ui, usize),
) -> Result<usize, BoxError> {
    let jobs = relift_build::job_count(jobs).max(1);
    let memory = relift_build::available_memory().map_or(u64::MAX, |m| m / 5 * 3);
    let compile = relift_progress::Metrics {
        unit: "libraries",
        elapsed: true,
        ..relift_progress::Metrics::default()
    };
    ui.update(row, compile.clone());
    let total = entries.len() as u64;
    let (mut steps, mut finished, mut failed) = (0, 0u64, None);
    side_by_side(
        entries,
        (jobs, memory),
        &|e: &SharedEntry| entry_need(&e.plan, jobs, memory),
        &|e: &SharedEntry, want| {
            let mut done = 0;
            relift_build::build_with(&e.plan, &e.dir, ninja, want, &mut |ev| {
                if let relift_build::Event::Step { done: d, .. } = ev {
                    done = d;
                }
            })
            .map(|()| done)
            .map_err(|e| e.to_string())
        },
        &mut |r| match r {
            None => ui.tick(),
            Some(r) => {
                finished += 1;
                match r {
                    Ok(d) => steps += d,
                    Err(e) => {
                        failed.get_or_insert(e);
                    }
                }
                ui.update(
                    row,
                    relift_progress::Metrics {
                        done: finished,
                        total: Some(total),
                        ..compile.clone()
                    },
                );
            }
        },
    );
    match failed {
        Some(e) => Err(e.into()),
        None => Ok(steps),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Runs `side_by_side` over `needs` and returns the most items that
    /// ran at once and the results in finishing order.
    fn overlap(needs: &[(u32, u64)], budget: (u32, u64)) -> (usize, Vec<usize>) {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let running = AtomicUsize::new(0);
        let most = AtomicUsize::new(0);
        let items: Vec<usize> = (0..needs.len()).collect();
        let mut done = Vec::new();
        side_by_side(
            &items,
            budget,
            &|i: &usize| needs[*i],
            &|i: &usize, _| {
                let now = running.fetch_add(1, Ordering::SeqCst) + 1;
                most.fetch_max(now, Ordering::SeqCst);
                std::thread::sleep(std::time::Duration::from_millis(100));
                running.fetch_sub(1, Ordering::SeqCst);
                *i
            },
            &mut |r| done.extend(r),
        );
        (most.load(Ordering::SeqCst), done)
    }

    #[test]
    fn two_small_entries_build_side_by_side() {
        let (most, mut done) = overlap(&[(1, 10), (1, 10)], (16, 100));
        assert_eq!(most, 2);
        done.sort_unstable();
        assert_eq!(done, [0, 1]);
    }

    #[test]
    fn entries_wait_for_jobs_and_memory_and_one_larger_than_the_budget_runs_alone() {
        assert_eq!(overlap(&[(1, 60), (1, 60), (1, 60)], (16, 100)).0, 1);
        assert_eq!(overlap(&[(8, 10), (8, 10), (8, 10)], (16, 100)).0, 2);
        let (most, done) = overlap(&[(1, 500), (1, 500)], (16, 100));
        assert_eq!((most, done.len()), (1, 2));
    }
}
