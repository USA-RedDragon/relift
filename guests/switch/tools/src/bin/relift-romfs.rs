//! `relift-romfs`: list, extract and print the files of a romfs image.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_switch_formats::romfs::Romfs;
use relift_switch_tools::{BoxError, input, print_json, run};
use serde_json::json;

/// Work with a decrypted romfs image.
#[derive(Parser, Debug)]
#[command(name = "relift-romfs", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// List the files: path, size.
    Ls {
        /// The romfs image, or - for stdin.
        image: PathBuf,
        /// Print JSON.
        #[arg(long)]
        json: bool,
    },
    /// Write the whole tree into a directory.
    Extract {
        /// The romfs image, or - for stdin.
        image: PathBuf,
        /// The output directory.
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Write one file to stdout.
    Cat {
        /// The romfs image, or - for stdin.
        image: PathBuf,
        /// The file's path inside the romfs.
        path: String,
    },
}

fn main() -> ExitCode {
    run::<Cli>("relift-romfs", |cli| match cli.command {
        Command::Ls { image, json } => ls(&open(&image)?, json),
        Command::Extract { image, output } => {
            open(&image)?.extract(&output)?;
            Ok(())
        }
        Command::Cat { image, path } => {
            let romfs = open(&image)?;
            let file = romfs
                .files()
                .iter()
                .find(|f| f.path == path.trim_start_matches('/'))
                .ok_or_else(|| format!("{path}: no such file in the romfs"))?;
            let mut out = std::io::stdout().lock();
            romfs.copy_file(file, &mut out)?;
            out.flush()?;
            Ok(())
        }
    })
}

fn open(path: &Path) -> Result<Romfs, BoxError> {
    Ok(Romfs::open(input(path)?)?)
}

fn ls(romfs: &Romfs, as_json: bool) -> Result<(), BoxError> {
    if as_json {
        let list: Vec<_> = romfs
            .files()
            .iter()
            .map(|f| json!({ "path": f.path, "size": f.size }))
            .collect();
        return print_json(&json!(list));
    }
    let mut w = std::io::stdout().lock();
    for f in romfs.files() {
        writeln!(w, "{}\t{}", f.path, f.size)?;
    }
    Ok(())
}
