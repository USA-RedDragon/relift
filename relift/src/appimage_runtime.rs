//! The AppImage type 2 runtime, pinned.
//!
//! `appimagetool` downloads its runtime from a moving release when it is not
//! given one, on every build. relift gives it the runtime itself: a release
//! of the type 2 runtime named by tag, with the SHA-256 of the file for each
//! architecture, fetched once into `<cache>/sdk/appimage-runtime/<tag>/<arch>`
//! and checked against the hash every time it is used. A build with the file
//! in the cache needs no network.

use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

use crate::import::BoxError;

/// The release of the type 2 runtime that relift ships AppImages with.
pub const TAG: &str = "20251108";

/// The license of the runtime, shipped inside every AppImage.
pub const LICENSE: &str = include_str!("../licenses/type2-runtime-LICENSE");

/// The runtime's project page.
pub const HOME: &str = "https://github.com/AppImage/type2-runtime";

const SHA256: &[(&str, &str)] = &[
    (
        "x86_64",
        "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
    ),
    (
        "aarch64",
        "00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444",
    ),
];

/// The expected SHA-256 of the runtime for `arch`, in hex.
#[must_use]
pub fn expected(arch: &str) -> Option<&'static str> {
    SHA256.iter().find(|(a, _)| *a == arch).map(|(_, h)| *h)
}

/// Where the runtime for `arch` sits in the cache.
#[must_use]
pub fn path(cache_root: &Path, arch: &str) -> PathBuf {
    cache_root
        .join("sdk")
        .join("appimage-runtime")
        .join(TAG)
        .join(arch)
}

/// The URL of the runtime for `arch`.
#[must_use]
pub fn url(arch: &str) -> String {
    format!("{HOME}/releases/download/{TAG}/runtime-{arch}")
}

fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write as _;
    bytes.iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    })
}

/// Whether `file` is a regular file with the SHA-256 `want`.
#[must_use]
pub fn matches(file: &Path, want: &str) -> bool {
    std::fs::read(file).is_ok_and(|b| hex(&Sha256::digest(&b)) == want)
}

/// A name no other build, in this process or another, makes.
pub(crate) fn unique() -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static N: AtomicU64 = AtomicU64::new(0);
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    format!(
        "{}-{t:x}-{}",
        std::process::id(),
        N.fetch_add(1, Ordering::Relaxed)
    )
}

fn download(url: &str, to: &Path) -> Result<(), String> {
    let out = relift_proc::command("curl")
        .args(["--fail", "--silent", "--show-error", "--location"])
        .args([
            "--retry",
            "3",
            "--connect-timeout",
            "20",
            "--max-time",
            "300",
        ])
        .arg("--output")
        .arg(to)
        .arg(url)
        .output()
        .map_err(|e| format!("curl: {e}"))?;
    if out.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string())
    }
}

/// The pinned runtime for `arch`: the cached file when it is there and
/// matches its hash, else fetched, verified and moved into place in one
/// rename, so that builds running at once never see a part.
///
/// # Errors
///
/// The architecture has no pinned runtime, or the file is not in the cache
/// and cannot be fetched or does not match its hash.
pub fn ensure(cache_root: &Path, arch: &str) -> Result<PathBuf, BoxError> {
    let want = expected(arch).ok_or_else(|| {
        format!(
            "no AppImage runtime is pinned for {arch}; package.appimage=never skips the AppImage"
        )
    })?;
    fetch_once(&path(cache_root, arch), &url(arch), want)
}

