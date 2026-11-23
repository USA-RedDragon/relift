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
    if let Err(e) = relift::projectfile::refuse_old_file(std::path::Path::new(".")) {
        eprintln!("relift: {e}");
        return ExitCode::FAILURE;
    }
    let loaded = Configulator::<Settings>::new()
        .with_cli_command(Cli::command())
        .with_file(FileOptions {
            paths: vec![relift::projectfile::FILE.into()],
            ..FileOptions::new(serde_loader(relift::projectfile::settings_from_str))
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
        Command::Import {
            dump,
            update,
            program,
            system_data,
            sysroot,
        } => relift::import::run(
            &settings,
            dump,
            update.as_deref(),
            program.as_deref(),
            system_data.as_deref(),
            sysroot.as_deref(),
            &mut ui,
        ),
        Command::Build { target, mods, dev } => {
            relift::build::run_build(&settings, target, mods, *dev, &mut ui)
        }
        Command::Push { target, device } => {
            relift::push::run_push(&settings, target, device.as_deref(), &mut ui)
        }
        Command::Run {
            target,
            trace_os,
            args,
            ..
        } => match relift::build::run_run(&settings, target.as_deref(), *trace_os, args, &mut ui) {
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
        Command::Shaders {
            build_dir,
            check_compile: true,
            device,
            work,
            children,
            threads,
            chunk,
            timeout,
            ..
        } => match relift::shaders::check_compile(
            &settings,
            &relift::shaders::CheckOptions {
                dir: build_dir,
                device: device.as_deref(),
                work: work.as_deref(),
                jobs: *children,
                threads: *threads,
                chunk: *chunk,
                timeout: *timeout,
            },
        ) {
            Ok(code) => return ExitCode::from(code),
            Err(e) => Err(e),
        },
        Command::Shaders {
            build_dir,
            math,
            trace,
            trace_dir,
            ..
        } => relift::shaders::run(
            build_dir,
            math,
            trace.as_deref().zip(trace_dir.as_deref()),
            &mut ui,
        ),
        Command::Info { binary } => relift::build::run_info(&settings, binary.as_deref()),
        Command::Report {
            file,
            build_dir,
            no_hint,
        } => relift::report::run(
            &settings,
            file,
            build_dir.as_deref(),
            if *no_hint {
                relift::report::HintMode::None
            } else {
                relift::report::HintMode::Add
            },
        ),
        Command::Check { .. } => Err(
            "not implemented yet; `relift diff <function>` compares one function of a Switch project with the reference interpreter, and check/ holds the `relift-check` tool for single instructions"
                .into(),
        ),
        Command::Where { .. } => Err(
            "not implemented yet; a stop report names the module, the guest address and the offset of a miss, and `relift coverage` lists every address as module+offset"
                .into(),
        ),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            ui.error(&format!("relift {}: {e}", cli.command.name()));
            ExitCode::FAILURE
        }
    }
}
