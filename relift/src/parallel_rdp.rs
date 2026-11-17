//! parallel-rdp, pinned.
//!
//! The Nintendo 64 runtime draws the RDP and scans out the VI with
//! parallel-rdp (MIT), built from its source as a C++ unit of the runtime.
//! relift does not vendor that source: it names one snapshot of the
//! standalone tree by commit, with the SHA-256 of its archive, fetches the
//! archive once into `<cache>/sdk/parallel-rdp/` and unpacks it there. A
//! build with the tree in the cache needs no network; an archive put at
//! [`archive`] by hand is unpacked without one.

use std::path::{Path, PathBuf};

use crate::appimage_runtime::{download, matches, unique};
use crate::import::BoxError;

/// The commit of `parallel-rdp-standalone` relift builds; it carries
/// parallel-rdp at [`UPSTREAM`].
pub const COMMIT: &str = "388d70f5835b352d841d9d9e5a08c5de01470f41";

/// The parallel-rdp commit the standalone tree holds.
pub const UPSTREAM: &str = "1cecd042b2619bc505c12bfdc713808386f2b54d";

/// The SHA-256 of the archive at [`url`].
pub const SHA256: &str = "0bced396618af285e7b3a78a20e624d18779f11433ef4b46192a0e2021fe00d5";

/// The project page.
pub const HOME: &str = "https://github.com/Themaister/parallel-rdp-standalone";

/// The C++ sources the runtime builds (relative to the tree), as the
/// standalone `config.mk` lists them.
pub const SOURCES: &[&str] = &[
    "parallel-rdp/command_ring.cpp",
    "parallel-rdp/rdp_device.cpp",
    "parallel-rdp/rdp_dump_write.cpp",
    "parallel-rdp/rdp_renderer.cpp",
    "parallel-rdp/video_interface.cpp",
    "vulkan/buffer.cpp",
    "vulkan/buffer_pool.cpp",
    "vulkan/command_buffer.cpp",
    "vulkan/command_pool.cpp",
    "vulkan/context.cpp",
    "vulkan/cookie.cpp",
    "vulkan/descriptor_set.cpp",
    "vulkan/device.cpp",
    "vulkan/event_manager.cpp",
    "vulkan/fence.cpp",
    "vulkan/fence_manager.cpp",
    "vulkan/image.cpp",
    "vulkan/indirect_layout.cpp",
    "vulkan/memory_allocator.cpp",
    "vulkan/pipeline_event.cpp",
    "vulkan/query_pool.cpp",
    "vulkan/render_pass.cpp",
    "vulkan/sampler.cpp",
    "vulkan/semaphore.cpp",
    "vulkan/semaphore_manager.cpp",
    "vulkan/shader.cpp",
    "vulkan/texture/texture_format.cpp",
    "util/arena_allocator.cpp",
    "util/logging.cpp",
    "util/thread_id.cpp",
    "util/aligned_alloc.cpp",
    "util/timer.cpp",
    "util/timeline_trace_file.cpp",
    "util/environment.cpp",
    "util/thread_name.cpp",
];

/// The C source of the Vulkan loader shim the tree carries.
pub const VOLK: &str = "volk/volk.c";

/// The include directories of the tree.
pub const INCLUDES: [&str; 5] = [
    "parallel-rdp",
    "vulkan",
    "vulkan-headers/include",
    "volk",
    "util",
];

/// A header that gives every Vulkan function pointer volk declares in the
/// tree at `dir` a name of relift's own (`vkCreateDevice` becomes
/// `rl_n64_prdp_vkCreateDevice`). Included first in parallel-rdp's sources,
/// it keeps volk's globals apart from the functions of a Vulkan loader the
/// program links (the static MoltenVK of an iOS app), which have the same
/// names.
///
/// # Errors
///
/// The tree has no `volk/volk.h`.
pub fn renames_header(dir: &Path) -> Result<String, BoxError> {
    let path = dir.join("volk").join("volk.h");
    let text = std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(renames(&text))
}

fn renames(volk_h: &str) -> String {
    let mut names: Vec<&str> = volk_h
        .lines()
        .filter_map(|l| l.trim().strip_prefix("extern PFN_vk"))
        .filter_map(|l| l.split_whitespace().nth(1))
        .filter_map(|n| n.strip_suffix(';'))
        .filter(|n| n.starts_with("vk"))
        .collect();
    names.sort_unstable();
    names.dedup();
    let mut out = String::from(
        "/* Written by relift build from parallel-rdp's volk.h: relift's names for volk's Vulkan function pointers. */\n#pragma once\n",
    );
    for n in names {
        out.push_str(&format!("#define {n} rl_n64_prdp_{n}\n"));
    }
    out
}

/// The URL of the archive.
#[must_use]
pub fn url() -> String {
    format!("{HOME}/archive/{COMMIT}.tar.gz")
}

/// Where the unpacked tree sits in the cache.
#[must_use]
pub fn tree(cache_root: &Path) -> PathBuf {
    cache_root.join("sdk").join("parallel-rdp").join(COMMIT)
}

/// Where the archive sits in the cache while it is unpacked, and where one
/// fetched by hand goes.
#[must_use]
pub fn archive(cache_root: &Path) -> PathBuf {
    cache_root
        .join("sdk")
        .join("parallel-rdp")
        .join(format!("{COMMIT}.tar.gz"))
}

const DONE: &str = ".relift-unpacked";

