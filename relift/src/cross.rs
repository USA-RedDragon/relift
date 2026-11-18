//! The host targets a build can name besides this host: Android and iOS.

use std::path::Path;

use crate::import::BoxError;
use crate::runtime_link::Extras;
use crate::settings::Settings;
use crate::{android, ios};
use relift_guest::Entry;

/// A target other than this host.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Target {
    Android(android::Target),
    Ios(ios::Target),
}

/// The target named `name`: `None` for `host`.
///
/// # Errors
///
/// A name no target has.
pub fn target(name: &str) -> Result<Option<Target>, BoxError> {
    if name == "host" {
        return Ok(None);
    }
    if let Some(t) = android::target(name) {
        return Ok(Some(Target::Android(t)));
    }
    if let Some(t) = ios::target(name) {
        return Ok(Some(Target::Ios(t)));
    }
    Err(format!(
        "target {name}: unknown; try host, android-arm64, android-x64, ios-arm64 or ios-sim-arm64"
    )
    .into())
}

/// The system libraries and link flags of an Android program: a shared
/// library for the app, 16 KiB page ready, with its relative relocations
/// packed (RELR, in the Android tags API 28 and newer read), which keeps a
/// lifted program's millions of code map pointers from taking 24 bytes each.
/// Symbols bind inside the library, so the lifted code's calls into the
/// runtime are direct branches rather than calls through the PLT.
const ANDROID_LIBS: [&str; 9] = [
    "-lm",
    "-ldl",
    "-shared",
    "-Wl,-soname,libmain.so",
    "-Wl,-z,max-page-size=16384",
    "-Wl,--no-undefined",
    "-Wl,-Bsymbolic",
    "-Wl,--pack-dyn-relocs=relr",
    "-Wl,--use-android-relr-tags",
];

/// The tools, libraries and signing of one target, found.
pub enum Build {
    Android(Box<android::AndroidBuild>),
    Ios(Box<ios::IosBuild>),
}

impl Build {
    /// Finds what `t` needs, or says what is missing.
    ///
    /// # Errors
    ///
    /// As [`android::AndroidBuild::prepare`] and [`ios::IosBuild::prepare`].
    pub fn prepare(settings: &Settings, t: Target, name: &str) -> Result<Self, BoxError> {
        Ok(match t {
            Target::Android(a) => {
                Self::Android(Box::new(android::AndroidBuild::prepare(settings, a, name)?))
            }
            Target::Ios(i) => Self::Ios(Box::new(ios::IosBuild::prepare(settings, i, name)?)),
        })
    }

    /// The name `--target` took.
    #[must_use]
    pub fn name(&self) -> &'static str {
        match self {
            Self::Android(a) => a.target.name,
            Self::Ios(i) => i.target.name,
        }
    }

    /// Rust's name for the target.
    #[must_use]
    pub fn triple(&self) -> &'static str {
        match self {
            Self::Android(a) => a.target.triple,
            Self::Ios(i) => i.target.triple,
        }
    }

    /// The compiler command.
    #[must_use]
    pub fn cc(&self) -> Vec<String> {
        match self {
            Self::Android(a) => vec![a.ndk.clang.display().to_string()],
            Self::Ios(i) => i.cc(),
        }
    }

    /// The file the link writes.
    #[must_use]
    pub fn program(&self) -> &'static str {
        match self {
            Self::Android(_) => "libmain.so",
            Self::Ios(_) => "game",
        }
    }

    /// How the program enters.
    #[must_use]
    pub fn entry(&self) -> Entry {
        match self {
            Self::Android(_) => Entry::Android,
            Self::Ios(_) => Entry::Ios,
        }
    }

    /// Whether the code is compiled position independent for a shared
    /// library.
    #[must_use]
    pub fn pic(&self) -> bool {
        matches!(self, Self::Android(_))
    }

    /// The window, sound and platform glue backends.
    ///
    /// # Errors
    ///
    /// The runtime's Vulkan sources cannot be listed.
    pub fn extras(&self, rt: &Path, with_vk: bool) -> std::io::Result<Extras> {
        match self {
            Self::Android(a) => crate::runtime_link::android_extras(rt, &a.deps, with_vk),
            Self::Ios(i) => crate::runtime_link::ios_extras(rt, &i.deps, with_vk),
        }
    }

    /// The system libraries and link flags of the program.
    #[must_use]
    pub fn libs(&self) -> Vec<String> {
        match self {
            Self::Android(_) => ANDROID_LIBS.map(String::from).into(),
            Self::Ios(i) => i.libs(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn android_libraries_pack_their_relative_relocations() {
        assert!(ANDROID_LIBS.contains(&"-Wl,-Bsymbolic"));
        assert!(ANDROID_LIBS.contains(&"-Wl,--pack-dyn-relocs=relr"));
        assert!(ANDROID_LIBS.contains(&"-Wl,--use-android-relr-tags"));
    }

    #[test]
    fn names_pick_the_target_kind() {
        assert_eq!(target("host").unwrap(), None);
        assert!(matches!(
            target("android-arm64").unwrap(),
            Some(Target::Android(_))
        ));
        assert!(matches!(target("ios-arm64").unwrap(), Some(Target::Ios(_))));
        let e = target("wasm").unwrap_err().to_string();
        assert!(e.contains("ios-sim-arm64"), "{e}");
    }
}
