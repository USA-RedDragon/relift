//! The optional parts of the runtime a game links besides the three layers:
//! Vulkan forwarding (always) and the SDL3 window backend (when SDL3 can be
//! found for the target).
//!
//! SDL3 comes from the first of these that exists:
//! 1. `RELIFT_SDL3_DIR`, a directory with `include/` and `lib/`;
//! 2. `<cache>/sdk/sdl3/<arch>-<os>/`, the same layout, where the pinned
//!    prebuilt SDL3 for the target is unpacked;
//! 3. `pkg-config` (`sdl3`), for development machines.
//!
//! With none of them the game builds without a window backend and runs
//! headless.
//!
//! On Apple hosts the runtime links MoltenVK directly instead of loading a
//! Vulkan loader at run time (`-DRL_VULKAN_LINKED`), so the program loads no
//! code at run time and the same path serves iOS. MoltenVK comes from the
//! first of `RELIFT_MOLTENVK_DIR`, `<cache>/sdk/moltenvk/<arch>-<os>/` and
//! Homebrew's `molten-vk`, each a directory with `lib/libMoltenVK.dylib`.
//! Without one the runtime looks for a loader at run time.
//!
//! SDL3 also plays the game's audio. libopus, which decodes the Opus streams
//! of games that use them, comes from `RELIFT_OPUS_DIR` (a directory with
//! `include/opus/` and `lib/`) or `pkg-config` (`opus`). Without SDL3 the
//! audio output is a silent one that keeps time; without libopus Opus
//! packets decode to silence.
//!
//! libvpx's VP9 decoder, which decodes the pictures a Switch game's NVDEC
//! work submits, is compiled from source: `RELIFT_LIBVPX_DIR` or
//! `<cache>/sdk/libvpx/<version>/`, which a Switch build fills from the
//! pinned release the first time (`ensure_video_sources`), plain C only,
//! with the generic configuration in `runtime/host/vpx`. With
//! `RELIFT_LIBVPX_DIR=off` the pictures stay undecoded.
//!
//! dav1d's AV1 decoder, which decodes the movies of Switch 2 games, is
//! compiled the same way: `RELIFT_DAV1D_DIR` or
//! `<cache>/sdk/dav1d/<version>/`, filled the same way, plain C only,
//! with the generic configuration in `runtime/host/dav1d`. With
//! `RELIFT_DAV1D_DIR=off` AV1 movies stay undecoded.

use std::path::{Path, PathBuf};

use crate::import::BoxError;
use relift_build::Compile;

/// Where SDL3 was found and how to compile and link against it.
#[derive(Debug, PartialEq, Eq)]
pub struct Sdl3 {
    pub cflags: Vec<String>,
    pub libs: Vec<String>,
    pub origin: String,
}

/// The Vulkan implementation the program links instead of loading one.
#[derive(Debug, PartialEq, Eq)]
pub struct LinkedVulkan {
    /// The link flags that bring it in.
    pub libs: Vec<String>,
    pub origin: String,
}

/// How to compile and link against libopus.
#[derive(Debug, PartialEq, Eq)]
pub struct Opus {
    pub cflags: Vec<String>,
    pub libs: Vec<String>,
    pub origin: String,
}

/// The pinned libvpx release whose VP9 decoder the runtime compiles in.
pub const LIBVPX_VERSION: &str = "1.17.0";

/// The libvpx files the VP9 decoder needs, besides `vp9_decodeframe.c`,
/// which `runtime/host/vp9_libvpx.c` includes.
const LIBVPX_SOURCES: [&str; 40] = [
    "vp9/common/vp9_alloccommon.c",
    "vp9/common/vp9_blockd.c",
    "vp9/common/vp9_common_data.c",
    "vp9/common/vp9_entropy.c",
    "vp9/common/vp9_entropymode.c",
    "vp9/common/vp9_entropymv.c",
    "vp9/common/vp9_filter.c",
    "vp9/common/vp9_frame_buffers.c",
    "vp9/common/vp9_idct.c",
    "vp9/common/vp9_loopfilter.c",
    "vp9/common/vp9_mvref_common.c",
    "vp9/common/vp9_pred_common.c",
    "vp9/common/vp9_quant_common.c",
    "vp9/common/vp9_reconinter.c",
    "vp9/common/vp9_reconintra.c",
    "vp9/common/vp9_rtcd.c",
    "vp9/common/vp9_scale.c",
    "vp9/common/vp9_scan.c",
    "vp9/common/vp9_seg_common.c",
    "vp9/common/vp9_thread_common.c",
    "vp9/common/vp9_tile_common.c",
    "vp9/decoder/vp9_decodemv.c",
    "vp9/decoder/vp9_decoder.c",
    "vp9/decoder/vp9_detokenize.c",
    "vp9/decoder/vp9_dsubexp.c",
    "vp9/decoder/vp9_job_queue.c",
    "vpx_dsp/bitreader_buffer.c",
    "vpx_dsp/bitreader.c",
    "vpx_dsp/intrapred.c",
    "vpx_dsp/inv_txfm.c",
    "vpx_dsp/loopfilter.c",
    "vpx_dsp/prob.c",
    "vpx_dsp/vpx_convolve.c",
    "vpx_dsp/vpx_dsp_rtcd.c",
    "vpx_mem/vpx_mem.c",
    "vpx_scale/generic/yv12config.c",
    "vpx_scale/generic/yv12extend.c",
    "vpx_scale/vpx_scale_rtcd.c",
    "vpx/src/vpx_codec.c",
    "vpx_util/vpx_thread.c",
];

/// The pinned dav1d release whose AV1 decoder the runtime compiles in.
pub const DAV1D_VERSION: &str = "1.5.3";

/// The dav1d files that do not depend on the bit depth.
const DAV1D_SOURCES: [&str; 26] = [
    "src/cdf.c",
    "src/cpu.c",
    "src/ctx.c",
    "src/data.c",
    "src/decode.c",
    "src/dequant_tables.c",
    "src/getbits.c",
    "src/intra_edge.c",
    "src/itx_1d.c",
    "src/lf_mask.c",
    "src/lib.c",
    "src/log.c",
    "src/mem.c",
    "src/msac.c",
    "src/obu.c",
    "src/pal.c",
    "src/picture.c",
    "src/qm.c",
    "src/ref.c",
    "src/refmvs.c",
    "src/scan.c",
    "src/tables.c",
    "src/thread_task.c",
    "src/warpmv.c",
    "src/wedge.c",
    "src/win32/thread.c",
];

/// The dav1d files compiled once per bit depth (8 and 16).
const DAV1D_BITDEPTH_SOURCES: [&str; 13] = [
    "src/cdef_apply_tmpl.c",
    "src/cdef_tmpl.c",
    "src/fg_apply_tmpl.c",
    "src/filmgrain_tmpl.c",
    "src/ipred_prepare_tmpl.c",
    "src/ipred_tmpl.c",
    "src/itx_tmpl.c",
    "src/lf_apply_tmpl.c",
    "src/loopfilter_tmpl.c",
    "src/looprestoration_tmpl.c",
    "src/lr_apply_tmpl.c",
    "src/mc_tmpl.c",
    "src/recon_tmpl.c",
];

