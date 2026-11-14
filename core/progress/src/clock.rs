use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// A source of elapsed time, injected so tests do not wait.
pub trait Clock: Send {
    /// The time since the clock started.
    fn elapsed(&self) -> Duration;
}

/// The wall clock, started when it is made.
#[derive(Debug)]
pub struct SystemClock(Instant);

impl SystemClock {
    /// A clock that starts now.
    pub fn new() -> SystemClock {
        SystemClock(Instant::now())
    }
}

impl Default for SystemClock {
    fn default() -> SystemClock {
        SystemClock::new()
    }
}

impl Clock for SystemClock {
    fn elapsed(&self) -> Duration {
        self.0.elapsed()
    }
}

/// A clock that only moves when told to; clones share the time.
#[derive(Clone, Debug, Default)]
pub struct ManualClock(Arc<Mutex<Duration>>);

impl ManualClock {
    /// A clock at zero.
    pub fn new() -> ManualClock {
        ManualClock::default()
    }

    /// Moves the clock forward.
    pub fn advance(&self, by: Duration) {
        if let Ok(mut t) = self.0.lock() {
            *t += by;
        }
    }
}

impl Clock for ManualClock {
    fn elapsed(&self) -> Duration {
        self.0.lock().map_or(Duration::ZERO, |t| *t)
    }
}
