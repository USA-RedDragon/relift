//! The NVIDIA program translator as a static library for the program
//! (`gpus/xlate`, `rlx.h`): a Switch 2 game links it so it makes its shader
//! tables on the device it runs on (`runtime/guests/switch/nvn/nvn_xlate.c`)
//! whenever the tables it carries were not made for that device and
//! translator. The library is built with cargo into the cache, for the host
//! or the cross target, the first time and whenever its sources change.

use std::path::{Path, PathBuf};

use crate::import::BoxError;

/// The library's file name for `triple`.
fn library_name(triple: Option<&str>) -> &'static str {
    let msvc = match triple {
        Some(t) => t.ends_with("-msvc"),
        None => cfg!(all(windows, target_env = "msvc")),
    };
    if msvc {
        "relift_gpu_xlate.lib"
    } else {
        "librelift_gpu_xlate.a"
    }
}

/// The cache folder of the library's build for `triple` (`None` for the
/// host).
#[must_use]
pub fn target_dir(cache_root: &Path, triple: Option<&str>) -> PathBuf {
    cache_root.join(match triple {
        Some(t) => format!("gpu-xlate-{t}"),
        None => "gpu-xlate".to_string(),
    })
}

/// The C compiler cargo's `cc` builds the library's C parts with for a cross
/// target, as the variables it reads: Android's NDK clang and its archiver.
fn cross_env(cross: Option<&crate::cross::Build>) -> Vec<(String, PathBuf)> {
    match cross {
        Some(crate::cross::Build::Android(a)) => {
            let t = a.target.triple.replace('-', "_");
            let bin = a
                .ndk
                .clang
                .parent()
                .map(Path::to_path_buf)
                .unwrap_or_default();
            vec![
                (format!("CC_{t}"), a.ndk.clang.clone()),
                (format!("AR_{t}"), bin.join("llvm-ar")),
            ]
        }
        _ => Vec::new(),
    }
}

/// The library built from the relift tree of the runtime `rt` (its
/// `../gpus/xlate`), for the host or `cross`.
///
/// # Errors
///
/// The sources are missing, or cargo fails.
pub fn library(
    cache_root: &Path,
    rt: &Path,
    cross: Option<&crate::cross::Build>,
) -> Result<PathBuf, BoxError> {
    let manifest = rt.join("..").join("gpus").join("xlate").join("Cargo.toml");
    if !manifest.is_file() {
        return Err(format!(
            "{}: the shader translator library's sources are missing",
            manifest.display()
        )
        .into());
    }
    let triple = cross.map(crate::cross::Build::triple);
    let target = target_dir(cache_root, triple);
    let mut cmd = relift_proc::command("cargo");
    cmd.args([
        "build",
        "--release",
        "--locked",
        "--quiet",
        "--manifest-path",
    ])
    .arg(&manifest)
    .current_dir(manifest.parent().unwrap_or(rt))
    .env("CARGO_TARGET_DIR", &target);
    if let Some(t) = triple {
        cmd.args(["--target", t]);
    }
    for (k, v) in cross_env(cross) {
        cmd.env(k, v);
    }
    let out = cmd
        .output()
        .map_err(|e| format!("cargo (for the shader translator library): {e}"))?;
    let dir = triple.map_or(target.clone(), |t| target.join(t));
    let lib = dir.join("release").join(library_name(triple));
    if !out.status.success() || !lib.is_file() {
        let err = String::from_utf8_lossy(&out.stderr);
        let hint = match triple {
            Some(t)
                if err.contains("can't find crate for `std`")
                    || err.contains("may not be installed") =>
            {
                format!("; the Rust std for {t} is missing: run `rustup target add {t}`")
            }
            _ => String::new(),
        };
        return Err(format!(
            "building the shader translator library failed ({}){hint}\n{}",
            out.status,
            err.trim_end()
        )
        .into());
    }
    Ok(lib)
}

/// The system libraries the library needs beyond the runtime's, for a
/// program of `triple` (`None` for the host).
#[must_use]
pub fn system_libs(triple: Option<&str>) -> Vec<String> {
    let windows = triple.map_or(cfg!(windows), |t| t.contains("windows"));
    if windows {
        ["-lunwind", "-lws2_32", "-lntdll", "-luserenv", "-lbcrypt"]
            .map(String::from)
            .into()
    } else if triple.map_or(cfg!(target_vendor = "apple"), |t| t.contains("apple")) {
        Vec::new()
    } else {
        vec!["-ldl".into()]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_library_is_cached_per_target() {
        let root = Path::new("/c");
        assert_eq!(target_dir(root, None), root.join("gpu-xlate"));
        assert_eq!(
            target_dir(root, Some("aarch64-linux-android")),
            root.join("gpu-xlate-aarch64-linux-android")
        );
        assert_eq!(
            library_name(Some("aarch64-linux-android")),
            "librelift_gpu_xlate.a"
        );
        assert_eq!(
            library_name(Some("x86_64-pc-windows-msvc")),
            "relift_gpu_xlate.lib"
        );
    }

    #[test]
    fn windows_programs_take_the_libraries_rust_std_needs() {
        let w = system_libs(Some("x86_64-pc-windows-gnullvm"));
        assert!(w.contains(&"-lbcrypt".to_string()) && w.contains(&"-lunwind".to_string()));
        assert_eq!(system_libs(Some("aarch64-linux-android")), ["-ldl"]);
        assert!(system_libs(Some("aarch64-apple-ios")).is_empty());
    }
}
