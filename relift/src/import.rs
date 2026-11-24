//! `relift import`: extract a dump into the project's import directory and
//! write a `relift.toml` stub.

use std::path::{Path, PathBuf};

use relift_manifest::{Game, ModuleRef};
use relift_progress::{Line, Metrics, Paint, bytes, count, duration, noun};
use relift_switch_formats::import::{Event, Options, import_with};
use relift_switch_formats::nup::UpdateSelect;

use crate::settings::Settings;
use crate::ui::Ui;

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

/// The import directory as `relift.toml` names it: relative to the cache
/// root, so the manifest reads the same wherever the cache lives.
///
/// # Errors
///
/// As [`cache_root`].
/// The path and table hash of `dump` for `relift.toml`, when it is a
/// `.nup` that opens; `None` for any other dump.
fn dump_identity(dump: &Path) -> Option<(String, String)> {
    let nup = crate::info::open_nup(dump)?;
    let path = std::fs::canonicalize(dump).ok()?;
    let toc: String = nup
        .superblock()
        .toc_sha256
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    Some((path.to_str()?.to_string(), toc))
}

pub fn import_dir_value(settings: &Settings, dir: &Path) -> Result<String, BoxError> {
    let root = cache_root(settings)?;
    Ok(match dir.strip_prefix(&root) {
        Ok(rel) => rel.to_string_lossy().replace('\\', "/"),
        Err(_) => dir.display().to_string(),
    })
}

/// The import directory of the project in `project`.
///
/// # Errors
///
/// As [`cache_root`], or the project directory has no name.
pub fn import_dir(settings: &Settings, project: &Path) -> Result<PathBuf, BoxError> {
    let abs = crate::paths::canonical(project)?;
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
/// names (a version or `base`; the newest by default). Shows each stage's
/// files and bytes as they are extracted, then a summary.
///
/// # Errors
///
/// Settings, format or write errors, or an encrypted dump, each as one
/// line.
pub fn run(
    settings: &Settings,
    dump: &Path,
    update: Option<&str>,
    program: Option<&str>,
    system_data: Option<&Path>,
    (sysroot, extra): (Option<&Path>, &[String]),
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let guest = crate::guests::of_input(dump);
    if sysroot.is_some() && !guest.is_some_and(|g| g.sysroot) {
        return Err("--sysroot is for a dynamically linked Linux program".into());
    }
    if let Some(guest) = guest {
        let what = guest.input;
        if system_data.is_some() || program.is_some() {
            let has = what
                .strip_suffix(" ROM")
                .or_else(|| what.strip_suffix(" image"))
                .unwrap_or(what);
            return Err(format!(
                "--system-data and --program are for Switch dumps; {has} has no system fonts"
            )
            .into());
        }
        if update.is_some() {
            return Err(format!("--update is for Switch dumps; {what} has no updates").into());
        }
        return (guest.import)(settings, dump, (sysroot, extra), ui);
    }
    let opts = Options {
        update: match update {
            Some(u) => UpdateSelect::parse(u)?,
            None => UpdateSelect::Newest,
        },
        program: program
            .map(|p| u64::from_str_radix(p.trim_start_matches("0x"), 16))
            .transpose()
            .map_err(|e| format!("--program: not a hex title id ({e})"))?,
    };
    let project = Path::new(".");
    let out = import_dir(settings, project)?;
    std::fs::create_dir_all(&out)?;
    let title = format!("relift import: {}", dump.display());
    let mut titled = false;
    let rows = [ui.row("exefs"), ui.row("romfs"), ui.row("dlc")];
    let mut stage = Stage::default();
    let report = import_with(dump, &out, &opts, &mut |event| match event {
        Event::Begin {
            stage: name,
            files,
            bytes,
        } => {
            let (row, label) = match name {
                "exefs" => (rows[0], "exefs"),
                "romfs" => (rows[1], "romfs"),
                _ => (rows[2], name),
            };
            if !titled {
                titled = true;
                ui.title(&title);
            }
            stage = Stage {
                row,
                total_files: files,
                total_bytes: bytes,
                ..Stage::default()
            };
            ui.begin(row, "extract");
            stage.update(ui, label);
        }
        Event::Chunk(n) => {
            stage.bytes += n;
            stage.update(ui, "");
        }
        Event::File => {
            stage.files += 1;
            stage.update(ui, "");
        }
        Event::End => {
            let mut text = format!("{}, {}", noun(stage.files, "file"), bytes(stage.bytes));
            if let Some(l) = &stage.label {
                text = format!("{l}: {text}");
            }
            ui.done(stage.row, Line::new().text(text));
        }
    })?;
    let game = Game {
        guest: "switch".into(),
        title_id: format!("{:016x}", report.program_id),
        version: report.version,
        import_dir: import_dir_value(settings, &out)?,
        dump: dump_identity(dump),
        modules: report
            .modules
            .iter()
            .map(|m| ModuleRef {
                name: m.name.clone(),
                build_id: m.build_id.clone(),
            })
            .collect(),
    };
    let fonts = match system_data {
        Some(sd) => {
            let (found, missing) =
                crate::switch::fonts::import(sd, &out.join(crate::switch::fonts::DIR))?;
            let names = |v: &[relift_switch_formats::bfttf::FontType]| {
                v.iter().map(|t| t.name()).collect::<Vec<_>>().join(", ")
            };
            if !missing.is_empty() {
                ui.note(&Line::new().paint(Paint::Yellow, "warning").text(format!(
                    ": --system-data: no {} font in {}",
                    names(&missing),
                    sd.display()
                )));
            }
            format!(", {} system font(s) ({})", found.len(), names(&found))
        }
        None => String::new(),
    };
    let manifest = project.join("relift.toml");
    let wrote_manifest = crate::projectfile::write_stub(&manifest, &game.to_toml())?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " title {:016x} v{}, {}, {}, {} DLC{}, {} written into {}{}, {}",
        report.program_id,
        report.version,
        noun(report.modules.len() as u64, "module"),
        noun(report.romfs_files as u64, "romfs file"),
        report.dlc.len(),
        fonts,
        noun(report.written as u64, "file"),
        out.display(),
        if wrote_manifest {
            "; wrote relift.toml"
        } else {
            ""
        },
        duration(ui.elapsed()),
    )));
    Ok(())
}

/// The running totals of the import stage in progress.
#[derive(Default)]
struct Stage {
    row: usize,
    total_files: Option<u64>,
    total_bytes: Option<u64>,
    files: u64,
    bytes: u64,
    label: Option<String>,
}

impl Stage {
    fn update(&mut self, ui: &mut Ui, label: &str) {
        if !label.is_empty() {
            self.label = label.strip_prefix("dlc/").map(str::to_string);
        }
        let files = match self.total_files {
            Some(t) => format!("{}/{} files", count(self.files), count(t)),
            None => format!("{} files", count(self.files)),
        };
        ui.update(
            self.row,
            Metrics {
                done: self.bytes,
                total: self.total_bytes,
                bytes: true,
                note: Some(match &self.label {
                    Some(l) => format!("{l}  {files}"),
                    None => files,
                }),
                elapsed: true,
                ..Metrics::default()
            },
        );
    }
}
