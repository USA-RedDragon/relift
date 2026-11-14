//! Live progress for the long commands (`build`, `import`).
//!
//! A [`Reporter`] writes to one sink (standard error in the tool) in one of
//! two modes. On a terminal it keeps a block of live lines at the bottom
//! and redraws it; a finished module collapses to one summary line.
//! Everywhere else (no terminal, `TERM=dumb`, CI) it writes plain lines
//! and never moves the cursor: one line per finished stage, and a progress
//! line for a long stage every 10 percent or every 10 seconds, whichever
//! comes first.
//!
//! Color is separate from redrawing: [`ColorChoice`] and the environment
//! decide it through [`Terminal`]. The reporter takes its time from a
//! [`Clock`], so tests drive it with a [`ManualClock`].

mod clock;
mod fmt;
mod line;
mod report;
mod term;
mod throttle;

pub use clock::{Clock, ManualClock, SystemClock};
pub use fmt::{bar, bytes, count, duration, noun};
pub use line::{Line, Paint};
pub use report::{Metrics, Mode, Reporter, Verbosity};
pub use term::{ColorChoice, Terminal};
pub use throttle::Throttle;
