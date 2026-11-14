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

use std::path::{Path, PathBuf};

use relift_build::Compile;

/// Where SDL3 was found and how to compile and link against it.
#[derive(Debug, PartialEq, Eq)]
pub struct Sdl3 {
    pub cflags: Vec<String>,
    pub libs: Vec<String>,
    pub origin: String,
}

/// The extra runtime sources, flags and libraries of one build.
#[derive(Debug, PartialEq, Eq)]
pub struct Extras {
    pub vk: Vec<PathBuf>,
    pub window: Option<PathBuf>,
    pub sdl3: Option<Sdl3>,
}

fn c_files(dir: &Path) -> std::io::Result<Vec<PathBuf>> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)?
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

fn from_pkg_config() -> Option<Sdl3> {
    let query = |flag: &str| {
        let out = relift_proc::command("pkg-config")
            .args([flag, "sdl3"])
            .output()
            .ok()
            .filter(|o| o.status.success())?;
        let text = String::from_utf8(out.stdout).ok()?;
        Some(
            text.split_whitespace()
                .map(String::from)
                .collect::<Vec<_>>(),
        )
    };
    Some(Sdl3 {
        cflags: query("--cflags")?,
        libs: query("--libs")?,
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

/// The extra sources of the runtime at `rt`, with SDL3 looked up under
/// `cache_root`.
///
/// # Errors
///
/// The runtime's `guests/switch/vk` cannot be read.
pub fn extras(rt: &Path, cache_root: &Path) -> std::io::Result<Extras> {
    let vk_dir = rt.join("guests").join("switch").join("vk");
    let mut vk = c_files(&vk_dir)?;
    vk.extend(c_files(&vk_dir.join("gen"))?);
    let window = rt.join("host").join("window_sdl3.c");
    let sdl3 = find_sdl3(cache_root).filter(|_| window.is_file());
    Ok(Extras {
        vk,
        window: sdl3.as_ref().map(|_| window),
        sdl3,
    })
}

impl Extras {
    /// What the build prints about the window backend.
    pub fn note(&self) -> String {
        match &self.sdl3 {
            Some(s) => format!("window: SDL3 ({})", s.origin),
            None => "window: SDL3 not found, building headless (set RELIFT_SDL3_DIR)".into(),
        }
    }

    /// Defines every runtime file needs.
    pub fn defines(&self) -> Vec<String> {
        if self.sdl3.is_some() {
            vec!["-DRL_HOST_VIDEO".into()]
        } else {
            Vec::new()
        }
    }

    /// Link flags.
    pub fn libs(&self) -> Vec<String> {
        self.sdl3.as_ref().map_or_else(Vec::new, |s| s.libs.clone())
    }

    /// Compiles for the extra sources. `rt_flags` are the runtime's flags
    /// and `obj` names an object.
    pub fn compiles(
        &self,
        rt: &Path,
        rt_flags: &[String],
        obj: &dyn Fn(&Path) -> PathBuf,
    ) -> Vec<Compile> {
        let vk_inc = ["guests/switch/vk", "guests/switch/vk/gen"]
            .map(|p| format!("-I{}", rt.join(p).display()));
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
        if let (Some(w), Some(sdl3)) = (&self.window, &self.sdl3) {
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
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn headless_note_says_so_and_adds_nothing() {
        let e = Extras {
            vk: Vec::new(),
            window: None,
            sdl3: None,
        };
        assert!(e.note().contains("headless"));
        assert!(e.defines().is_empty());
        assert!(e.libs().is_empty());
    }
}
