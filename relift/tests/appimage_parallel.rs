//! Packaging as an AppImage, the one test about it: builds that package at
//! the same time share one pinned runtime in the cache, use no shared
//! temporary path, and each leaves a whole image and no scratch behind.

mod tiny;

use std::path::{Path, PathBuf};

fn default_cache() -> PathBuf {
    let base = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| Path::new(&h).join(".cache")))
        .expect("a cache directory");
    base.join("relift")
}

fn build(project: &Path, cache: &str) -> std::process::Output {
    let bin = std::env::var_os("RELIFT_TEST_BIN").map_or_else(
        || PathBuf::from(env!("CARGO_BIN_EXE_relift")),
        PathBuf::from,
    );
    relift_proc::command(bin)
        .args(["--cache-dir", cache, "-j", "2", "build"])
        .env_remove("RELIFT_JOBS")
        .env("RELIFT_PACKAGE_APPIMAGE", "always")
        .current_dir(project)
        .output()
        .expect("the relift binary runs")
}

#[test]
fn parallel_builds_package_with_the_pinned_runtime() {
    if !cfg!(target_os = "linux")
        || relift_build::find_tool("clang").is_none()
        || relift_build::find_tool("appimagetool").is_none()
    {
        eprintln!("skipped: needs Linux, clang and appimagetool on PATH");
        return;
    }
    let arch = std::env::consts::ARCH;
    let runtime = match relift::appimage_runtime::ensure(&default_cache(), arch) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("skipped: {e}");
            return;
        }
    };
    let root = tiny::scratch("appimage-parallel");
    let runtime_bytes = std::fs::read(&runtime).unwrap();
    let builds: Vec<(PathBuf, PathBuf)> = (0..4)
        .map(|i| {
            let r = root.join(format!("p{i}"));
            std::fs::create_dir_all(&r).unwrap();
            let cache = r.join("cache");
            let seeded = relift::appimage_runtime::path(&cache, arch);
            std::fs::create_dir_all(seeded.parent().unwrap()).unwrap();
            relift_cache::write_if_changed(&seeded, &runtime_bytes).unwrap();
            let project = tiny::dump(&r);
            let cache_arg = cache.display().to_string();
            let out = tiny::relift(&project, &["--cache-dir", &cache_arg, "import", "../dump"]);
            assert!(
                out.status.success(),
                "{}",
                String::from_utf8_lossy(&out.stderr)
            );
            (project, cache)
        })
        .collect();
    let outs: Vec<_> = std::thread::scope(|s| {
        let hs: Vec<_> = builds
            .iter()
            .map(|(p, c)| s.spawn(|| build(p, &c.display().to_string())))
            .collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    });
    for out in &outs {
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
    }
    for (_, cache) in &builds {
        let host = cache.join("projects/tiny/build/host");
        let found: Vec<_> = std::fs::read_dir(&host)
            .unwrap()
            .map(|e| e.unwrap().path())
            .filter(|p| p.extension().is_some_and(|x| x == "AppImage"))
            .collect();
        assert_eq!(found.len(), 1, "one AppImage in {}", host.display());
        let head = std::fs::read(&found[0]).unwrap();
        assert_eq!(&head[..4], b"\x7fELF");
        assert_eq!(&head[8..11], b"AI\x02", "a type 2 AppImage");
        let left = std::fs::read_dir(host.join("appimage")).map_or(0, Iterator::count);
        assert_eq!(left, 0, "no scratch left in {}", host.display());
        let folder = relift::appimage_runtime::path(cache, arch);
        let parts = std::fs::read_dir(folder.parent().unwrap()).unwrap().count();
        assert_eq!(parts, 1, "only the runtime is in its folder");
    }
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn concurrent_processes_do_not_share_scratch() {
    let exe = std::env::current_exe().expect("the test binary path");
    let run = || {
        std::process::Command::new(&exe)
            .args(["--exact", "parallel_builds_package_with_the_pinned_runtime"])
            .output()
            .expect("the test binary runs")
    };
    let outs: Vec<_> = std::thread::scope(|s| {
        let hs: Vec<_> = (0..2).map(|_| s.spawn(run)).collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    });
    for out in &outs {
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stdout)
        );
    }
}
