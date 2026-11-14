//! The subcommands, as a clap-derive tree that configulator adds the global
//! settings flags to.

use std::path::PathBuf;

use clap::{Parser, Subcommand};

/// relift: recompile console game code to portable C11.
#[derive(Parser, Debug)]
#[command(name = "relift", version, propagate_version = true)]
pub struct Cli {
    /// Print only the final summary and errors.
    ///
    /// A flag of its own, not a setting: configulator's boolean flags take
    /// an optional value and would swallow the subcommand in `relift -q
    /// build`.
    #[arg(short = 'q', long, global = true, conflicts_with = "verbose")]
    pub quiet: bool,
    /// Also print each file as it compiles and every instruction that does
    /// not lift.
    #[arg(short = 'v', long, global = true)]
    pub verbose: bool,
    #[command(subcommand)]
    pub command: Command,
}

/// One `relift` action.
#[derive(Subcommand, Debug)]
pub enum Command {
    /// Extract a decrypted game dump into a project.
    ///
    /// The dump is a .nup file, or a directory holding base/{exefs,romfs},
    /// updates/<version>/{exefs,romfs} and dlc/<title id>/<index>/romfs (or
    /// exefs/ and romfs/ directly). Encrypted dumps are refused: convert
    /// them with a separate tool first.
    Import {
        /// The .nup file or dump directory.
        dump: PathBuf,
        /// The update version to apply, or `base` for the base alone
        /// (default: the newest update).
        #[arg(long, value_name = "VERSION|base")]
        update: Option<String>,
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
        /// Log every guest OS call and service request (RELIFT_TRACE_OS).
        #[arg(long)]
        trace_os: bool,
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
    /// Compare one lifted function with the reference interpreter in a live run.
    ///
    /// Builds a dev build and runs it under gdb. At each call of the function
    /// the same guest function runs in the interpreter from the same registers
    /// and live guest memory, the lifted code runs with the other threads
    /// frozen, and the registers and every page the interpreter touched are
    /// compared. A run that reaches a system call or a runtime-provided
    /// function is reported and not compared. Exits 1 after a mismatch and 2
    /// when no call was compared.
    Diff {
        /// The function: a host name (`_ZN5guest...`), `module:symbol` (the
        /// guest's mangled symbol) or `module+0xoffset` for an unnamed one.
        /// With `--serve`, nothing.
        function: Option<String>,
        /// How many calls to compare.
        #[arg(long, default_value_t = 1)]
        count: u32,
        /// How many calls to let pass first.
        #[arg(long, default_value_t = 0)]
        skip: u32,
        /// Another engine to compare against, a program that speaks the
        /// protocol of `--serve` (docs/abi.md, "Differential checks"), such
        /// as the Unicorn one in check/.
        #[arg(long, value_name = "COMMAND")]
        engine: Option<String>,
        /// Speak the protocol on standard input and output as the
        /// interpreter engine, for a driver that runs the program.
        #[arg(long, conflicts_with_all = ["function", "engine"])]
        serve: bool,
        /// Flip a bit of the Nth memory read of the interpreter run, so a
        /// correct build must report a mismatch (a control for the tool).
        #[arg(long, value_name = "N", default_value_t = 0)]
        flip_read: u64,
    },
    /// Map between a host address, a guest address and a symbol.
    Where {
        /// A host address, guest address or symbol.
        what: String,
    },
    /// Print the self-description of a built game binary, without running
    /// it: the JSON document its `--relift-info` flag prints.
    Info {
        /// The binary to read. Default: the project's last build.
        binary: Option<PathBuf>,
    },
    /// List what discovery could not resolve: per module, functions,
    /// instructions not lifted and every unresolved indirect call or jump,
    /// with hints from earlier hot-load runs.
    Coverage {
        /// Print JSON instead of text.
        #[arg(long)]
        json: bool,
        /// Report only the module of this name.
        #[arg(long, value_name = "NAME")]
        module: Option<String>,
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
            Command::Diff { .. } => "diff",
            Command::Where { .. } => "where",
            Command::Coverage { .. } => "coverage",
            Command::Info { .. } => "info",
        }
    }
}
