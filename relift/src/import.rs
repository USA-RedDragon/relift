//! `relift import`: extract a dump into the project's import directory and
//! write a `game.toml` stub.

use std::path::{Path, PathBuf};

use relift_manifest::{Game, ModuleRef};
use relift_switch_formats::import::{Options, import};
use relift_switch_formats::nup::UpdateSelect;

use crate::settings::Settings;

/// The error type of a subcommand.
pub type BoxError = Box<dyn std::error::Error + Send + Sync>;

/// The cache root: the `cache-dir` setting, or the user cache directory.
///
/// # Errors
///
/// No setting and no home or cache directory in the environment.
pub fn cache_root(settings: &Settings) -> Result<PathBuf, BoxError> {
    if !settings.cache_dir.as_os_str().is_empty() {
        return Ok(settings.cache_dir.clone());
    }
    if let Some(x) = std::env::var_os("XDG_CACHE_HOME").filter(|v| !v.is_empty()) {
        return Ok(PathBuf::from(x).join("relift"));
    }
    if let Some(l) = std::env::var_os("LOCALAPPDATA").filter(|v| !v.is_empty()) {
        return Ok(PathBuf::from(l).join("relift").join("cache"));
    }
    if let Some(h) = std::env::var_os("HOME").filter(|v| !v.is_empty()) {
        return Ok(PathBuf::from(h).join(".cache").join("relift"));
    }
    Err("no cache directory: set cache-dir".into())
}

/// The import directory of the project in `project`.
///
/// # Errors
///
/// As [`cache_root`], or the project directory has no name.
pub fn import_dir(settings: &Settings, project: &Path) -> Result<PathBuf, BoxError> {
    let abs = std::fs::canonicalize(project)?;
    let name = abs
        .file_name()
        .ok_or("the project directory has no name")?
        .to_string_lossy()
        .into_owned();
    Ok(cache_root(settings)?
        .join("projects")
        .join(name)
        .join("import"))
}

/// Run the import of the decrypted `dump` (a `.nup` or a dump directory)
/// for the project in the working directory, with the update `update`
/// names (a version or `base`; the newest by default). Prints a summary to
/// stderr.
///
/// # Errors
///
/// Settings, format or write errors, or an encrypted dump, each as one
/// line.
pub fn run(settings: &Settings, dump: &Path, update: Option<&str>) -> Result<(), BoxError> {
    let opts = Options {
        update: match update {
            Some(u) => UpdateSelect::parse(u)?,
            None => UpdateSelect::Newest,
        },
    };
    let project = Path::new(".");
    let out = import_dir(settings, project)?;
    std::fs::create_dir_all(&out)?;
    let report = import(dump, &out, &opts)?;
    let game = Game {
        guest: "switch".into(),
        title_id: format!("{:016x}", report.program_id),
        version: report.version,
        import_dir: out.display().to_string(),
        modules: report
            .modules
            .iter()
            .map(|m| ModuleRef {
                name: m.name.clone(),
                build_id: m.build_id.clone(),
            })
            .collect(),
    };
    let manifest = project.join("game.toml");
    let wrote_manifest = if manifest.exists() {
        false
    } else {
        relift_cache::write_if_changed(&manifest, game.to_toml().as_bytes())?
    };
    eprintln!(
        "relift import: title {:016x} v{}, {} modules, {} romfs files, {} DLC, {} files written into {}{}",
        report.program_id,
        report.version,
        report.modules.len(),
        report.romfs_files,
        report.dlc.len(),
        report.written,
        out.display(),
        if wrote_manifest {
            "; wrote game.toml"
        } else {
            ""
        }
    );
    Ok(())
}
