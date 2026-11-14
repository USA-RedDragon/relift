use std::io::Write;
use std::time::Duration;

use crate::clock::Clock;
use crate::fmt::{bar, count, duration};
use crate::line::{Line, Paint};
use crate::throttle::Throttle;

/// How much the reporter says.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verbosity {
    /// Only the final summary and errors.
    Quiet,
    /// Stages, counts and times.
    Normal,
    /// Also the detail lines passed to [`Reporter::detail`].
    Verbose,
}

/// Whether the reporter redraws a block of lines or only appends lines.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// Redraw the running stages in place at the bottom.
    Live,
    /// Plain lines, never a cursor move.
    Plain,
}

/// The measured state of a running stage.
#[derive(Clone, Debug, Default)]
pub struct Metrics {
    /// Items finished.
    pub done: u64,
    /// Items in all, when known.
    pub total: Option<u64>,
    /// What the items are, such as `functions` or `shards`.
    pub unit: &'static str,
    /// Items that could not be lifted, when the stage counts them.
    pub unlifted: Option<u64>,
    /// Extra text after the counts, such as the compiler and job count.
    pub note: Option<String>,
    /// Whether a live line shows the elapsed time.
    pub elapsed: bool,
}

struct Row {
    label: String,
    stage: String,
    started: Duration,
    metrics: Option<Metrics>,
    throttle: Throttle,
}

/// Progress output for one command.
pub struct Reporter<W: Write> {
    out: W,
    clock: Box<dyn Clock>,
    mode: Mode,
    color: bool,
    width: usize,
    verbosity: Verbosity,
    label_width: usize,
    rows: Vec<Row>,
    drawn: usize,
    last_draw: Option<Duration>,
}

const STAGE_WIDTH: usize = 8;
const REDRAW: Duration = Duration::from_millis(80);
const LINE_PERCENT: u64 = 10;
const LINE_SECONDS: Duration = Duration::from_secs(10);

impl<W: Write> Reporter<W> {
    /// A reporter that writes to `out`.
    pub fn new(
        out: W,
        clock: Box<dyn Clock>,
        mode: Mode,
        color: bool,
        width: usize,
        verbosity: Verbosity,
    ) -> Reporter<W> {
        Reporter {
            out,
            clock,
            mode,
            color,
            width,
            verbosity,
            label_width: 0,
            rows: Vec::new(),
            drawn: 0,
            last_draw: None,
        }
    }

    /// The sink, for tests.
    pub fn sink(&self) -> &W {
        &self.out
    }

    /// Whether color is on.
    pub fn color(&self) -> bool {
        self.color
    }

    /// The verbosity.
    pub fn verbosity(&self) -> Verbosity {
        self.verbosity
    }

    /// Time since the reporter's clock started.
    pub fn elapsed(&self) -> Duration {
        self.clock.elapsed()
    }

    /// Registers a row (a module, or a build-wide line with an empty label)
    /// and returns its handle. Register every row before the first stage so
    /// the columns line up.
    pub fn row(&mut self, label: &str) -> usize {
        self.label_width = self.label_width.max(label.chars().count());
        let now = self.clock.elapsed();
        self.rows.push(Row {
            label: label.to_string(),
            stage: String::new(),
            started: now,
            metrics: None,
            throttle: Throttle::new(LINE_PERCENT, LINE_SECONDS, now),
        });
        self.rows.len() - 1
    }

    /// Prints a heading line.
    pub fn title(&mut self, text: &str) {
        if self.verbosity != Verbosity::Quiet {
            self.permanent(&Line::new().paint(Paint::Bold, text));
        }
    }

    /// Starts `stage` on `row`; the stage's time starts now.
    pub fn begin(&mut self, row: usize, stage: &str) {
        let now = self.clock.elapsed();
        if let Some(r) = self.rows.get_mut(row) {
            r.stage = stage.to_string();
            r.started = now;
            r.metrics = Some(Metrics::default());
            r.throttle = Throttle::new(LINE_PERCENT, LINE_SECONDS, now);
        }
        self.redraw(true);
    }

    /// Records the counts of the running stage of `row`. A live reporter
    /// redraws at most about every 80 ms; a plain one writes a line every
    /// 10 percent or 10 seconds.
    pub fn update(&mut self, row: usize, metrics: Metrics) {
        let now = self.clock.elapsed();
        let mut due = false;
        if let Some(r) = self.rows.get_mut(row) {
            due = r.throttle.due(metrics.done, metrics.total, now);
            r.metrics = Some(metrics);
        }
        match self.mode {
            Mode::Live => self.redraw(false),
            Mode::Plain if due => self.plain_line(row),
            Mode::Plain => {}
        }
    }

    /// Lets the reporter act on the passage of time: a live reporter
    /// refreshes its elapsed times, a plain one writes a line for a stage
    /// that has been quiet for 10 seconds.
    pub fn tick(&mut self) {
        match self.mode {
            Mode::Live => self.redraw(false),
            Mode::Plain => {
                let now = self.clock.elapsed();
                let due: Vec<usize> = self
                    .rows
                    .iter_mut()
                    .enumerate()
                    .filter_map(|(i, r)| {
                        let m = r.metrics.as_ref()?;
                        r.throttle.due(m.done, None, now).then_some(i)
                    })
                    .collect();
                for i in due {
                    self.plain_line(i);
                }
            }
        }
    }

