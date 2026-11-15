//! The `relift` command.

use std::process::ExitCode;

use clap::{CommandFactory, FromArgMatches};
use configulator::{
    CLIFlagOptions, Configulator, ConfigulatorError, EnvironmentVariableOptions, FileOptions,
    serde_loader,
};

use relift::cli::{Cli, Command};
use relift::settings::Settings;

fn main() -> ExitCode {
    let loaded = Configulator::<Settings>::new()
        .with_cli_command(Cli::command())
        .with_file(FileOptions {
            paths: vec!["relift.toml".into()],
            ..FileOptions::new(serde_loader(|s| toml::from_str(s)))
        })
        .with_environment_variables(EnvironmentVariableOptions {
            prefix: "RELIFT_".into(),
            separator: "_".into(),
        })
        .with_cli_flags(CLIFlagOptions {
            separator: ".".into(),
        })
        .load_with_matches();

    let (mut settings, matches) = match loaded {
        Ok(loaded) => loaded,
        Err(ConfigulatorError::CLIError(e)) => e.exit(),
        Err(e) => {
            eprintln!("relift: {e}");
            return ExitCode::FAILURE;
        }
    };
    if let Err(e) = settings.resolve_paths() {
        eprintln!("relift: {e}");
        return ExitCode::FAILURE;
    }
    let cli = match Cli::from_arg_matches(&matches) {
        Ok(cli) => cli,
        Err(e) => e.exit(),
    };
    relift_proc::workers::set_jobs(settings.jobs);
    let mut ui = relift::ui::open(&settings, &cli);
    let result = match &cli.command {
        Command::Import { dump, update } => {
            relift::import::run(&settings, dump, update.as_deref(), &mut ui)
        }
        Command::Build { target, mods } => {
            relift::build::run_build(&settings, target, mods, &mut ui)
        }
        Command::Run {
            target, trace_os, ..
        } => match relift::build::run_run(&settings, target.as_deref(), *trace_os, &mut ui) {
            Ok(code) => return ExitCode::from(u8::try_from(code).unwrap_or(1)),
            Err(e) => Err(e),
        },
        Command::Coverage { json, module } => {
            relift::coverage::run(&settings, *json, module.as_deref(), &mut ui)
        }
        Command::Diff {
            function,
            count,
            skip,
            engine,
            serve,
            flip_read,
        } => match relift::diff::run(
            &settings,
            &relift::diff::Request {
                function: function.as_deref(),
                count: *count,
                skip: *skip,
                engine: engine.as_deref(),
                serve: *serve,
                flip_read: *flip_read,
            },
            &mut ui,
        ) {
            Ok(code) => return ExitCode::from(code),
            Err(e) => Err(e),
        },
        Command::Info { binary } => relift::build::run_info(&settings, binary.as_deref()),
        _ => Err("not implemented".into()),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            ui.error(&format!("relift {}: {e}", cli.command.name()));
            ExitCode::FAILURE
        }
    }
}
