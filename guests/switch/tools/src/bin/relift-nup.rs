//! `relift-nup`: list and extract NUP containers (decrypted Switch dumps).

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_switch_formats::hex;
use relift_switch_formats::nup::{Nup, PackageKind, UpdateSelect};
use relift_switch_tools::{BoxError, input, print_json, run};
use serde_json::json;

/// List and extract NUP containers.
#[derive(Parser, Debug)]
#[command(name = "relift-nup", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// List the packages and their files.
    Ls {
        /// The `.nup` file, or - for stdin.
        input: PathBuf,
        /// Print JSON instead of text.
        #[arg(long)]
        json: bool,
    },
    /// Extract one package, or the effective title, verifying every file.
    Extract {
        /// The `.nup` file, or - for stdin.
        input: PathBuf,
        /// The directory to extract into.
        #[arg(short, long)]
        output: PathBuf,
        /// What to extract: `title` (the default: the base with the update
        /// layered over it, as `exefs/` and `romfs/`), `base`,
        /// `update:<version>` (decimal or 0x hex), or `dlc:<id>:<index>`
        /// (hex title id, index among that id's packages). A single package
        /// is extracted whole, as its own tree.
        #[arg(long, default_value = "title")]
        package: String,
        /// For `title`: the update version to use (decimal or 0x hex), or
        /// `base` for the base alone. The newest update is used by default.
        #[arg(long)]
        update: Option<String>,
    },
}

fn version(s: &str) -> Result<u32, BoxError> {
    let v = match s.strip_prefix("0x") {
        Some(h) => u32::from_str_radix(h, 16),
        None => s.parse(),
    };
    v.map_err(|_| format!("not a version: {s}").into())
}

fn find_package(n: &Nup, spec: &str) -> Result<usize, BoxError> {
    let pk = n.packages();
    let found = if spec == "base" {
        pk.iter().position(|p| p.record.kind == PackageKind::Base)
    } else if let Some(v) = spec.strip_prefix("update:") {
        let v = version(v)?;
        pk.iter()
            .position(|p| p.record.kind == PackageKind::Update && p.record.version == v)
    } else if let Some(r) = spec.strip_prefix("dlc:") {
        let (id, idx) = r.split_once(':').unwrap_or((r, "0"));
        let id = u64::from_str_radix(id.trim_start_matches("0x"), 16)
            .map_err(|_| format!("not a title id: {id}"))?;
        let idx: usize = idx.parse().map_err(|_| format!("not an index: {idx}"))?;
        pk.iter()
            .enumerate()
            .filter(|(_, p)| p.record.kind == PackageKind::Addon && p.record.title_id == id)
            .nth(idx)
            .map(|(i, _)| i)
    } else {
        return Err(format!(
            "unknown --package {spec}: use title, base, update:<ver>, dlc:<id>:<idx>"
        )
        .into());
    };
    found.ok_or_else(|| format!("no package {spec} in this file").into())
}

fn ls(n: &Nup, json: bool) -> Result<(), BoxError> {
    let mut out = Vec::new();
    for (i, p) in n.packages().iter().enumerate() {
        let files: Vec<_> = p
            .files
            .iter()
            .map(|(path, e)| {
                json!({
                    "path": path,
                    "size": e.size,
                    "stored": e.extent_size,
                    "codec": e.codec.name(),
                    "sha256": hex(&e.sha256),
                })
            })
            .collect();
        out.push(json!({
            "index": i,
            "kind": p.record.kind.name(),
            "title_id": format!("{:016x}", p.record.title_id),
            "version": p.record.version,
            "display_version": p.display_version(),
            "name": p.name(),
            "publisher": p.publisher(),
            "standalone_update": p.standalone_update(),
            "files": files,
        }));
    }
    if json {
        return print_json(&json!({
            "uuid": hex(&n.superblock().uuid),
            "packages": out,
        }));
    }
    let mut w = std::io::stdout().lock();
    for p in &out {
        writeln!(
            w,
            "[{}] {} {} version {} ({}){}",
            p["index"],
            p["kind"].as_str().unwrap_or(""),
            p["title_id"].as_str().unwrap_or(""),
            p["version"],
            p["display_version"].as_str().unwrap_or("?"),
            p["name"]
                .as_str()
                .map(|s| format!(" {s}"))
                .unwrap_or_default(),
        )?;
        for f in p["files"].as_array().into_iter().flatten() {
            writeln!(
                w,
                "    {:>12}  {:<4}  {}",
                f["size"],
                f["codec"].as_str().unwrap_or(""),
                f["path"].as_str().unwrap_or("")
            )?;
        }
    }
    Ok(())
}

fn extract(
    n: &Nup,
    out: &std::path::Path,
    package: &str,
    update: Option<&str>,
) -> Result<(), BoxError> {
    let written = if package == "title" {
        let sel = match update {
            None => UpdateSelect::Newest,
            Some(v) => UpdateSelect::parse(v)?,
        };
        let t = n.effective_title(sel)?;
        n.extract_exefs(&t, &out.join("exefs"))? + n.extract_romfs(&t, &out.join("romfs"))?
    } else {
        if update.is_some() {
            return Err("--update only applies to --package title".into());
        }
        n.extract_package(find_package(n, package)?, out)?
    };
    eprintln!("{written} files written");
    Ok(())
}

fn main() -> ExitCode {
    run::<Cli>("relift-nup", |cli| match cli.command {
        Command::Ls { input: path, json } => ls(&Nup::open(input(&path)?)?, json),
        Command::Extract {
            input: path,
            output,
            package,
            update,
        } => extract(
            &Nup::open(input(&path)?)?,
            &output,
            &package,
            update.as_deref(),
        ),
    })
}