/// The x86-64 assembly files of dav1d: first those of every bit depth, then 8 bit only, then 16 bit only.
/// They are NASM syntax; dav1d picks among their SSE, AVX2 and AVX-512 versions at run time from the CPU's
/// features, so one binary is right on every x86-64 CPU.
const DAV1D_X86_ASM: [&str; 9] = [
    "cpuid",
    "msac",
    "pal",
    "refmvs",
    "itx_avx512",
    "cdef_avx2",
    "itx_avx2",
    "cdef_sse",
    "itx_sse",
];
const DAV1D_X86_ASM_8: [&str; 16] = [
    "cdef_avx512",
    "filmgrain_avx512",
    "ipred_avx512",
    "loopfilter_avx512",
    "looprestoration_avx512",
    "mc_avx512",
    "filmgrain_avx2",
    "ipred_avx2",
    "loopfilter_avx2",
    "looprestoration_avx2",
    "mc_avx2",
    "filmgrain_sse",
    "ipred_sse",
    "loopfilter_sse",
    "looprestoration_sse",
    "mc_sse",
];
const DAV1D_X86_ASM_16: [&str; 21] = [
    "cdef16_avx512",
    "filmgrain16_avx512",
    "ipred16_avx512",
    "itx16_avx512",
    "loopfilter16_avx512",
    "looprestoration16_avx512",
    "mc16_avx512",
    "cdef16_avx2",
    "filmgrain16_avx2",
    "ipred16_avx2",
    "itx16_avx2",
    "loopfilter16_avx2",
    "looprestoration16_avx2",
    "mc16_avx2",
    "cdef16_sse",
    "filmgrain16_sse",
    "ipred16_sse",
    "itx16_sse",
    "loopfilter16_sse",
    "looprestoration16_sse",
    "mc16_sse",
];

/// The arm64 assembly files of dav1d (GNU assembler syntax, through the C
/// compiler): first those of every bit depth, then 8 bit, then 16 bit. dav1d
/// picks the dot product and SVE versions at run time from the CPU's
/// features, so one library is right on every arm64 CPU.
const DAV1D_ARM64_ASM: [&str; 19] = [
    "itx",
    "looprestoration_common",
    "msac",
    "refmvs",
    "cdef",
    "filmgrain",
    "ipred",
    "loopfilter",
    "looprestoration",
    "mc",
    "mc_dotprod",
    "cdef16",
    "filmgrain16",
    "ipred16",
    "itx16",
    "loopfilter16",
    "looprestoration16",
    "mc16",
    "mc16_sve",
];

/// How dav1d's assembly is built on a host: the assembler, the object format it writes and the configuration
/// directory under `runtime/host/dav1d` (its `config.h` and `config.asm`). The arm64 assembly goes through the C
/// compiler: no assembler and no format.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Dav1dAsm {
    /// NASM's command.
    pub nasm: Vec<String>,
    /// NASM's `-f` format.
    pub format: &'static str,
    /// The configuration directory name.
    pub config: &'static str,
}

/// The assembly dav1d is built with for a native build on this machine: x86-64 Linux with NASM 2.14 or later on
/// the path. Every other host, and a host without NASM, builds dav1d's plain C, which decodes the same pictures
/// several times slower.
fn native_dav1d_asm() -> Option<Dav1dAsm> {
    if !cfg!(all(target_arch = "x86_64", target_os = "linux")) {
        return None;
    }
    let nasm = relift_build::find_tool("nasm")?;
    let out = relift_proc::command(&nasm).arg("-v").output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    let version = text.split_whitespace().nth(2)?;
    let mut parts = version.split('.').map(|p| p.parse::<u32>().ok());
    let (major, minor) = (parts.next()??, parts.next()??);
    if (major, minor) < (2, 14) {
        return None;
    }
    Some(Dav1dAsm {
        nasm: vec![nasm.display().to_string()],
        format: "elf64",
        config: "x86_64",
    })
}

/// The assembly dav1d is built with for an arm64 target (Android): its GNU syntax files go through the target's
/// C compiler, with the configuration in `runtime/host/dav1d/aarch64`.
#[must_use]
pub fn aarch64_dav1d_asm() -> Dav1dAsm {
    Dav1dAsm {
        nasm: Vec::new(),
        format: "",
        config: "aarch64",
    }
}

/// A dav1d source tree.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Dav1d {
    pub src: PathBuf,
    pub origin: String,
}

fn dav1d_in(dir: &Path, origin: String) -> Option<Dav1d> {
    dir.join("src").join("lib.c").is_file().then(|| Dav1d {
        src: dir.to_path_buf(),
        origin,
    })
}

/// The dav1d source tree, or `None`.
pub fn find_dav1d(cache_root: &Path) -> Option<Dav1d> {
    if let Some(d) = std::env::var_os(DAV1D_PIN.env).filter(|v| !v.is_empty()) {
        return dav1d_in(Path::new(&d), DAV1D_PIN.env.into());
    }
    dav1d_in(
        &pinned_dir(cache_root, &DAV1D_PIN),
        format!("{DAV1D_VERSION}, pinned source"),
    )
}

/// A libvpx source tree.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Vpx {
    pub src: PathBuf,
    pub origin: String,
}

fn vpx_in(dir: &Path, origin: String) -> Option<Vpx> {
    dir.join("vp9")
        .join("decoder")
        .join("vp9_decodeframe.c")
        .is_file()
        .then(|| Vpx {
            src: dir.to_path_buf(),
            origin,
        })
}

/// The libvpx source tree, or `None`.
pub fn find_vpx(cache_root: &Path) -> Option<Vpx> {
    if let Some(d) = std::env::var_os(LIBVPX_PIN.env).filter(|v| !v.is_empty()) {
        return vpx_in(Path::new(&d), LIBVPX_PIN.env.into());
    }
    vpx_in(
        &pinned_dir(cache_root, &LIBVPX_PIN),
        format!("{LIBVPX_VERSION}, pinned source"),
    )
}

/// A source release that `relift build` fetches into the cache by itself.
pub struct Pinned {
    /// The library's name, for messages.
    pub name: &'static str,
    /// The variable that names another source tree, or `off`.
    pub env: &'static str,
    pub version: &'static str,
    pub url: &'static str,
    pub sha256: &'static str,
    /// The folder the release tarball holds everything in.
    pub top: &'static str,
}

/// The pinned libvpx release.
pub const LIBVPX_PIN: Pinned = Pinned {
    name: "libvpx",
    env: "RELIFT_LIBVPX_DIR",
    version: LIBVPX_VERSION,
    url: "https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz",
    sha256: "1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a",
    top: "libvpx-1.17.0",
};

/// The pinned dav1d release.
pub const DAV1D_PIN: Pinned = Pinned {
    name: "dav1d",
    env: "RELIFT_DAV1D_DIR",
    version: DAV1D_VERSION,
    url: "https://download.videolan.org/pub/videolan/dav1d/1.5.3/dav1d-1.5.3.tar.xz",
    sha256: "732010aa5ef461fa93355ed2c6c5fedb48ddc4b74e697eaabe8907eaeb943011",
    top: "dav1d-1.5.3",
};

/// Where `p`'s source tree is kept in the cache.
#[must_use]
pub fn pinned_dir(cache_root: &Path, p: &Pinned) -> PathBuf {
    cache_root.join("sdk").join(p.name).join(p.version)
}

/// Makes sure a Switch build has the video decoders' sources: the pinned
/// libvpx (VP9) and dav1d (AV1) releases, fetched into the cache the first
/// time, checked against their SHA-256 and unpacked into place in one
/// rename, so builds running at once never see a part. A decoder whose
/// variable names a tree of its own, or `off`, is left to that.
///
/// # Errors
///
/// A source is not in the cache and cannot be fetched, does not match its
/// hash, or does not unpack.
pub fn ensure_video_sources(cache_root: &Path) -> Result<(), BoxError> {
    ensure_pinned(cache_root, &LIBVPX_PIN, |r| find_vpx(r).is_some())?;
    ensure_dav1d_source(cache_root)
}

