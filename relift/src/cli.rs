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
    /// Extract a game dump, and its update if any, into a project.
    Import {
        /// The base game dump.
        base: PathBuf,
        /// The update dump applied on top of the base.
        update: Option<PathBuf>,
    },
    /// Lift, emit and compile the project.
    Build {
        /// A host target to build for; repeat for several. Default: this host.
        #[arg(long)]
        target: Vec<String>,
    },
    /// Build if needed, then run the project.
    Run {
        /// The host target to run on. Default: this host.
        #[arg(long)]
        target: Option<String>,
        /// Log every system call and service request.
        #[arg(long)]
        trace_svc: bool,
        /// Log entry and exit of guest functions matching this glob.
        #[arg(long)]
        trace: Vec<String>,
        /// Stop on the first unknown service command or system call.
        #[arg(long)]
        strict: bool,
    },
    /// Compare lifted functions against the reference interpreter.
    Check {
        /// A guest symbol or address.
        what: String,
    },
    /// Map between a host address, a guest address and a symbol.
    Where {
        /// A host address, guest address or symbol.
        what: String,
    },
}

impl Command {
    /// The subcommand's name as typed.
    pub fn name(&self) -> &'static str {
        match self {
            Command::Import { .. } => "import",
            Command::Build { .. } => "build",
            Command::Run { .. } => "run",
            Command::Check { .. } => "check",
            Command::Where { .. } => "where",
        }
    }
}
