use std::time::Duration;

/// Decides when a plain-output progress line is due: when the percentage
/// crosses a new multiple of `percent`, or `every` has passed since the last
/// line, whichever comes first.
#[derive(Clone, Debug)]
pub struct Throttle {
    percent: u64,
    every: Duration,
    bucket: u64,
    last: Duration,
}

impl Throttle {
    /// A throttle that starts counting at `now`.
    pub fn new(percent: u64, every: Duration, now: Duration) -> Throttle {
        Throttle {
            percent: percent.max(1),
            every,
            bucket: 0,
            last: now,
        }
    }

    /// Whether a line is due for `done` of `total` at time `now`; a true
    /// answer restarts both limits.
    pub fn due(&mut self, done: u64, total: Option<u64>, now: Duration) -> bool {
        let bucket = match total {
            Some(t) if t > 0 => done.min(t) * 100 / t / self.percent,
            _ => self.bucket,
        };
        if bucket > self.bucket || now.saturating_sub(self.last) >= self.every {
            self.bucket = bucket.max(self.bucket);
            self.last = now;
            return true;
        }
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(s: u64) -> Duration {
        Duration::from_secs(s)
    }

    #[test]
    fn a_new_ten_percent_step_is_due() {
        let mut t = Throttle::new(10, at(10), at(0));
        assert!(!t.due(5, Some(100), at(1)));
        assert!(t.due(10, Some(100), at(1)));
        assert!(!t.due(15, Some(100), at(2)));
        assert!(t.due(20, Some(100), at(2)));
    }

    #[test]
    fn ten_seconds_make_a_line_due_without_a_step() {
        let mut t = Throttle::new(10, at(10), at(0));
        assert!(!t.due(1, Some(1000), at(9)));
        assert!(t.due(2, Some(1000), at(10)));
        assert!(!t.due(3, Some(1000), at(19)));
        assert!(t.due(4, Some(1000), at(20)));
    }

    #[test]
    fn a_step_restarts_the_ten_seconds() {
        let mut t = Throttle::new(10, at(10), at(0));
        assert!(t.due(10, Some(100), at(8)));
        assert!(!t.due(11, Some(100), at(17)));
        assert!(t.due(11, Some(100), at(18)));
    }

    #[test]
    fn a_jump_over_several_steps_gives_one_line() {
        let mut t = Throttle::new(10, at(10), at(0));
        assert!(t.due(55, Some(100), at(1)));
        assert!(!t.due(58, Some(100), at(2)));
        assert!(t.due(60, Some(100), at(2)));
    }

    #[test]
    fn no_total_means_time_only() {
        let mut t = Throttle::new(10, at(10), at(0));
        assert!(!t.due(500, None, at(5)));
        assert!(t.due(900, None, at(10)));
    }
}