/// The dav1d part of [`ensure_video_sources`].
///
/// # Errors
///
/// As [`ensure_video_sources`].
pub fn ensure_dav1d_source(cache_root: &Path) -> Result<(), BoxError> {
    ensure_pinned(cache_root, &DAV1D_PIN, |r| find_dav1d(r).is_some())
}

fn ensure_pinned(
    cache_root: &Path,
    p: &Pinned,
    found: impl Fn(&Path) -> bool,
) -> Result<(), BoxError> {
    if std::env::var_os(p.env).is_some_and(|v| !v.is_empty()) || found(cache_root) {
        return Ok(());
    }
    let dest = pinned_dir(cache_root, p);
    let got = fetch_pinned(p, &dest, || found(cache_root));
    if got.is_err() && found(cache_root) {
        return Ok(());
    }
    got.map_err(|e| {
        format!(
            "the {} {} source is not in the cache and could not be fetched from {}: {e}\n\
             Download it on a machine with network access, unpack it so that {} holds the \
             release's {} folder's contents (SHA-256 of the tarball {}), or set {}=off to \
             build without it",
            p.name,
            p.version,
            p.url,
            dest.display(),
            p.top,
            p.sha256,
            p.env
        )
        .into()
    })
}

/// Fetches `p` into a part folder beside `dest` and renames it into place.
/// A rename that loses to another build of the same source is a success
/// when `complete` says the tree is whole; a folder left at `dest` that is
/// not whole (a crash, an older relift) is removed and replaced.
fn fetch_pinned(p: &Pinned, dest: &Path, complete: impl Fn() -> bool) -> Result<(), String> {
    let dir = dest.parent().ok_or("the source has no folder")?;
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let part = dir.join(format!(".part-{}", crate::appimage_runtime::unique()));
    let tarball = part.with_extension("tar");
    let got = (|| {
        crate::appimage_runtime::download(p.url, &tarball)?;
        if !crate::appimage_runtime::matches(&tarball, p.sha256) {
            return Err(format!(
                "the download does not have the pinned SHA-256 {}; it was not used",
                p.sha256
            ));
        }
        unpack(&tarball, &part)?;
        let from = part.join(p.top);
        let mut tries = 0;
        loop {
            match std::fs::rename(&from, dest) {
                Ok(()) => break Ok(()),
                Err(_) if complete() => break Ok(()),
                Err(e) if tries >= 3 => break Err(format!("{}: {e}", dest.display())),
                Err(_) => {
                    tries += 1;
                    let _ = std::fs::remove_dir_all(dest);
                }
            }
        }
    })();
    let _ = std::fs::remove_file(&tarball);
    let _ = std::fs::remove_dir_all(&part);
    got
}

/// Why `p`'s source is not in a build: turned off, or not where its
/// variable or the cache says.
fn missing(p: &Pinned) -> String {
    match std::env::var(p.env) {
        Ok(v) if v == "off" => format!("off ({}=off)", p.env),
        Ok(v) if !v.is_empty() => format!("not found in {v} ({})", p.env),
        _ => "not in the cache".into(),
    }
}

/// Unpacks the tarball `file` into a new folder `into`, with the system's
/// `tar`, which reads gzip and xz on every host relift builds on.
fn unpack(file: &Path, into: &Path) -> Result<(), String> {
    std::fs::create_dir_all(into).map_err(|e| format!("{}: {e}", into.display()))?;
    let out = relift_proc::command("tar")
        .arg("-xf")
        .arg(file)
        .arg("-C")
        .arg(into)
        .output()
        .map_err(|e| format!("tar: {e}"))?;
    if out.status.success() {
        Ok(())
    } else {
        Err(format!(
            "tar: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        ))
    }
}

/// The extra runtime sources, flags and libraries of one build.
#[derive(Debug, PartialEq, Eq)]
pub struct Extras {
    pub vk: Vec<PathBuf>,
    pub window: Option<PathBuf>,
    pub sdl3: Option<Sdl3>,
    pub vulkan: Option<LinkedVulkan>,
    /// Host backends beyond the window (sound, Android glue), compiled with
    /// SDL3's flags.
    pub host: Vec<PathBuf>,
    pub opus_src: Option<PathBuf>,
    pub opus: Option<Opus>,
    /// libadrenotools, which lets `video.gpu_driver` load a custom Vulkan
    /// driver package on Android.
    pub adrenotools: Option<crate::android::Adrenotools>,
    /// libvpx, compiled in for VP9 decoding.
    pub vpx: Option<Vpx>,
    /// dav1d, compiled in for AV1 decoding.
    pub dav1d: Option<Dav1d>,
    /// The assembly dav1d is built with on this host, or `None` for its plain C.
    pub dav1d_asm: Option<Dav1dAsm>,
    /// Whether the runtime has video decoders at all (a Switch runtime), so
    /// the note says what became of them.
    pub video: bool,
    /// The shader translator library (`crate::xlate`) a Switch 2 program
    /// links to make its shader tables on the device, and the flags it
    /// links with.
    pub xlate: Option<Xlate>,
}

/// The shader translator library a program links (see [`Extras::with_xlate`]).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Xlate {
    /// The static library.
    pub lib: PathBuf,
    /// Its C header's folder.
    pub include: PathBuf,
    /// The system libraries it needs.
    pub libs: Vec<String>,
}

fn c_files(dir: &Path) -> std::io::Result<Vec<PathBuf>> {
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e),
    };
    let mut v: Vec<PathBuf> = entries
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "c"))
        .collect();
    v.sort();
    Ok(v)
}

fn from_dir(dir: &Path, origin: String) -> Option<Sdl3> {
    if !dir.join("include").join("SDL3").join("SDL.h").is_file() {
        return None;
    }
    let lib = dir.join("lib");
    let lib_flag = lib.display().to_string();
    Some(Sdl3 {
        cflags: vec![format!("-I{}", dir.join("include").display())],
        libs: vec![
            format!("-L{lib_flag}"),
            format!("-Wl,-rpath,{lib_flag}"),
            "-lSDL3".into(),
        ],
        origin,
    })
}

fn pkg_config(flag: &str, package: &str) -> Option<Vec<String>> {
    let out = relift_proc::command("pkg-config")
        .args([flag, package])
        .output()
        .ok()
        .filter(|o| o.status.success())?;
    let text = String::from_utf8(out.stdout).ok()?;
    Some(
        text.split_whitespace()
            .map(String::from)
            .collect::<Vec<_>>(),
    )
}

fn from_pkg_config() -> Option<Sdl3> {
    Some(Sdl3 {
        cflags: pkg_config("--cflags", "sdl3")?,
        libs: pkg_config("--libs", "sdl3")?,
        origin: "pkg-config".into(),
    })
}

fn opus_from_dir(dir: &Path, origin: String) -> Option<Opus> {
    if !dir.join("include").join("opus").join("opus.h").is_file() {
        return None;
    }
    let lib = dir.join("lib");
    let lib_flag = lib.display().to_string();
    Some(Opus {
        cflags: vec![format!("-I{}", dir.join("include").display())],
        libs: vec![
            format!("-L{lib_flag}"),
            format!("-Wl,-rpath,{lib_flag}"),
            "-lopus".into(),
        ],
        origin,
    })
}

/// pkg-config names the `opus` subdirectory of the include path, but the
/// runtime includes `<opus/opus.h>`: add the parent of each such directory.
fn opus_cflags(flags: Vec<String>) -> Vec<String> {
    let parents: Vec<String> = flags
        .iter()
        .filter_map(|f| f.strip_prefix("-I"))
        .filter_map(|d| d.strip_suffix("/opus"))
        .map(|d| format!("-I{d}"))
        .collect();
    flags.into_iter().chain(parents).collect()
}

