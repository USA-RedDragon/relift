//! The progress reporter a command writes to, set up from the settings and
//! the environment.

use std::io::{IsTerminal, Stderr};

use relift_progress::{ColorChoice, Mode, Reporter, SystemClock, Terminal, Verbosity};

use crate::settings::Settings;

/// The progress output of one command, on standard error.
pub type Ui = Reporter<Stderr>;

/// A reporter for `settings` on the real standard error. Live lines and
/// color follow [`Terminal::detect`]; `color` and the quiet and verbose
/// settings come from the settings.
pub fn open(settings: &Settings) -> Ui {
    let term = Terminal::detect(std::io::stderr().is_terminal());
    from_parts(
        settings,
        &term,
        Box::new(SystemClock::new()),
        std::io::stderr(),
    )
}

fn from_parts<W: std::io::Write>(
    settings: &Settings,
    term: &Terminal,
    clock: Box<dyn relift_progress::Clock>,
    out: W,
) -> Reporter<W> {
    let choice = ColorChoice::parse(&settings.color).unwrap_or(ColorChoice::Auto);
    let verbosity = if settings.quiet {
        Verbosity::Quiet
    } else if settings.verbose {
        Verbosity::Verbose
    } else {
        Verbosity::Normal
    };
    let mode = if term.live { Mode::Live } else { Mode::Plain };
    Reporter::new(out, clock, mode, term.color(choice), term.width, verbosity)
}
