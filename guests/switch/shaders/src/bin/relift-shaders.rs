//! `relift-shaders`: find the shader programs of a Switch 2 (SM86) or
//! Switch 1 (SM53) game.

use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;

use clap::{Parser, Subcommand};
use relift_switch_formats::storage::FileStorage;
use relift_switch_shaders::{Arch, DirSource, NupSource, Options, Source, scan};

/// Find every NVIDIA shader program of a game.
#[derive(Parser, Debug)]
#[command(name = "relift-shaders", version)]
struct Cli {
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Scan a `.nup`, or an extracted `exefs`/`romfs` tree.
    Scan {
        /// The `.nup` file or the directory.
        input: PathBuf,
        /// The output directory: `<sha256>.bin` per program,
        /// `manifest.json`, `meshes.json` and `summary.json`.
        #[arg(long)]
        out: PathBuf,
        /// Where per-file results are cached (default: `<out>/.cache`).
        #[arg(long)]
        cache: Option<PathBuf>,
        /// Worker threads (default: one per CPU).
        #[arg(short = 'j', long, default_value_t = 0)]
        jobs: usize,
        /// Only scan this package of a `.nup`.
        #[arg(long)]
        package: Option<usize>,
        /// Look for SM53 (Switch 1, Maxwell) programs instead of SM86.
        #[arg(long)]
        sm53: bool,
    },
}

type BoxError = Box<dyn std::error::Error + Send + Sync>;

fn main() -> ExitCode {
    match body(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("relift-shaders: {e}");
            ExitCode::FAILURE
        }
    }
}

fn body(cli: Cli) -> Result<(), BoxError> {
    let Cmd::Scan {
        input,
        out,
        cache,
        jobs,
        package,
        sm53,
    } = cli.command;
    let source: Box<dyn Source> = if input.is_dir() {
        Box::new(DirSource::new(&input))
    } else {
        Box::new(NupSource::new(Arc::new(FileStorage::open(&input)?))?)
    };
    let opts = Options {
        arch: if sm53 { Arch::Sm53 } else { Arch::Sm86 },
        jobs,
        cache: Some(cache.unwrap_or_else(|| out.join(".cache"))),
        package,
    };
    let report = scan(&*source, &out, &opts)?;
    relift_cache::write_if_changed(
        &out.join("manifest.json"),
        &serde_json::to_vec(&report.manifest_json())?,
    )?;
    relift_cache::write_if_changed(
        &out.join("meshes.json"),
        &serde_json::to_vec(&report.meshes_json())?,
    )?;
    let summary = serde_json::to_vec_pretty(&report.summary_json())?;
    relift_cache::write_if_changed(&out.join("summary.json"), &summary)?;
    println!("{}", String::from_utf8_lossy(&summary));
    Ok(())
}