/// libopus for the host target, or `None`.
pub fn find_opus() -> Option<Opus> {
    if let Some(d) = std::env::var_os("RELIFT_OPUS_DIR").filter(|v| !v.is_empty()) {
        return opus_from_dir(Path::new(&d), "RELIFT_OPUS_DIR".into());
    }
    Some(Opus {
        cflags: opus_cflags(pkg_config("--cflags", "opus")?),
        libs: pkg_config("--libs", "opus")?,
        origin: "pkg-config".into(),
    })
}

/// SDL3 for the host target, or `None`.
pub fn find_sdl3(cache_root: &Path) -> Option<Sdl3> {
    if let Some(d) = std::env::var_os("RELIFT_SDL3_DIR").filter(|v| !v.is_empty()) {
        return from_dir(Path::new(&d), "RELIFT_SDL3_DIR".into());
    }
    let target = format!("{}-{}", std::env::consts::ARCH, std::env::consts::OS);
    let pinned = cache_root.join("sdk").join("sdl3").join(target);
    from_dir(&pinned, "pinned prebuilt".into()).or_else(from_pkg_config)
}

fn moltenvk_in(dir: &Path, origin: &str) -> Option<LinkedVulkan> {
    let lib_dir = dir.join("lib");
    let d = lib_dir.display();
    lib_dir
        .join("libMoltenVK.dylib")
        .is_file()
        .then(|| LinkedVulkan {
            libs: vec![
                format!("-L{d}"),
                format!("-Wl,-rpath,{d}"),
                "-lMoltenVK".into(),
            ],
            origin: origin.into(),
        })
}

/// MoltenVK for an Apple host, or `None` on other hosts and when absent.
pub fn find_moltenvk(cache_root: &Path) -> Option<LinkedVulkan> {
    if !cfg!(target_vendor = "apple") {
        return None;
    }
    if let Some(d) = std::env::var_os("RELIFT_MOLTENVK_DIR").filter(|v| !v.is_empty()) {
        return moltenvk_in(Path::new(&d), "RELIFT_MOLTENVK_DIR");
    }
    let target = format!("{}-{}", std::env::consts::ARCH, std::env::consts::OS);
    moltenvk_in(
        &cache_root.join("sdk").join("moltenvk").join(target),
        "pinned prebuilt",
    )
    .or_else(|| moltenvk_in(Path::new("/opt/homebrew/opt/molten-vk"), "Homebrew"))
    .or_else(|| moltenvk_in(Path::new("/usr/local/opt/molten-vk"), "Homebrew"))
}

/// The sources of the Vulkan forwarding layer and of the NVN2 layer over it,
/// with the vendored Zstandard decoder the NVN2 layer reads the build's
/// compressed shader table through.
fn vk_sources(rt: &Path) -> std::io::Result<Vec<PathBuf>> {
    let switch = rt.join("guests").join("switch");
    let mut v = c_files(&switch.join("vk"))?;
    v.extend(c_files(&switch.join("vk").join("gen"))?);
    v.extend(c_files(&switch.join("nvn"))?);
    v.push(rt.join(ZSTD_DIR).join("zstddeclib.c"));
    Ok(v)
}

/// The vendored Zstandard decoder (`runtime/third_party/zstd/NOTICE`).
const ZSTD_DIR: &str = "third_party/zstd";

/// Keeps the decoder's symbols out of the runtime's exported ones.
const ZSTD_DEFS: [&str; 2] = ["-DZSTDLIB_VISIBLE=", "-DZSTDLIB_HIDDEN="];

/// The extra sources of the runtime at `rt`, with SDL3 and MoltenVK looked
/// up under `cache_root`.
///
/// # Errors
///
/// A source directory of the runtime cannot be read.
pub fn extras(rt: &Path, cache_root: &Path) -> std::io::Result<Extras> {
    let vk = vk_sources(rt)?;
    let window = rt.join("host").join("window_sdl3.c");
    let sdl3 = find_sdl3(cache_root).filter(|_| window.is_file());
    let audio = rt.join("host").join("audio_sdl3.c");
    let opus_src = rt.join("host").join("opus_libopus.c");
    let opus = find_opus().filter(|_| opus_src.is_file());
    let vpx = find_vpx(cache_root).filter(|_| has_vpx_glue(rt));
    let dav1d = find_dav1d(cache_root).filter(|_| has_dav1d_glue(rt));
    Ok(Extras {
        vk,
        window: sdl3.as_ref().map(|_| window),
        host: sdl3
            .as_ref()
            .filter(|_| audio.is_file())
            .map(|_| audio)
            .into_iter()
            .collect(),
        opus_src: opus.as_ref().map(|_| opus_src),
        opus,
        sdl3,
        vulkan: find_moltenvk(cache_root),
        adrenotools: None,
        vpx,
        dav1d_asm: dav1d.as_ref().and_then(|_| native_dav1d_asm()),
        video: has_video(rt),
        xlate: None,
        dav1d,
    })
}

fn has_dav1d_glue(rt: &Path) -> bool {
    rt.join("host").join("av1_dav1d.c").is_file()
}

fn has_video(rt: &Path) -> bool {
    rt.join("guests").join("switch").is_dir()
}

fn has_vpx_glue(rt: &Path) -> bool {
    rt.join("host").join("vp9_libvpx.c").is_file()
}

/// The extras of an Android program: the window, sound and Android glue
/// backends against the SDL3 and libopus builds in `deps`.
pub fn android_extras(
    rt: &Path,
    deps: &crate::android::Deps,
    target: crate::android::Target,
    with_vk: bool,
) -> std::io::Result<Extras> {
    let host = rt.join("host");
    let mut vk = Vec::new();
    if with_vk {
        vk = vk_sources(rt)?;
    }
    Ok(Extras {
        vk,
        window: Some(host.join("window_sdl3.c")),
        sdl3: Some(Sdl3 {
            cflags: vec![
                format!("-I{}", deps.sdl3_include.display()),
                format!("-I{}", deps.opus_include.display()),
            ],
            libs: vec![
                deps.sdl3_lib.display().to_string(),
                deps.opus_lib.display().to_string(),
                "-llog".into(),
                "-landroid".into(),
            ],
            origin: "Android prebuilt".into(),
        }),
        vulkan: None,
        host: [
            "audio_sdl3.c",
            "opus_libopus.c",
            "android_sdl3.c",
            "android_display.c",
            "data_sdl3.c",
        ]
        .iter()
        .map(|n| host.join(n))
        .collect(),
        opus_src: None,
        opus: None,
        adrenotools: deps.adrenotools.clone(),
        vpx: deps.vpx.clone().filter(|_| has_vpx_glue(rt)),
        dav1d: deps.dav1d.clone().filter(|_| has_dav1d_glue(rt)),
        dav1d_asm: (target.abi == "arm64-v8a").then(aarch64_dav1d_asm),
        video: has_video(rt),
        xlate: None,
    })
}

