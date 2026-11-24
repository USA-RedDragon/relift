//! `relift-vkgen`: writes the Vulkan forwarding sources from a pinned vk.xml,
//! or with `--check` fails when the committed sources differ.

use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use relift_gpu_apigen::{Config, generate_from};

/// Generate the GPU-API forwarding of the runtime from the Khronos registries.
#[derive(Parser, Debug)]
#[command(name = "relift-vkgen", version)]
struct Cli {
    /// The registry files at their pinned commits: the main one (vk.xml,
    /// gl.xml or egl.xml), then each `[[registry.extra]]` file in order.
    #[arg(long, required = true)]
    xml: Vec<PathBuf>,
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
    let mut xmls = Vec::new();
    for x in &cli.xml {
        let text = std::fs::read_to_string(x).map_err(|e| format!("{}: {e}", x.display()))?;
        xmls.push(text);
    }
    let files = generate_from(&xmls, &cfg)?;
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