fn fetch_once(dest: &Path, url: &str, want: &str) -> Result<PathBuf, BoxError> {
    if matches(dest, want) {
        return Ok(dest.to_path_buf());
    }
    let dir = dest.parent().ok_or("the runtime has no folder")?;
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let part = dir.join(format!(".part-{}", unique()));
    let got = download(url, &part).and_then(|()| {
        if matches(&part, want) {
            Ok(())
        } else {
            Err(format!(
                "the file does not have the pinned SHA-256 {want}; it was not used"
            ))
        }
    });
    let placed = got.and_then(|()| std::fs::rename(&part, dest).map_err(|e| e.to_string()));
    let _ = std::fs::remove_file(&part);
    placed.map_err(|e| {
        format!(
            "the AppImage runtime {TAG} is not in the cache and could not be fetched from {url}: {e}\n\
             Download it on a machine with network access and put it at {} (SHA-256 {want}), or set package.appimage=never",
            dest.display()
        )
    })?;
    Ok(dest.to_path_buf())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../target/tmp")
            .join(format!("appimage-runtime-{name}"));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn every_pinned_hash_is_a_sha256() {
        for (a, h) in SHA256 {
            assert!(
                h.len() == 64 && h.bytes().all(|b| b.is_ascii_hexdigit()),
                "{a}"
            );
        }
        assert!(expected("x86_64").is_some() && expected("aarch64").is_some());
        assert!(expected("riscv64").is_none());
    }

    #[test]
    fn the_runtime_lives_under_the_sdk_by_tag_and_arch() {
        let p = path(Path::new("/c"), "x86_64");
        assert_eq!(
            p,
            Path::new("/c/sdk/appimage-runtime")
                .join(TAG)
                .join("x86_64")
        );
        assert!(url("aarch64").ends_with(&format!("/{TAG}/runtime-aarch64")));
        assert!(!url("x86_64").contains("continuous"));
    }

    #[test]
    fn a_file_matches_only_its_own_hash() {
        let d = scratch("matches");
        let f = d.join("f");
        relift_cache::write_if_changed(&f, b"abc").unwrap();
        assert!(matches(&f, ABC));
        assert!(!matches(&f, &"0".repeat(64)));
        assert!(!matches(&d.join("missing"), ABC));
    }

    const ABC: &str = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    #[test]
    fn a_cached_runtime_needs_no_network() {
        let d = scratch("cached");
        let dest = d.join("a/b/rt");
        std::fs::create_dir_all(dest.parent().unwrap()).unwrap();
        relift_cache::write_if_changed(&dest, b"abc").unwrap();
        let got = fetch_once(&dest, "http://127.0.0.1:9/unreachable", ABC).unwrap();
        assert_eq!(got, dest);
    }

    #[test]
    fn a_fetched_runtime_is_checked_then_placed_whole() {
        let d = scratch("fetch");
        let src = d.join("src");
        relift_cache::write_if_changed(&src, b"abc").unwrap();
        let url = format!("file://{}", src.display());
        let dest = d.join("sdk/rt");
        assert!(fetch_once(&dest, &url, &"0".repeat(64)).is_err());
        assert!(!dest.exists());
        assert_eq!(
            std::fs::read_dir(dest.parent().unwrap()).unwrap().count(),
            0
        );
        fetch_once(&dest, &url, ABC).unwrap();
        assert_eq!(std::fs::read(&dest).unwrap(), b"abc");
        assert_eq!(
            std::fs::read_dir(dest.parent().unwrap()).unwrap().count(),
            1
        );
    }

    #[test]
    fn no_network_and_no_cache_says_where_to_put_the_file() {
        let d = scratch("offline");
        let dest = d.join("sdk/rt");
        let e = fetch_once(&dest, "http://127.0.0.1:9/x", ABC)
            .unwrap_err()
            .to_string();
        assert!(e.contains("not in the cache") && e.contains("package.appimage=never"));
        assert!(e.contains(&dest.display().to_string()) && e.contains(ABC));
    }

    #[test]
    fn an_architecture_without_a_runtime_is_said_plainly() {
        let e = ensure(&scratch("noarch"), "riscv64")
            .unwrap_err()
            .to_string();
        assert!(e.contains("riscv64") && e.contains("package.appimage=never"));
    }
}