/// The extras of an iOS program: the window, sound and iOS glue backends
/// against the static SDL3 and libopus in `deps`, and with `with_vk` the
/// Vulkan forwarding against the static MoltenVK.
///
/// # Errors
///
/// The runtime's Vulkan sources cannot be listed, or `with_vk` without a
/// MoltenVK in `deps`.
pub fn ios_extras(rt: &Path, deps: &crate::ios::Deps, with_vk: bool) -> std::io::Result<Extras> {
    let host = rt.join("host");
    let mut vk = Vec::new();
    let mut vulkan = None;
    if with_vk {
        let lib = deps.moltenvk.as_ref().ok_or_else(|| {
            std::io::Error::new(std::io::ErrorKind::NotFound, deps.moltenvk_missing.clone())
        })?;
        vk = vk_sources(rt)?;
        vulkan = Some(LinkedVulkan {
            libs: vec![lib.display().to_string()],
            origin: "iOS static".into(),
        });
    }
    Ok(Extras {
        vk,
        window: Some(host.join("window_sdl3.c")),
        sdl3: Some(Sdl3 {
            cflags: vec![
                format!("-I{}", deps.sdl3_include.display()),
                format!("-I{}", deps.opus_include.display()),
            ],
            libs: vec![
                deps.sdl3_lib.display().to_string(),
                deps.opus_lib.display().to_string(),
            ],
            origin: "iOS static".into(),
        }),
        vulkan,
        host: [
            "audio_sdl3.c",
            "opus_libopus.c",
            "ios_sdl3.c",
            "data_sdl3.c",
        ]
        .iter()
        .map(|n| host.join(n))
        .collect(),
        opus_src: None,
        opus: None,
        adrenotools: None,
        vpx: deps.vpx.clone().filter(|_| has_vpx_glue(rt)),
        dav1d: deps.dav1d.clone().filter(|_| has_dav1d_glue(rt)),
        dav1d_asm: None,
        video: has_video(rt),
        xlate: None,
    })
}

impl Extras {
    /// The assembler of the plan these extras go into, when dav1d is built with assembly.
    pub fn assembler(&self) -> Option<Vec<String>> {
        self.dav1d_asm
            .as_ref()
            .filter(|a| !a.nasm.is_empty())
            .map(|a| a.nasm.clone())
    }

    /// What the build prints about the window, audio and Opus backends.
    pub fn note(&self) -> String {
        let window = match &self.sdl3 {
            Some(s) => format!("window and audio: SDL3 ({})", s.origin),
            None => {
                "window: SDL3 not found, building headless and silent (set RELIFT_SDL3_DIR)".into()
            }
        };
        let window = match &self.vulkan {
            Some(v) => format!("{window}; Vulkan: MoltenVK linked ({})", v.origin),
            None => window,
        };
        let audio = match &self.opus {
            Some(o) => format!("{window}; opus: libopus ({})", o.origin),
            None if self.has_host("opus_libopus.c") => window,
            None => format!(
                "{window}; opus: libopus not found, Opus streams decode to silence (set RELIFT_OPUS_DIR)"
            ),
        };
        if !self.video {
            return audio;
        }
        let video = match &self.vpx {
            Some(v) => format!("{audio}; vp9: libvpx ({})", v.origin),
            None => format!(
                "{audio}; vp9: libvpx {}, VP9 video stays undecoded",
                missing(&LIBVPX_PIN)
            ),
        };
        match &self.dav1d {
            Some(v) => format!("{video}; av1: dav1d ({})", v.origin),
            None => format!(
                "{video}; av1: dav1d {}, AV1 video stays undecoded",
                missing(&DAV1D_PIN)
            ),
        }
    }

    fn has_host(&self, name: &str) -> bool {
        self.host
            .iter()
            .any(|h| h.file_name().is_some_and(|n| n == name))
    }

    /// Links the shader translator library `x`: the NVN2 layer makes its
    /// shader tables on the device with it, and reads them with the
    /// library's Zstandard instead of the vendored decoder.
    pub fn with_xlate(&mut self, x: Xlate) {
        self.vk
            .retain(|s| !s.ends_with(Path::new(ZSTD_DIR).join("zstddeclib.c")));
        self.xlate = Some(x);
    }

    /// Defines and include paths every runtime file needs.
    pub fn defines(&self) -> Vec<String> {
        let mut d = Vec::new();
        if let Some(x) = &self.xlate {
            d.push("-DRL_NVN_XLATE".into());
            d.push(format!("-I{}", x.include.display()));
        }
        if self.sdl3.is_some() {
            d.push("-DRL_HOST_VIDEO".into());
        }
        if self.vulkan.is_some() {
            d.push("-DRL_VULKAN_LINKED".into());
        }
        if self.has_host("audio_sdl3.c") {
            d.push("-DRL_HOST_AUDIO".into());
        }
        if self.opus.is_some() || self.has_host("opus_libopus.c") {
            d.push("-DRL_HOST_OPUS".into());
        }
        if self.vpx.is_some() {
            d.push("-DRL_HOST_VP9".into());
        }
        if self.dav1d.is_some() {
            d.push("-DRL_HOST_AV1".into());
        }
        if let Some(a) = &self.adrenotools {
            d.push("-DRL_ADRENOTOOLS".into());
            d.push(format!("-I{}", a.include.display()));
        }
        d
    }

    /// Link flags.
    pub fn libs(&self) -> Vec<String> {
        let mut l = self.sdl3.as_ref().map_or_else(Vec::new, |s| s.libs.clone());
        if let Some(v) = &self.vulkan {
            l.extend(v.libs.iter().cloned());
        }
        if let Some(a) = &self.adrenotools {
            l.push(a.lib.display().to_string());
        }
        if let Some(o) = &self.opus {
            l.extend(o.libs.iter().cloned());
        }
        if let Some(x) = &self.xlate {
            l.push(x.lib.display().to_string());
            l.extend(x.libs.iter().cloned());
        }
        let android = self.has_host("android_sdl3.c");
        if !l.is_empty() && !android && cfg!(target_vendor = "apple") {
            l.push("-Wl,-headerpad_max_install_names".into());
        }
        l
    }

    /// Compiles for the extra sources. `rt_flags` are the runtime's flags
    /// and `obj` names an object.
    pub fn compiles(
        &self,
        rt: &Path,
        rt_flags: &[String],
        obj: &dyn Fn(&Path) -> PathBuf,
    ) -> Vec<Compile> {
        let vk_inc: Vec<String> = [
            "guests/switch/vk",
            "guests/switch/vk/gen",
            "guests/switch/nvn",
            ZSTD_DIR,
        ]
        .map(|p| format!("-I{}", rt.join(p).display()))
        .into_iter()
        .chain(ZSTD_DEFS.map(String::from))
        .collect();
        let mut out: Vec<Compile> = self
            .vk
            .iter()
            .map(|s| Compile {
                src: s.clone(),
                obj: obj(s),
                flags: rt_flags.iter().cloned().chain(vk_inc.clone()).collect(),
                pool: None,
                stage: 0,
            })
            .collect();
        if let Some(sdl3) = &self.sdl3 {
            for w in self.window.iter().chain(&self.host) {
                out.push(Compile {
                    src: w.clone(),
                    obj: obj(w),
                    flags: rt_flags
                        .iter()
                        .cloned()
                        .chain(sdl3.cflags.iter().cloned())
                        .collect(),
                    pool: None,
                    stage: 0,
                });
            }
        }
        if let (Some(src), Some(opus)) = (&self.opus_src, &self.opus) {
            out.push(Compile {
                src: src.clone(),
                obj: obj(src),
                flags: rt_flags
                    .iter()
                    .cloned()
                    .chain(opus.cflags.iter().cloned())
                    .collect(),
                pool: None,
                stage: 0,
            });
        }
        if let Some(v) = &self.vpx {
            out.extend(vpx_compiles(rt, v, rt_flags, obj));
        }
        if let Some(v) = &self.dav1d {
            out.extend(dav1d_compiles(
                rt,
                v,
                self.dav1d_asm.as_ref(),
                rt_flags,
                obj,
            ));
        }
        out
    }
}