fn complete(dir: &Path) -> bool {
    std::fs::read_to_string(dir.join(DONE)).is_ok_and(|s| s.trim() == SHA256)
        && dir.join("parallel-rdp").join("rdp_device.cpp").is_file()
}

/// The parallel-rdp tree to build: `RELIFT_PARALLEL_RDP_DIR` when it is
/// set (a working copy, used as it is), else the pinned tree in the cache,
/// fetched, checked and unpacked first when it is not there.
///
/// # Errors
///
/// The tree is not in the cache and the archive cannot be fetched, does not
/// match its hash, or cannot be unpacked.
pub fn ensure(cache_root: &Path) -> Result<PathBuf, BoxError> {
    if let Some(dir) = std::env::var_os("RELIFT_PARALLEL_RDP_DIR").filter(|v| !v.is_empty()) {
        let dir = PathBuf::from(dir);
        if !dir.join("parallel-rdp").join("rdp_device.cpp").is_file() {
            return Err(format!(
                "RELIFT_PARALLEL_RDP_DIR={}: not a parallel-rdp standalone tree",
                dir.display()
            )
            .into());
        }
        return Ok(dir);
    }
    let dest = tree(cache_root);
    if complete(&dest) {
        return Ok(dest);
    }
    let tarball = archive(cache_root);
    let parent = tarball.parent().ok_or("the cache has no sdk folder")?;
    std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    let hint = || {
        format!(
            "Download {} on a machine with network access and put it at {} (SHA-256 {SHA256})",
            url(),
            tarball.display()
        )
    };
    if !matches(&tarball, SHA256) {
        let part = parent.join(format!(".part-{}", unique()));
        let got = download(&url(), &part).and_then(|()| {
            if matches(&part, SHA256) {
                std::fs::rename(&part, &tarball).map_err(|e| e.to_string())
            } else {
                Err(format!(
                    "the archive does not have the pinned SHA-256 {SHA256}; it was not used"
                ))
            }
        });
        let _ = std::fs::remove_file(&part);
        got.map_err(|e| {
            format!(
                "parallel-rdp {COMMIT} is not in the cache and could not be fetched from {}: {e}\n{}",
                url(),
                hint()
            )
        })?;
    }
    let work = parent.join(format!(".unpack-{}", unique()));
    let unpacked = unpack(&tarball, &work).and_then(|()| {
        relift_cache::write_if_changed(&work.join(DONE), SHA256.as_bytes())
            .map_err(|e| e.to_string())?;
        match std::fs::rename(&work, &dest) {
            Ok(()) => Ok(()),
            Err(_) if complete(&dest) => Ok(()),
            Err(e) => {
                let _ = std::fs::remove_dir_all(&dest);
                std::fs::rename(&work, &dest).map_err(|f| format!("{e}; {f}"))
            }
        }
    });
    let _ = std::fs::remove_dir_all(&work);
    unpacked.map_err(|e| format!("parallel-rdp: could not unpack {}: {e}", tarball.display()))?;
    Ok(dest)
}

fn unpack(tarball: &Path, into: &Path) -> Result<(), String> {
    std::fs::create_dir_all(into).map_err(|e| format!("{}: {e}", into.display()))?;
    let out = relift_proc::command("tar")
        .arg("-xzf")
        .arg(tarball)
        .arg("-C")
        .arg(into)
        .arg("--strip-components=1")
        .output()
        .map_err(|e| format!("tar: {e}"))?;
    if out.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_volk_function_pointer_gets_a_name_of_its_own() {
        let h = renames(
            "extern PFN_vkCreateDevice vkCreateDevice;\n  extern PFN_vkCmdDraw vkCmdDraw;\nextern PFN_vkCreateDevice vkCreateDevice;\nPFN_vkQueueSubmit vkQueueSubmit;\n",
        );
        assert!(
            h.contains("#define vkCreateDevice rl_n64_prdp_vkCreateDevice\n"),
            "{h}"
        );
        assert!(
            h.contains("#define vkCmdDraw rl_n64_prdp_vkCmdDraw\n"),
            "{h}"
        );
        assert_eq!(h.matches("vkCreateDevice rl_").count(), 1, "{h}");
        assert!(!h.contains("vkQueueSubmit"), "{h}");
    }

    #[test]
    fn the_pin_is_a_commit_and_a_sha256() {
        for h in [COMMIT, UPSTREAM] {
            assert!(h.len() == 40 && h.bytes().all(|b| b.is_ascii_hexdigit()));
        }
        assert!(SHA256.len() == 64 && SHA256.bytes().all(|b| b.is_ascii_hexdigit()));
        assert!(url().starts_with("https://") && url().contains(COMMIT));
    }

    #[test]
    fn the_tree_and_archive_live_under_the_sdk_by_commit() {
        let c = Path::new("/c");
        assert_eq!(tree(c), Path::new("/c/sdk/parallel-rdp").join(COMMIT));
        assert_eq!(
            archive(c),
            Path::new("/c/sdk/parallel-rdp").join(format!("{COMMIT}.tar.gz"))
        );
    }

    #[test]
    fn every_source_is_a_relative_path_in_the_tree() {
        for s in SOURCES.iter().chain([&VOLK]) {
            assert!(!s.starts_with('/') && !s.contains(".."), "{s}");
            assert!(s.ends_with(".cpp") || s.ends_with(".c"), "{s}");
        }
    }
}
