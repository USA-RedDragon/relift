//! End to end for the Linux guest: a statically linked aarch64 ELF goes
//! through `relift import` and `relift build`, and the program runs with the
//! output and exit status the kernel would give it. The first test builds
//! its program from machine code written here; the second builds programs of
//! the guest's corpus (`guests/linux/guest/corpus`) with zig and compares
//! them with the answers recorded on a real arm64 Linux. Both skip with a
//! note when clang is not on `PATH`, and the second when zig is not, unless
//! `RELIFT_LINUX_REQUIRE` is set. `setjmp` and `coroutines` are built a second
//! time with trampoline mode forced; the corpus check script runs every
//! program in both modes. Only `hello` builds with the cold copy (the Go and
//! signal programs of the corpus check use it); the rest build without it,
//! which keeps the test short. The dynamic program builds as a dev build
//! (`relift build --dev`), so its libraries' lifted C compiles at -O0:
//! libc's 34 shards took 131 CPU seconds there against 1119 at the release
//! build's -O2. The static programs keep the release build, so the corpus
//! still runs code the optimizer compiled.

mod tiny;

use std::path::Path;

use relift_objfile::synth::{Builder, SegmentSpec};

const BASE: u64 = 0x40_0000;

/// `write(1, "hello\n", 6); exit_group(7)`.
fn program() -> Vec<u8> {
    let code: [u32; 8] = [
        0xd280_0808,
        0xd280_0020,
        0x1000_7fc1,
        0xd280_00c2,
        0xd400_0001,
        0xd280_0ba8,
        0xd280_00e0,
        0xd400_0001,
    ];
    let mut text: Vec<u8> = code.iter().flat_map(|w| w.to_le_bytes()).collect();
    text.extend_from_slice(&0xd503_201fu32.to_le_bytes());
    let size = text.len() as u64;
    let msg = b"hello\n".to_vec();
    Builder {
        entry: BASE,
        segments: vec![
            SegmentSpec {
                addr: BASE,
                data: text,
                mem_size: size,
                flags: 5,
            },
            SegmentSpec {
                addr: BASE + 0x1000,
                mem_size: msg.len() as u64,
                data: msg,
                flags: 4,
            },
        ],
        ..Builder::default()
    }
    .build()
}

