//! The subcommands, as a clap-derive tree that configulator adds the global
//! settings flags to.

use std::path::PathBuf;

use clap::{Parser, Subcommand};

/// relift: recompile console game code to portable C11.
#[derive(Parser, Debug)]
#[command(name = "relift", version, propagate_version = true)]
pub struct Cli {
    #[command(subcommand)]
    pub command: Command,
}

/// One `relift` action.
#[derive(Subcommand, Debug)]
pub enum Command {
    /// Extract a game dump into a project.
    Import {
        /// The dump to import.
        dump: PathBuf,
    },
    /// Lift, emit and compile the project.
    Build {
        /// The host target to build for.
        #[arg(long)]
        target: Option<String>,
    },
    /// Build if needed, then run the project.
    Run {
        /// The host target to run on.
        #[arg(long)]
        target: Option<String>,
    },
    /// Compare lifted functions against the reference interpreter.
    Check,
}

impl Command {
    /// The subcommand's name as typed.
    pub fn name(&self) -> &'static str {
        match self {
            Command::Import { .. } => "import",
            Command::Build { .. } => "build",
            Command::Run { .. } => "run",
            Command::Check => "check",
        }
    }
}
