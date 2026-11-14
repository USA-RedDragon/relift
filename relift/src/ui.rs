//! The progress reporter a command writes to, set up from the settings and
//! the environment.

use std::io::{IsTerminal, Stderr};

use relift_progress::{ColorChoice, Mode, Reporter, SystemClock, Terminal, Verbosity};

use crate::cli::Cli;
use crate::settings::Settings;

/// The progress output of one command, on standard error.
pub type Ui = Reporter<Stderr>;

/// A reporter for `settings` on the real standard error. Live lines and
/// color follow [`Terminal::detect`]; `color` comes from the settings and
/// `-q` and `-v` from the command line.
pub fn open(settings: &Settings, cli: &Cli) -> Ui {
    let term = Terminal::detect(std::io::stderr().is_terminal());
    from_parts(
        settings,
        cli,
        &term,
        Box::new(SystemClock::new()),
        std::io::stderr(),
    )
}

fn from_parts<W: std::io::Write>(
    settings: &Settings,
    cli: &Cli,
    term: &Terminal,
    clock: Box<dyn relift_progress::Clock>,
    out: W,
) -> Reporter<W> {
    let choice = ColorChoice::parse(&settings.color).unwrap_or(ColorChoice::Auto);
    let verbosity = if cli.quiet {
        Verbosity::Quiet
    } else if cli.verbose {
        Verbosity::Verbose
    } else {
        Verbosity::Normal
    };
    let mode = if term.live { Mode::Live } else { Mode::Plain };
    Reporter::new(out, clock, mode, term.color(choice), term.width, verbosity)
}