    /// Finishes the running stage of `row` with a one-line summary; the
    /// stage's name, `summary` and its time become a permanent line.
    pub fn done(&mut self, row: usize, summary: Line) {
        let now = self.clock.elapsed();
        let Some(r) = self.rows.get_mut(row) else {
            return;
        };
        let Some(_) = r.metrics.take() else {
            return;
        };
        let line = Self::stage_line(
            self.label_width,
            &r.label,
            Line::new().paint(Paint::Green, pad(&r.stage, STAGE_WIDTH)),
        )
        .then(summary)
        .text("  ")
        .paint(Paint::Dim, duration(now.saturating_sub(r.started)));
        if self.verbosity != Verbosity::Quiet {
            self.permanent(&line);
        } else {
            self.redraw(true);
        }
    }

    /// A permanent line, unless quiet.
    pub fn note(&mut self, line: &Line) {
        if self.verbosity != Verbosity::Quiet {
            self.permanent(line);
        }
    }

    /// A permanent line shown only with `-v`.
    pub fn detail(&mut self, line: &Line) {
        if self.verbosity == Verbosity::Verbose {
            self.permanent(line);
        }
    }

    /// The final summary: shown at every verbosity.
    pub fn summary(&mut self, line: &Line) {
        self.permanent(line);
    }

    /// An error: shown at every verbosity, `text` in red when color is on.
    pub fn error(&mut self, text: &str) {
        self.permanent(&Line::new().paint(Paint::Red, text));
    }

    fn stage_line(label_width: usize, label: &str, stage: Line) -> Line {
        Line::new()
            .text(format!("  {} ", pad(label, label_width)))
            .then(stage)
            .text(" ")
    }

    fn permanent(&mut self, line: &Line) {
        let mut buf = String::new();
        self.erase_into(&mut buf);
        buf.push_str(&line.render(self.color, None));
        buf.push('\n');
        self.drawn = 0;
        self.draw_into(&mut buf);
        self.write(&buf);
    }

    fn erase_into(&mut self, buf: &mut String) {
        if self.mode == Mode::Live && self.drawn > 0 {
            buf.push_str(&format!("\x1b[{}A\r\x1b[J", self.drawn));
            self.drawn = 0;
        }
    }

    fn live_line(&self, r: &Row, m: &Metrics) -> Line {
        let now = self.clock.elapsed();
        let mut l = Self::stage_line(
            self.label_width,
            &r.label,
            Line::new().paint(Paint::Cyan, pad(&r.stage, STAGE_WIDTH)),
        );
        if let Some(t) = m.total {
            l = l.text(format!("{} ", bar(m.done, t, 20)));
        }
        l = l.then(counts(m, false));
        if m.elapsed {
            l = l
                .text("  ")
                .paint(Paint::Dim, duration(now.saturating_sub(r.started)));
        }
        l
    }

    fn draw_into(&mut self, buf: &mut String) {
        if self.mode != Mode::Live || self.verbosity == Verbosity::Quiet {
            return;
        }
        let width = self.width.saturating_sub(1);
        let mut n = 0;
        for r in &self.rows {
            if let Some(m) = &r.metrics {
                buf.push_str(&self.live_line(r, m).render(self.color, Some(width)));
                buf.push('\n');
                n += 1;
            }
        }
        self.drawn = n;
        self.last_draw = Some(self.clock.elapsed());
    }

    fn redraw(&mut self, force: bool) {
        if self.mode != Mode::Live {
            return;
        }
        let now = self.clock.elapsed();
        if !force
            && self
                .last_draw
                .is_some_and(|t| now.saturating_sub(t) < REDRAW)
        {
            return;
        }
        let mut buf = String::new();
        self.erase_into(&mut buf);
        self.draw_into(&mut buf);
        self.write(&buf);
    }

    fn plain_line(&mut self, row: usize) {
        if self.verbosity == Verbosity::Quiet {
            return;
        }
        let now = self.clock.elapsed();
        let Some(r) = self.rows.get(row) else {
            return;
        };
        let Some(m) = &r.metrics else {
            return;
        };
        let line = Self::stage_line(
            self.label_width,
            &r.label,
            Line::new().paint(Paint::Cyan, pad(&r.stage, STAGE_WIDTH)),
        )
        .then(counts(m, true))
        .text("  ")
        .paint(Paint::Dim, duration(now.saturating_sub(r.started)));
        self.permanent(&line);
    }

    fn write(&mut self, buf: &str) {
        if buf.is_empty() {
            return;
        }
        let _ = self.out.write_all(buf.as_bytes());
        let _ = self.out.flush();
    }
}

fn pad(s: &str, width: usize) -> String {
    format!("{s:<width$}")
}

fn counts(m: &Metrics, percent: bool) -> Line {
    let mut l = Line::new();
    match m.total {
        Some(t) => {
            l = l.text(format!("{}/{}", count(m.done), count(t)));
            if !m.unit.is_empty() {
                l = l.text(format!(" {}", m.unit));
            }
            if percent && t > 0 {
                l = l.text(format!(" ({}%)", m.done.min(t) * 100 / t));
            }
        }
        None => {
            l = l.text(count(m.done));
            if !m.unit.is_empty() {
                l = l.text(format!(" {}", m.unit));
            }
        }
    }
    if let Some(u) = m.unlifted {
        let paint = if u > 0 { Paint::Yellow } else { Paint::Plain };
        l = l.text("  ").paint(paint, format!("{} unlifted", count(u)));
    }
    if let Some(n) = &m.note {
        l = l.text(format!("  {n}"));
    }
    l
}
