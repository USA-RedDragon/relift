//! `relift-vkgen`: writes the Vulkan forwarding sources from a pinned vk.xml,
//! or with `--check` fails when the committed sources differ.

use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use relift_switch_vkgen::{Config, generate};

/// Generate the Switch runtime's Vulkan forwarding layer from vk.xml.
#[derive(Parser, Debug)]
#[command(name = "relift-vkgen", version)]
struct Cli {
    /// The registry file (vk.xml at the pinned tag).
    #[arg(long)]
    xml: PathBuf,
    /// Configuration files, merged in order (lists concatenate).
    #[arg(long, required = true)]
    config: Vec<PathBuf>,
    /// The output directory.
    #[arg(long)]
    out: PathBuf,
    /// Compare with the files in the output directory instead of writing; exit 1 on any difference.
    #[arg(long)]
    check: bool,
}

fn run(cli: Cli) -> Result<bool, Box<dyn std::error::Error>> {
    let mut cfg = Config::default();
    for c in &cli.config {
        let text = std::fs::read_to_string(c).map_err(|e| format!("{}: {e}", c.display()))?;
        cfg.merge(&text)
            .map_err(|e| format!("{}: {e}", c.display()))?;
    }
    let xml =
        std::fs::read_to_string(&cli.xml).map_err(|e| format!("{}: {e}", cli.xml.display()))?;
    let files = generate(&xml, &cfg)?;
    let mut same = true;
    for f in files {
        let path = cli.out.join(f.name);
        if cli.check {
            let old = std::fs::read_to_string(&path).unwrap_or_default();
            if old != f.text {
                eprintln!("relift-vkgen: {} is stale; regenerate it", path.display());
                same = false;
            }
        } else {
            relift_cache::write_if_changed(&path, f.text.as_bytes())
                .map_err(|e| format!("{}: {e}", path.display()))?;
        }
    }
    Ok(same)
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("relift-vkgen: {e}");
            ExitCode::FAILURE
        }
    }
}