fn built(
    root: &Path,
    program: &Path,
    cache: &str,
    (trampoline, cold, dev): (bool, bool, bool),
    sysroot: Option<&str>,
) -> (std::path::PathBuf, String) {
    let project = root.join("sample");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = program.display().to_string();
    let mut args = vec!["--cache-dir", cache, "import"];
    if let Some(s) = sysroot {
        args.extend(["--sysroot", s]);
    }
    args.push(&image);
    let out = tiny::relift(&project, &args);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let toml = std::fs::read_to_string(project.join("relift.toml")).expect("relift.toml");
    assert!(toml.contains("guest = \"linux\""), "{toml}");
    let forced = if trampoline { "true" } else { "false" };
    let mut args = vec!["--cache-dir", cache, "-j", "4", "build"];
    if dev {
        args.push("--dev");
    }
    let out = tiny::relift_env(
        &project,
        &args,
        &[
            ("RELIFT_EMIT_TRAMPOLINE", forced),
            ("RELIFT_EMIT_COLD", if cold { "true" } else { "false" }),
            ("RELIFT_TEST_SEED", "off"),
        ],
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(!stderr.contains("missed"), "{stderr}");
    assert!(
        !trampoline || stderr.contains("returns: trampoline mode"),
        "{stderr}"
    );
    let exe = Path::new(cache)
        .join("projects/sample/build/host")
        .join(format!("sample{}", std::env::consts::EXE_SUFFIX));
    (exe, stderr.into_owned())
}

/// Programs built and run at once by the corpus test: each build compiles
/// with four jobs, and one large file at the end of it leaves the other
/// cores idle.
const WORKERS: usize = 4;

/// Builds the corpus program `name` in a directory of its own under `root`,
/// in trampoline mode or not, runs it, and checks its output and status
/// against the files of its corpus directory.
fn check_program(root: &Path, corpus: &Path, programs: &Path, name: &str, trampoline: bool) {
    let mode = if trampoline { "trampoline" } else { "direct" };
    let root = root.join(format!("{name}-{mode}"));
    std::fs::create_dir_all(&root).expect("the program dir is creatable");
    let cache = root.join("cache").display().to_string();
    let (exe, note) = built(
        &root,
        &programs.join(name),
        &cache,
        (trampoline, name == "hello.static" && !trampoline, false),
        None,
    );
    let wants = match name {
        "coroutines.static" | "setjmp.static" => Some("returns: trampoline mode ("),
        "hello.static" => Some("returns: direct"),
        _ => None,
    };
    if let Some(wants) = wants.filter(|_| !trampoline) {
        assert!(note.contains(wants), "{name}: {note}");
    }
    let dir = corpus.join(name.split('.').next().expect("a name"));
    let mut cmd = relift_proc::command(&exe);
    cmd.env("RELIFT_REPORT_DIR", &root).env("TMPDIR", &root);
    if let Ok(args) = std::fs::read_to_string(dir.join("args")) {
        cmd.args(args.lines());
    }
    let out = cmd.output().expect("the program runs");
    let want = std::fs::read(dir.join("expected.stdout")).expect("expected.stdout");
    assert_eq!(
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&want),
        "{name} (trampoline: {trampoline})"
    );
    let code: i32 = std::fs::read_to_string(dir.join("expected.exit"))
        .expect("expected.exit")
        .trim()
        .parse()
        .expect("a status");
    assert_eq!(
        out.status.code(),
        Some(code),
        "{name} (trampoline: {trampoline})"
    );
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_hand_assembled_program_builds_and_exits_with_its_status() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-linux");
    let elf = root.join("hello");
    std::fs::create_dir_all(&root).expect("the scratch dir is creatable");
    relift_cache::write_if_changed(&elf, &program()).expect("the program is writable");
    let cache = root.join("cache").display().to_string();
    let (exe, _) = built(&root, &elf, &cache, (false, false, false), None);
    let out = relift_proc::command(&exe)
        .env("RELIFT_REPORT_DIR", &root)
        .output()
        .expect("the program runs");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "hello\n");
    assert_eq!(out.status.code(), Some(7));
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_program_is_named_after_the_project() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-linux-name");
    let elf = root.join("hello");
    std::fs::create_dir_all(&root).expect("the scratch dir is creatable");
    relift_cache::write_if_changed(&elf, &program()).expect("the program is writable");
    let cache = root.join("cache").display().to_string();
    let project = root.join("Nameless Folder");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = elf.display().to_string();
    let exe = |dir: &str| {
        root.join("cache/projects")
            .join(dir)
            .join("build/host")
            .join(format!("{}{}", dir_slug(dir), std::env::consts::EXE_SUFFIX))
    };
    let run = |args: &[&str]| {
        let out = tiny::relift(&project, args);
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
    };
    run(&["--cache-dir", &cache, "import", &image]);
    run(&["--cache-dir", &cache, "-j", "4", "build"]);
    assert!(
        exe("Nameless Folder").is_file(),
        "a nameless project takes its folder's name"
    );
    let manifest = project.join("relift.toml");
    let text = std::fs::read_to_string(&manifest).expect("relift.toml reads");
    let text = text.replacen("[game]\n", "[game]\nname = \"Example Project 2\"\n", 1);
    relift_cache::write_if_changed(&manifest, text.as_bytes()).expect("relift.toml writes");
    run(&["--cache-dir", &cache, "-j", "4", "build"]);
    let named = root
        .join("cache/projects/Nameless Folder/build/host")
        .join(format!("example-project-2{}", std::env::consts::EXE_SUFFIX));
    assert!(named.is_file(), "the manifest's name names the program");
    let out = relift_proc::command(&named)
        .output()
        .expect("the program runs");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "hello\n");
    let _ = std::fs::remove_dir_all(&root);
}

fn dir_slug(dir: &str) -> String {
    dir.to_lowercase().replace(' ', "-")
}

#[test]
fn corpus_programs_print_what_they_printed_on_arm64_linux() {
    let have = |t: &str| relift_build::find_tool(t).is_some();
    if !have("clang") || !have("zig") {
        assert!(
            std::env::var_os("RELIFT_LINUX_REQUIRE").is_none(),
            "RELIFT_LINUX_REQUIRE is set and clang or zig is missing"
        );
        eprintln!("skipped: no clang or zig on PATH");
        return;
    }
    let corpus = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guests/linux/guest/corpus");
    let root = tiny::scratch("e2e-linux-corpus");
    std::fs::create_dir_all(&root).expect("the scratch dir is creatable");
    let programs = root.join("programs");
    let status = relift_proc::command("sh")
        .arg(corpus.join("build.sh"))
        .arg(&programs)
        .env("ZIG_GLOBAL_CACHE_DIR", root.join("zig"))
        .env("ZIG_LOCAL_CACHE_DIR", root.join("zig"))
        .status()
        .expect("build.sh runs");
    assert!(status.success());
    let work: Vec<(&str, bool)> = [
        "hello.static",
        "setjmp.static",
        "setjmp.pie",
        "jmp_ptr.static",
        "threads.static",
        "sig_interrupt.static",
        "signals.static",
        "cxx_exceptions.static",
        "fileio.static",
        "coroutines.static",
    ]
    .into_iter()
    .flat_map(|name| {
        let both = ["setjmp.static", "coroutines.static"].contains(&name);
        [(name, false)]
            .into_iter()
            .chain(both.then_some((name, true)))
    })
    .collect();
    let next = std::sync::atomic::AtomicUsize::new(0);
    std::thread::scope(|scope| {
        let workers: Vec<_> = (0..WORKERS.min(work.len()))
            .map(|_| {
                scope.spawn(|| {
                    loop {
                        let i = next.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                        let Some(&(name, trampoline)) = work.get(i) else {
                            return;
                        };
                        check_program(&root, &corpus, &programs, name, trampoline);
                    }
                })
            })
            .collect();
        for w in workers {
            if let Err(e) = w.join() {
                std::panic::resume_unwind(e);
            }
        }
    });
    let _ = std::fs::remove_dir_all(&root);
}