/// The dav1d sources (the bit depth files twice), with the generic
/// configuration and without the runtime's include paths and defines, and
/// the glue.
fn dav1d_compiles(
    rt: &Path,
    v: &Dav1d,
    asm: Option<&Dav1dAsm>,
    rt_flags: &[String],
    obj: &dyn Fn(&Path) -> PathBuf,
) -> Vec<Compile> {
    let config = rt.join("host").join("dav1d");
    let arch_config = asm.map(|a| config.join(a.config));
    let own: Vec<String> = rt_flags
        .iter()
        .filter(|f| !f.starts_with("-I") && !f.starts_with("-D"))
        .cloned()
        .collect();
    let flags = |extra: Option<u32>| -> Vec<String> {
        arch_config
            .iter()
            .map(|c| format!("-I{}", c.display()))
            .chain([
                format!("-I{}", config.display()),
                format!("-I{}", v.src.display()),
                format!("-I{}", v.src.join("include").display()),
                format!("-I{}", v.src.join("include").join("dav1d").display()),
                "-D_GNU_SOURCE".into(),
            ])
            .chain(extra.map(|b| format!("-DBITDEPTH={b}")))
            .chain(own.iter().cloned())
            .collect()
    };
    let arm = asm.is_some_and(|a| a.config == "aarch64");
    let arch_sources: &[&str] = match asm {
        Some(_) if arm => &["src/arm/cpu.c"],
        Some(_) => &["src/x86/cpu.c"],
        None => &[],
    };
    let mut out: Vec<Compile> = DAV1D_SOURCES
        .iter()
        .chain(arch_sources)
        .map(|s| Compile {
            src: v.src.join(s),
            obj: obj(&rt.join("host").join("dav1d").join(s)),
            flags: flags(None),
            pool: None,
            stage: 0,
        })
        .collect();
    for s in DAV1D_BITDEPTH_SOURCES {
        for bits in [8u32, 16] {
            out.push(Compile {
                src: v.src.join(s),
                obj: obj(&rt
                    .join("host")
                    .join("dav1d")
                    .join(s.replace("_tmpl.c", &format!("_{bits}bpc.c")))),
                flags: flags(Some(bits)),
                pool: None,
                stage: 0,
            });
        }
    }
    if arm {
        for name in DAV1D_ARM64_ASM {
            let s = format!("src/arm/64/{name}.S");
            out.push(Compile {
                src: v.src.join(&s),
                obj: obj(&rt.join("host").join("dav1d").join(&s)),
                flags: flags(None),
                pool: None,
                stage: 0,
            });
        }
    } else if let (Some(a), Some(arch)) = (asm, &arch_config) {
        let nasm_flags = [
            "-f".to_string(),
            a.format.to_string(),
            format!("-I{}/", v.src.join("src").display()),
            format!("-I{}/", arch.display()),
        ];
        let listed = DAV1D_X86_ASM
            .iter()
            .chain(&DAV1D_X86_ASM_8)
            .chain(&DAV1D_X86_ASM_16);
        for name in listed {
            let src = v.src.join("src").join("x86").join(format!("{name}.asm"));
            out.push(Compile {
                obj: obj(&rt
                    .join("host")
                    .join("dav1d")
                    .join("src/x86")
                    .join(format!("{name}.o"))),
                src,
                flags: nasm_flags.to_vec(),
                pool: None,
                stage: 0,
            });
        }
    }
    let glue = rt.join("host").join("av1_dav1d.c");
    out.push(Compile {
        obj: obj(&glue),
        src: glue,
        flags: rt_flags
            .iter()
            .cloned()
            .chain([
                "-isystem".into(),
                config.display().to_string(),
                "-isystem".into(),
                v.src.join("include").display().to_string(),
                "-isystem".into(),
                v.src.join("include").join("dav1d").display().to_string(),
            ])
            .collect(),
        pool: None,
        stage: 0,
    });
    out
}

