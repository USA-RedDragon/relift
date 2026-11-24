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
    /// Import a decrypted game dump into a project.
    ///
    /// The dump is a .nup file, or a directory holding base/{exefs,romfs},
    /// updates/<version>/{exefs,romfs} and dlc/<title id>/<index>/romfs (or
    /// exefs/ and romfs/ directly). The romfs is not extracted: a .nup's is
    /// read in place (keep the .nup where it is), a tree's becomes one
    /// image file. Encrypted dumps are refused: convert them with a
    /// separate tool first. A Game Boy or Game Boy Color
    /// cartridge ROM (.gb, .gbc) is imported as it is; its battery save
    /// stays next to it as a .sav file.
    Import {
        /// The .nup file, dump directory or cartridge ROM.
        dump: PathBuf,
        /// The update version to apply, or `base` for the base alone
        /// (default: the newest update).
        #[arg(long, value_name = "VERSION|base")]
        update: Option<String>,
        /// Which program to import from a container holding several (its
        /// title id in hex); the import lists them when it must choose.
        #[arg(long, value_name = "TITLE_ID")]
        program: Option<String>,
        /// A folder of the player's own decrypted system data, exported
        /// with switch-tools from their console: the shared font titles
        /// (0100000000000810 to 0100000000000814), one folder per title id.
        /// Its fonts are decoded into the import for pl:u.
        #[arg(long, value_name = "DIR")]
        system_data: Option<PathBuf>,
        /// For a dynamically linked Linux program: the arm64 Linux root file
        /// system, or a cross toolchain's sysroot, that holds its program
        /// interpreter and libraries. They are found the way the interpreter
        /// searches (run paths, the sysroot's ld.so.conf, the default
        /// directories) and copied into the import.
        #[arg(long, value_name = "DIR")]
        sysroot: Option<PathBuf>,
    },
    /// Lift, emit and compile the project.
    Build {
        /// A host target to build for; repeat for several. Default: this host.
        #[arg(long)]
        target: Vec<String>,
        /// Code mods to build in, in order: mod directories (Atmosphere or
        /// Ryujinx layout) or .ips and .pchtxt files, comma-separated or
        /// repeated. Later mods apply over earlier ones; two mods that
        /// write different bytes at one address are refused. For a Game
        /// Boy ROM, IPS, UPS or BPS ROM-hack patches, applied in order.
        #[arg(long, value_delimiter = ',', value_name = "MOD")]
        mods: Vec<PathBuf>,
        /// A development build of a cartridge ROM project, as `relift run`
        /// makes: lifted C compiles at the guest's dev optimization, for
        /// compile time. For a Game Boy project, code without a lifted body
        /// also runs on the device library's CPU and `--check` works, where
        /// a release build stops at it.
        #[arg(long)]
        dev: bool,
    },
    /// Copy the project's game data (romfs, DLC) onto a phone for the app
    /// `relift build --target` made, and check every file's size against
    /// the data manifest; files that differ are copied again, and the push
    /// fails naming the first file that still differs.
    Push {
        /// The app's target: ios-arm64 or android-arm64.
        #[arg(long)]
        target: String,
        /// The device: an iOS identifier or name, or an adb serial.
        /// Default: the one connected device.
        #[arg(long)]
        device: Option<String>,
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
        /// The arguments a Linux program runs with, after `--`.
        #[arg(last = true)]
        args: Vec<String>,
    },
    /// Compare lifted functions against the reference interpreter.
    Check {
        /// A guest symbol or address.
        what: String,
    },
    /// Compare one lifted function with the reference interpreter in a live run.
    ///
    /// Switch projects only.
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
    /// with hints from earlier hot-load runs. Switch projects only; for a
    /// Game Boy ROM use `relift-gbrom functions`.
    Coverage {
        /// Print JSON instead of text.
        #[arg(long)]
        json: bool,
        /// Report only the module of this name.
        #[arg(long, value_name = "NAME")]
        module: Option<String>,
    },
    /// Translate the GPU programs a build recorded again, for the device of
    /// its `shaders/device.json`, and rewrite its tables, without building
    /// the game. Switch 2 builds only.
    Shaders {
        /// The build directory (the folder that holds `shaders/`).
        build_dir: PathBuf,
        /// The float math of the tables to write: exact, native, or both.
        #[arg(long, value_name = "MODE", default_value = "exact",
              value_parser = ["exact", "native", "both"])]
        math: String,
        /// Instead of translating, make every program of the tables in
        /// `build_dir` (or `build_dir/shaders`) on this machine's Vulkan
        /// device, as the launch warm-up does, and name each program the
        /// driver crashes on, hangs on or refuses. Needs a C compiler and
        /// make, which build the runtime's check tool on first use.
        #[arg(long)]
        check_compile: bool,
        /// With `--check-compile`: the `device.json` the tables were made
        /// from, compared with this device's (default: the folder's own).
        #[arg(long, value_name = "FILE", requires = "check_compile")]
        device: Option<PathBuf>,
        /// With `--check-compile`: the folder for the check's scratch files,
        /// failed programs' SPIR-V and report (default: `compile-check-work`).
        #[arg(long, value_name = "DIR", requires = "check_compile")]
        work: Option<PathBuf>,
        /// With `--check-compile`: children running at once.
        #[arg(long, value_name = "N", requires = "check_compile")]
        children: Option<u32>,
        /// With `--check-compile`: compile threads in each child (the
        /// warm-up runs many at once; a crash that needs them needs this).
        #[arg(long, value_name = "N", requires = "check_compile")]
        threads: Option<u32>,
        /// With `--check-compile`: programs per child.
        #[arg(long, value_name = "N", requires = "check_compile")]
        chunk: Option<u32>,
        /// With `--check-compile`: seconds without progress before a child
        /// counts as hung and is killed.
        #[arg(long, value_name = "S", requires = "check_compile")]
        timeout: Option<u32>,
        /// A debugging table in trace mode instead: `<key prefix>[,...]:<condition>`, where the condition is
        /// `nan`, `inf`, `denorm` or `any`, comma-separated, then `@<pc>` and `/R<n>` to narrow it. The selected
        /// programs record the first register write meeting it per instruction (runtime README,
        /// RELIFT_NVN_GPU_TRACE). Written to --trace-dir; the build's own tables stay as they are.
        #[arg(long, value_name = "SELECTION", requires = "trace_dir")]
        trace: Option<String>,
        /// Where the trace-mode table goes (run with RELIFT_NVN_TABLES set to it).
        #[arg(long, value_name = "DIR")]
        trace_dir: Option<PathBuf>,
        /// Discover the programs again from this import folder first (the project's `import`), as a build
        /// does: for a build whose scan outputs (programs, meshes.json, kernels.json) predate this relift.
        #[arg(long, value_name = "IMPORT")]
        discover: Option<PathBuf>,
    },
    /// Explain a stop report a shipped build wrote (missed code, an
    /// instruction it could not run) and add the missed target as a root
    /// for the next build.
    ///
    /// Prints the transfer site and target with the guest instructions
    /// around them, the site's lifted function and its C at the site, and
    /// what the build knew about the target. Run in the project directory
    /// to read its build and add the root (a hint in relift.toml for a Switch
    /// project, the learned file for a Game Boy one).
    Report {
        /// The report file (`relift-stop-*.txt`).
        file: PathBuf,
        /// The build directory to read instead of the project's.
        #[arg(long, value_name = "DIR")]
        build_dir: Option<PathBuf>,
        /// Print only; add no root.
        #[arg(long)]
        no_hint: bool,
    },
}

impl Command {
    /// The subcommand's name as typed.
    pub fn name(&self) -> &'static str {
        match self {
            Command::Import { .. } => "import",
            Command::Build { .. } => "build",
            Command::Run { .. } => "run",
            Command::Push { .. } => "push",
            Command::Check { .. } => "check",
            Command::Diff { .. } => "diff",
            Command::Where { .. } => "where",
            Command::Coverage { .. } => "coverage",
            Command::Info { .. } => "info",
            Command::Report { .. } => "report",
            Command::Shaders { .. } => "shaders",
        }
    }
}
