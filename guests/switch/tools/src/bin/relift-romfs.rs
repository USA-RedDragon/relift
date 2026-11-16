//! `relift-romfs`: list, hash, extract and print the files of a romfs, and
//! write it as one plain image. A romfs is a plain image, a RomFS map
//! (`romfs.map`, as `relift import` writes from a `.nup`), an extracted tree,
//! or `-` for an image on stdin.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_switch_formats::romfs::Romfs;
use relift_switch_formats::romfs_image;
use relift_switch_formats::storage::Shared;
use relift_switch_formats::{Sha256Writer, hex};
use relift_switch_tools::{BoxError, input, print_json, run};
use serde_json::json;

/// Work with a decrypted romfs: an image, a RomFS map or an extracted tree.
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
    /// Print the SHA-256 of every file (`<sha256>  <size>  <path>`, sorted by
    /// path) and last of the whole image, so two forms of one romfs (a tree
    /// and the map of a `.nup`) can be compared.
    Hash {
        /// The romfs image, or - for stdin.
        image: PathBuf,
    },
    /// Write the romfs as one plain image file (what a phone gets).
    Image {
        /// The romfs image, or - for stdin.
        image: PathBuf,
        /// The image file to write.
        #[arg(short, long)]
        output: PathBuf,
    },
}

fn main() -> ExitCode {
    run::<Cli>("relift-romfs", |cli| match cli.command {
        Command::Ls { image, json } => ls(&open(&image)?, json),
        Command::Extract { image, output } => {
            open(&image)?.extract(&output)?;
            Ok(())
        }
        Command::Hash { image } => hash(&image),
        Command::Image { image, output } => {
            let s = source(&image)?;
            let mut w = relift_cache::StreamWriter::new(&output)?;
            let mut buf = vec![0u8; 1 << 20];
            let mut off = 0u64;
            while off < s.size() {
                let n = (buf.len() as u64).min(s.size() - off) as usize;
                s.read_at(off, &mut buf[..n])?;
                w.write_all(&buf[..n])?;
                off += n as u64;
            }
            w.finish()?;
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

fn source(path: &Path) -> Result<Shared, BoxError> {
    if path.as_os_str() == "-" {
        return input(path);
    }
    Ok(romfs_image::open(path)?)
}

fn open(path: &Path) -> Result<Romfs, BoxError> {
    Ok(Romfs::open(source(path)?)?)
}

fn hash(path: &Path) -> Result<(), BoxError> {
    let s = source(path)?;
    let romfs = Romfs::open(s.clone())?;
    let mut w = std::io::stdout().lock();
    let mut files: Vec<_> = romfs.files().iter().collect();
    files.sort_by(|a, b| a.path.cmp(&b.path));
    for f in files {
        let mut h = Sha256Writer::default();
        romfs.copy_file(f, &mut h)?;
        writeln!(w, "{}  {}  {}", hex(&h.finish()), f.size, f.path)?;
    }
    let mut h = Sha256Writer::default();
    let mut buf = vec![0u8; 1 << 20];
    let mut off = 0u64;
    while off < s.size() {
        let n = (buf.len() as u64).min(s.size() - off) as usize;
        s.read_at(off, &mut buf[..n])?;
        h.write_all(&buf[..n])?;
        off += n as u64;
    }
    writeln!(w, "{}  {}  (image)", hex(&h.finish()), s.size())?;
    Ok(())
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