/// Removes the compiled libraries of the kept cache `kept` that the last
/// build did not link: each change of the lifted C keys new ones, and CI
/// saves the folder whole, so without this it would grow by every version.
fn prune_lifted(kept: &Path) {
    let ninja = std::fs::read_to_string(kept.join("projects/sample/build/host/build.ninja"))
        .expect("the build wrote build.ninja");
    let Ok(entries) = std::fs::read_dir(kept.join("linux-runtime-lifted")) else {
        return;
    };
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if !ninja.contains(&format!("linux-runtime-lifted/{name}/")) {
            let _ = std::fs::remove_dir_all(e.path());
        }
    }
}

/// The sysroot of the arm64 cross compiler: the directory above the `lib`
/// that holds its loader. `-print-sysroot` is no help, since Debian and
/// Ubuntu print `/` for a compiler whose libraries sit in
/// `/usr/aarch64-linux-gnu/lib`, while Arch prints that directory.
fn cross_sysroot() -> String {
    let out = relift_proc::command("aarch64-linux-gnu-gcc")
        .arg("-print-file-name=ld-linux-aarch64.so.1")
        .output()
        .expect("the compiler runs");
    let loader = String::from_utf8_lossy(&out.stdout).trim().to_string();
    let lib = Path::new(&loader)
        .parent()
        .and_then(|d| d.canonicalize().ok())
        .unwrap_or_else(|| panic!("the compiler does not find its loader: {loader}"));
    lib.parent()
        .expect("the loader's directory has a parent")
        .display()
        .to_string()
}

#[test]
fn dynamic_corpus_programs_print_what_they_printed_on_arm64_linux() {
    let have = |t: &str| relift_build::find_tool(t).is_some();
    if !have("clang") || !have("aarch64-linux-gnu-gcc") {
        assert!(
            std::env::var_os("RELIFT_LINUX_REQUIRE").is_none(),
            "RELIFT_LINUX_REQUIRE is set and clang or aarch64-linux-gnu-gcc is missing"
        );
        eprintln!("skipped: no clang or aarch64-linux-gnu-gcc on PATH");
        return;
    }
    let sysroot = cross_sysroot();
    let corpus = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guests/linux/guest/corpus");
    let root = tiny::scratch("e2e-linux-dynamic");
    std::fs::create_dir_all(&root).expect("the scratch dir is creatable");
    let programs = root.join("programs");
    let status = relift_proc::command("sh")
        .arg(corpus.join("build.sh"))
        .arg(&programs)
        .env("ONLY_DYNAMIC", "1")
        .env("AARCH64_GCC", "aarch64-linux-gnu-gcc")
        .status()
        .expect("build.sh runs");
    assert!(status.success());
    let kept = Path::new(env!("CARGO_TARGET_TMPDIR")).join("e2e-linux-dynamic-cache");
    let cache = kept.display().to_string();
    {
        let (name, modes) = ("shared_lib.dyn", (false, false, true));
        let _ = std::fs::remove_dir_all(root.join("sample"));
        let _ = std::fs::remove_dir_all(kept.join("projects"));
        let (exe, _) = built(&root, &programs.join(name), &cache, modes, Some(&sysroot));
        prune_lifted(&kept);
        let dir = corpus.join(name.split('.').next().expect("a name"));
        let out = relift_proc::command(&exe)
            .env("RELIFT_REPORT_DIR", &root)
            .env("TMPDIR", &root)
            .output()
            .expect("the program runs");
        let want = std::fs::read(dir.join("expected.stdout")).expect("expected.stdout");
        assert_eq!(
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&want),
            "{name}"
        );
        let code: i32 = std::fs::read_to_string(dir.join("expected.exit"))
            .expect("expected.exit")
            .trim()
            .parse()
            .expect("a status");
        assert_eq!(out.status.code(), Some(code), "{name}");
    }
    let _ = std::fs::remove_dir_all(&root);
}