/// The libvpx sources, with the generic configuration and without the
/// runtime's include paths and defines, and the glue that includes
/// libvpx's frame decoder.
fn vpx_compiles(
    rt: &Path,
    v: &Vpx,
    rt_flags: &[String],
    obj: &dyn Fn(&Path) -> PathBuf,
) -> Vec<Compile> {
    let config = rt.join("host").join("vpx");
    let own: Vec<String> = rt_flags
        .iter()
        .filter(|f| !f.starts_with("-I") && !f.starts_with("-D"))
        .cloned()
        .collect();
    let mut out: Vec<Compile> = LIBVPX_SOURCES
        .iter()
        .map(|s| Compile {
            src: v.src.join(s),
            obj: obj(&rt.join("host").join("libvpx").join(s)),
            flags: [
                format!("-I{}", config.display()),
                format!("-I{}", v.src.display()),
                "-D_GNU_SOURCE".into(),
            ]
            .into_iter()
            .chain(own.iter().cloned())
            .collect(),
            pool: None,
            stage: 0,
        })
        .collect();
    let glue = rt.join("host").join("vp9_libvpx.c");
    out.push(Compile {
        obj: obj(&glue),
        src: glue,
        flags: rt_flags
            .iter()
            .cloned()
            .chain([
                "-isystem".into(),
                config.display().to_string(),
                "-isystem".into(),
                v.src.display().to_string(),
            ])
            .collect(),
        pool: None,
        stage: 0,
    });
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../target/tmp")
            .join(format!("{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        d
    }

    fn pinned_tarball(root: &Path) -> (Pinned, PathBuf) {
        use sha2::{Digest, Sha256};
        let src = root.join("src").join("lib-1.0");
        std::fs::create_dir_all(src.join("sub")).unwrap();
        relift_cache::write_if_changed(&src.join("sub").join("a.c"), b"int a;\n").unwrap();
        let tarball = root.join("lib-1.0.tar.gz");
        let ok = relift_proc::command("tar")
            .arg("-czf")
            .arg(&tarball)
            .arg("-C")
            .arg(root.join("src"))
            .arg("lib-1.0")
            .status()
            .unwrap()
            .success();
        assert!(ok, "tar makes the test tarball");
        let sha: String = Sha256::digest(std::fs::read(&tarball).unwrap())
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
        let url = format!("file://{}", tarball.display());
        let p = Pinned {
            name: "lib",
            env: "RELIFT_TEST_LIB_DIR",
            version: "1.0",
            url: Box::leak(url.into_boxed_str()),
            sha256: Box::leak(sha.into_boxed_str()),
            top: "lib-1.0",
        };
        (p, tarball)
    }

    #[test]
    fn a_pinned_source_is_fetched_checked_and_unpacked_into_the_cache() {
        let root = scratch("pinned-fetch");
        let (p, _) = pinned_tarball(&root);
        let cache = root.join("cache");
        let a = pinned_dir(&cache, &p).join("sub").join("a.c");
        ensure_pinned(&cache, &p, |r| pinned_dir(r, &p).join("sub/a.c").is_file()).unwrap();
        assert_eq!(std::fs::read_to_string(&a).unwrap(), "int a;\n");
        let left: Vec<_> = std::fs::read_dir(pinned_dir(&cache, &p).parent().unwrap())
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        assert_eq!(
            left,
            vec![std::ffi::OsString::from("1.0")],
            "no part is left"
        );
    }

    #[test]
    fn two_fetches_of_one_pinned_source_at_once_both_succeed() {
        let root = scratch("pinned-race");
        let (p, _) = pinned_tarball(&root);
        let cache = root.join("cache");
        for round in 0..8 {
            let cache = cache.join(round.to_string());
            let gate = std::sync::Barrier::new(4);
            std::thread::scope(|s| {
                let hs: Vec<_> = (0..4)
                    .map(|_| {
                        s.spawn(|| {
                            gate.wait();
                            ensure_pinned(&cache, &p, |r| {
                                pinned_dir(r, &p).join("sub/a.c").is_file()
                            })
                            .map_err(|e| e.to_string())
                        })
                    })
                    .collect();
                for h in hs {
                    h.join().unwrap().unwrap();
                }
            });
            let a = pinned_dir(&cache, &p).join("sub").join("a.c");
            assert_eq!(std::fs::read_to_string(a).unwrap(), "int a;\n");
            let left = std::fs::read_dir(pinned_dir(&cache, &p).parent().unwrap())
                .unwrap()
                .count();
            assert_eq!(left, 1, "no part is left");
        }
    }

    #[test]
    fn a_half_written_pinned_folder_is_replaced() {
        let root = scratch("pinned-half");
        let (p, _) = pinned_tarball(&root);
        let cache = root.join("cache");
        let half = pinned_dir(&cache, &p).join("other");
        std::fs::create_dir_all(&half).unwrap();
        relift_cache::write_if_changed(&half.join("x.c"), b"int x;\n").unwrap();
        ensure_pinned(&cache, &p, |r| pinned_dir(r, &p).join("sub/a.c").is_file()).unwrap();
        assert!(pinned_dir(&cache, &p).join("sub/a.c").is_file());
        assert!(!pinned_dir(&cache, &p).join("other").exists());
    }

    #[test]
    fn a_pinned_source_with_another_hash_is_refused_and_not_kept() {
        let root = scratch("pinned-hash");
        let (mut p, _) = pinned_tarball(&root);
        p.sha256 = "00";
        let cache = root.join("cache");
        let err = ensure_pinned(&cache, &p, |r| pinned_dir(r, &p).is_dir())
            .unwrap_err()
            .to_string();
        assert!(err.contains("does not have the pinned SHA-256 00"), "{err}");
        assert!(err.contains("RELIFT_TEST_LIB_DIR=off"), "{err}");
        assert!(!pinned_dir(&cache, &p).exists());
    }

    #[test]
    fn missing_sdl3_dir_is_not_found() {
        assert_eq!(from_dir(Path::new("/nonexistent"), "x".into()), None);
    }

    #[test]
    fn sdl3_dir_gives_include_and_lib_flags() {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("target")
            .join(format!("relift-sdl3-{}", std::process::id()));
        std::fs::create_dir_all(d.join("include").join("SDL3")).unwrap();
        relift_cache::write_if_changed(&d.join("include").join("SDL3").join("SDL.h"), b"").unwrap();
        let s = from_dir(&d, "t".into()).unwrap();
        std::fs::remove_dir_all(&d).unwrap();
        assert!(s.cflags[0].ends_with("/include"));
        assert!(s.libs.iter().any(|l| l == "-lSDL3"));
    }

    #[test]
    fn pkg_config_opus_include_gains_its_parent() {
        let flags = vec!["-I/p/include/opus".to_string(), "-I/q".to_string()];
        assert_eq!(
            opus_cflags(flags),
            ["-I/p/include/opus", "-I/q", "-I/p/include"]
        );
    }

    #[test]
    fn missing_opus_dir_is_not_found() {
        assert_eq!(opus_from_dir(Path::new("/nonexistent"), "x".into()), None);
    }

    #[test]
    fn the_translator_library_replaces_the_vendored_decoder() {
        let rt = Path::new("/rt");
        let mut e = Extras {
            vk: vec![
                rt.join("guests/switch/nvn/nvn_shader.c"),
                rt.join(ZSTD_DIR).join("zstddeclib.c"),
            ],
            window: None,
            sdl3: None,
            vulkan: None,
            host: Vec::new(),
            opus_src: None,
            opus: None,
            adrenotools: None,
            vpx: None,
            dav1d: None,
            dav1d_asm: None,
            video: true,
            xlate: None,
        };
        e.with_xlate(Xlate {
            lib: PathBuf::from("/c/librelift_gpu_xlate.a"),
            include: PathBuf::from("/g/include"),
            libs: vec!["-ldl".into()],
        });
        assert_eq!(e.vk, [rt.join("guests/switch/nvn/nvn_shader.c")]);
        assert_eq!(e.defines(), ["-DRL_NVN_XLATE", "-I/g/include"]);
        let mut want = vec!["/c/librelift_gpu_xlate.a", "-ldl"];
        if cfg!(target_vendor = "apple") {
            want.push("-Wl,-headerpad_max_install_names");
        }
        assert_eq!(e.libs(), want);
    }

    #[test]
    fn headless_note_says_so_and_adds_nothing() {
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
            vulkan: None,
            host: Vec::new(),
            opus_src: None,
            opus: None,
            adrenotools: None,
            vpx: None,
            dav1d: None,
            dav1d_asm: None,
            video: true,
            xlate: None,
        };
        assert!(e.note().contains("headless"));
        assert!(e.defines().is_empty());
        assert!(e.libs().is_empty());
    }

    #[test]
    fn adrenotools_defines_includes_and_links_it() {
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
            vulkan: None,
            host: vec![PathBuf::from("android_sdl3.c")],
            opus_src: None,
            opus: None,
            adrenotools: Some(crate::android::Adrenotools {
                include: PathBuf::from("/a/include"),
                lib: PathBuf::from("/a/lib/libadrenotools.so"),
                hooks: Vec::new(),
                licenses: Vec::new(),
            }),
            vpx: None,
            dav1d: None,
            dav1d_asm: None,
            video: true,
            xlate: None,
        };
        assert_eq!(e.defines(), ["-DRL_ADRENOTOOLS", "-I/a/include"]);
        assert_eq!(e.libs(), ["/a/lib/libadrenotools.so"]);
    }

    #[test]
    fn libvpx_compiles_its_sources_and_the_glue() {
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
            vulkan: None,
            host: Vec::new(),
            opus_src: None,
            opus: None,
            adrenotools: None,
            vpx: Some(Vpx {
                src: PathBuf::from("/v"),
                origin: "t".into(),
            }),
            dav1d: None,
            dav1d_asm: None,
            video: true,
            xlate: None,
        };
        assert!(e.note().contains("vp9: libvpx (t)"));
        assert_eq!(e.defines(), ["-DRL_HOST_VP9"]);
        let rt = Path::new("/rt");
        let flags = ["-O2".to_string(), "-Ix".into(), "-DY".into()];
        let c = e.compiles(rt, &flags, &|s| s.with_extension("o"));
        assert_eq!(c.len(), LIBVPX_SOURCES.len() + 1);
        assert_eq!(c[0].src, Path::new("/v/vp9/common/vp9_alloccommon.c"));
        assert_eq!(
            c[0].obj,
            Path::new("/rt/host/libvpx/vp9/common/vp9_alloccommon.o")
        );
        assert!(c[0].flags.contains(&"-O2".to_string()));
        assert!(!c[0].flags.contains(&"-Ix".to_string()));
        let glue = c.last().unwrap();
        assert_eq!(glue.src, Path::new("/rt/host/vp9_libvpx.c"));
        assert!(glue.flags.contains(&"-DY".to_string()));
    }

    #[test]
    fn dav1d_compiles_both_bit_depths_and_the_glue() {
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
            vulkan: None,
            host: Vec::new(),
            opus_src: None,
            opus: None,
            adrenotools: None,
            vpx: None,
            dav1d: Some(Dav1d {
                src: PathBuf::from("/d"),
                origin: "t".into(),
            }),
            dav1d_asm: None,
            video: true,
            xlate: None,
        };
        assert!(e.note().contains("av1: dav1d (t)"));
        assert_eq!(e.assembler(), None);
        assert_eq!(e.defines(), ["-DRL_HOST_AV1"]);
        let rt = Path::new("/rt");
        let flags = ["-O2".to_string(), "-Ix".into(), "-DY".into()];
        let c = e.compiles(rt, &flags, &|s| s.with_extension("o"));
        assert_eq!(
            c.len(),
            DAV1D_SOURCES.len() + 2 * DAV1D_BITDEPTH_SOURCES.len() + 1
        );
        assert_eq!(c[0].src, Path::new("/d/src/cdf.c"));
        let cdef: Vec<_> = c
            .iter()
            .filter(|x| x.src == Path::new("/d/src/cdef_tmpl.c"))
            .collect();
        assert_eq!(cdef.len(), 2);
        assert_eq!(cdef[0].obj, Path::new("/rt/host/dav1d/src/cdef_8bpc.o"));
        assert!(cdef[0].flags.contains(&"-DBITDEPTH=8".to_string()));
        assert!(cdef[1].flags.contains(&"-DBITDEPTH=16".to_string()));
        assert!(!cdef[0].flags.contains(&"-Ix".to_string()));
        let glue = c.last().unwrap();
        assert_eq!(glue.src, Path::new("/rt/host/av1_dav1d.c"));
        assert!(glue.flags.contains(&"-DY".to_string()));
    }

    #[test]
    fn dav1d_with_assembly_adds_the_x86_files_and_the_assembler() {
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
            vulkan: None,
            host: Vec::new(),
            opus_src: None,
            opus: None,
            adrenotools: None,
            vpx: None,
            dav1d: Some(Dav1d {
                src: PathBuf::from("/d"),
                origin: "t".into(),
            }),
            dav1d_asm: Some(Dav1dAsm {
                nasm: vec!["/bin/nasm".into()],
                format: "elf64",
                config: "x86_64",
            }),
            video: true,
            xlate: None,
        };
        assert_eq!(e.assembler(), Some(vec!["/bin/nasm".to_string()]));
        let rt = Path::new("/rt");
        let c = e.compiles(rt, &["-O2".to_string()], &|s| s.with_extension("o"));
        let asm: Vec<_> = c
            .iter()
            .filter(|x| x.src.extension().is_some_and(|e| e == "asm"))
            .collect();
        assert_eq!(
            asm.len(),
            DAV1D_X86_ASM.len() + DAV1D_X86_ASM_8.len() + DAV1D_X86_ASM_16.len()
        );
        assert_eq!(asm[0].src, Path::new("/d/src/x86/cpuid.asm"));
        assert_eq!(asm[0].obj, Path::new("/rt/host/dav1d/src/x86/cpuid.o"));
        assert_eq!(
            asm[0].flags,
            ["-f", "elf64", "-I/d/src/", "-I/rt/host/dav1d/x86_64/"]
        );
        assert!(c.iter().any(|x| x.src == Path::new("/d/src/x86/cpu.c")));
        let cdef = c
            .iter()
            .find(|x| x.src == Path::new("/d/src/cdef_tmpl.c"))
            .unwrap();
        assert_eq!(cdef.flags[0], "-I/rt/host/dav1d/x86_64");
    }

    #[test]
    fn dav1d_on_arm64_assembles_its_arm_files_with_the_c_compiler() {
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
            vulkan: None,
            host: Vec::new(),
            opus_src: None,
            opus: None,
            adrenotools: None,
            vpx: None,
            dav1d: Some(Dav1d {
                src: PathBuf::from("/d"),
                origin: "t".into(),
            }),
            dav1d_asm: Some(aarch64_dav1d_asm()),
            video: true,
            xlate: None,
        };
        assert_eq!(e.assembler(), None, "no NASM for arm64");
        let rt = Path::new("/rt");
        let c = e.compiles(rt, &["-O2".to_string()], &|s| s.with_extension("o"));
        let asm: Vec<_> = c
            .iter()
            .filter(|x| x.src.extension().is_some_and(|e| e == "S"))
            .collect();
        assert_eq!(asm.len(), DAV1D_ARM64_ASM.len());
        assert_eq!(asm[0].src, Path::new("/d/src/arm/64/itx.S"));
        assert_eq!(asm[0].obj, Path::new("/rt/host/dav1d/src/arm/64/itx.o"));
        assert_eq!(asm[0].flags[0], "-I/rt/host/dav1d/aarch64");
        assert!(asm[0].flags.contains(&"-O2".to_string()));
        assert!(
            asm.iter()
                .all(|x| !x.flags.iter().any(|f| f.starts_with("-DBITDEPTH")))
        );
        assert!(c.iter().any(|x| x.src == Path::new("/d/src/arm/cpu.c")));
        assert!(!c.iter().any(|x| x.src == Path::new("/d/src/x86/cpu.c")));
        assert!(
            !c.iter()
                .any(|x| x.src.extension().is_some_and(|e| e == "asm"))
        );
    }

    #[test]
    fn android_arm64_builds_dav1d_with_assembly_and_x64_without() {
        let rt = Path::new("/nonexistent-rt");
        let deps = crate::android::Deps {
            sdl3_include: PathBuf::new(),
            sdl3_lib: PathBuf::new(),
            sdl3_jar: PathBuf::new(),
            opus_include: PathBuf::new(),
            opus_lib: PathBuf::new(),
            adrenotools: None,
            vpx: None,
            dav1d: None,
        };
        let arm = android_extras(
            rt,
            &deps,
            crate::android::target("android-arm64").unwrap(),
            false,
        )
        .unwrap();
        assert_eq!(arm.dav1d_asm, Some(aarch64_dav1d_asm()));
        let x64 = android_extras(
            rt,
            &deps,
            crate::android::target("android-x64").unwrap(),
            false,
        )
        .unwrap();
        assert_eq!(x64.dav1d_asm, None);
    }

    #[test]
    fn missing_dav1d_dir_is_not_found() {
        assert_eq!(dav1d_in(Path::new("/nonexistent"), "x".into()), None);
    }

    #[test]
    fn missing_libvpx_dir_is_not_found() {
        assert_eq!(vpx_in(Path::new("/nonexistent"), "x".into()), None);
    }

    #[test]
    fn missing_moltenvk_dir_is_not_found() {
        assert_eq!(moltenvk_in(Path::new("/nonexistent"), "x"), None);
    }

    #[test]
    fn linked_moltenvk_defines_and_links_it() {
        let m = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("target")
            .join(format!("relift-mvk-{}", std::process::id()));
        std::fs::create_dir_all(m.join("lib")).unwrap();
        relift_cache::write_if_changed(&m.join("lib").join("libMoltenVK.dylib"), b"").unwrap();
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
            vulkan: moltenvk_in(&m, "t"),
            host: Vec::new(),
            opus_src: None,
            opus: None,
            adrenotools: None,
            vpx: None,
            dav1d: None,
            dav1d_asm: None,
            video: true,
            xlate: None,
        };
        std::fs::remove_dir_all(&m).unwrap();
        assert!(e.note().contains("MoltenVK linked (t)"));
        assert_eq!(e.defines(), ["-DRL_VULKAN_LINKED"]);
        let libs = e.libs();
        let l = m.join("lib").display().to_string();
        assert_eq!(
            libs[..3],
            [
                format!("-L{l}"),
                format!("-Wl,-rpath,{l}"),
                "-lMoltenVK".into()
            ]
        );
        assert_eq!(
            libs.get(3).map(String::as_str),
            cfg!(target_vendor = "apple").then_some("-Wl,-headerpad_max_install_names")
        );
    }
}
