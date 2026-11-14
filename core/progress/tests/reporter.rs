//! The reporter's output for both modes, with a manual clock.

use std::time::Duration;

use relift_progress::{Line, ManualClock, Metrics, Mode, Paint, Reporter, Verbosity};

fn reporter(mode: Mode, color: bool, v: Verbosity) -> (Reporter<Vec<u8>>, ManualClock) {
    let clock = ManualClock::new();
    let r = Reporter::new(Vec::new(), Box::new(clock.clone()), mode, color, 100, v);
    (r, clock)
}

fn text(r: &Reporter<Vec<u8>>) -> String {
    String::from_utf8_lossy(r.sink()).into_owned()
}

fn m(done: u64, total: u64) -> Metrics {
    Metrics {
        done,
        total: Some(total),
        unit: "functions",
        unlifted: Some(0),
        ..Metrics::default()
    }
}

#[test]
fn plain_mode_writes_throttled_lines_and_a_line_per_finished_stage() {
    let (mut r, clock) = reporter(Mode::Plain, false, Verbosity::Normal);
    let a = r.row("nnrtld");
    let b = r.row("main");
    r.title("relift build: demo (2 modules)");
    r.begin(a, "discover");
    clock.advance(Duration::from_millis(100));
    r.done(a, Line::new().text("40 functions"));
    r.begin(b, "lift");
    for done in [10, 15, 20, 100] {
        clock.advance(Duration::from_secs(1));
        r.update(b, m(done, 100));
    }
    r.done(
        b,
        Line::new()
            .text("100 functions, ")
            .paint(Paint::Yellow, "3 unlifted"),
    );
    let want = "relift build: demo (2 modules)\n\
        \x20 nnrtld discover 40 functions  0.1 s\n\
        \x20 main   lift     10/100 functions (10%)  0 unlifted  1.0 s\n\
        \x20 main   lift     20/100 functions (20%)  0 unlifted  3.0 s\n\
        \x20 main   lift     100 functions, 3 unlifted  4.0 s\n";
    assert_eq!(text(&r), want);
}

#[test]
fn plain_mode_writes_a_line_when_ten_seconds_pass_with_no_step() {
    let (mut r, clock) = reporter(Mode::Plain, false, Verbosity::Normal);
    let c = r.row("");
    r.begin(c, "compile");
    r.update(c, m(1, 1000));
    clock.advance(Duration::from_secs(9));
    r.update(c, m(2, 1000));
    assert_eq!(text(&r), "");
    clock.advance(Duration::from_secs(1));
    r.update(c, m(3, 1000));
    assert_eq!(
        text(&r),
        "   compile  3/1,000 functions (0%)  0 unlifted  10.0 s\n"
    );
    clock.advance(Duration::from_secs(10));
    r.tick();
    assert!(text(&r).ends_with("3/1,000 functions (0%)  0 unlifted  20.0 s\n"));
}

#[test]
fn plain_output_has_no_escape_codes_unless_color_is_forced() {
    for (color, want) in [(false, false), (true, true)] {
        let (mut r, _c) = reporter(Mode::Plain, color, Verbosity::Normal);
        let a = r.row("m");
        r.begin(a, "lift");
        r.done(a, Line::new().paint(Paint::Yellow, "2 unlifted"));
        r.error("boom");
        assert_eq!(text(&r).contains('\x1b'), want, "color {color}");
    }
}

#[test]
fn quiet_prints_only_the_summary_and_errors() {
    let (mut r, clock) = reporter(Mode::Plain, false, Verbosity::Quiet);
    let a = r.row("m");
    r.title("t");
    r.begin(a, "lift");
    clock.advance(Duration::from_secs(30));
    r.update(a, m(50, 100));
    r.done(a, Line::new().text("x"));
    r.note(&Line::new().text("note"));
    r.detail(&Line::new().text("detail"));
    r.error("bad");
    r.summary(&Line::new().text("done"));
    assert_eq!(text(&r), "bad\ndone\n");
}

#[test]
fn verbose_adds_detail_lines() {
    let (mut r, _c) = reporter(Mode::Plain, false, Verbosity::Verbose);
    r.detail(&Line::new().text("shard0001.c"));
    let (mut n, _c) = reporter(Mode::Plain, false, Verbosity::Normal);
    n.detail(&Line::new().text("shard0001.c"));
    assert_eq!(text(&r), "shard0001.c\n");
    assert_eq!(text(&n), "");
}

#[test]
fn live_mode_redraws_in_place_and_collapses_a_finished_stage() {
    let (mut r, clock) = reporter(Mode::Live, false, Verbosity::Normal);
    let a = r.row("main");
    r.begin(a, "lift");
    let after_begin = text(&r);
    assert_eq!(after_begin, "  main lift     0\n");
    clock.advance(Duration::from_millis(200));
    r.update(a, m(5, 10));
    let all = text(&r);
    assert!(all.contains("\x1b[1A\r\x1b[J"), "{all:?}");
    assert!(
        all.ends_with("  main lift     [==========>         ] 5/10 functions  0 unlifted\n"),
        "{all:?}"
    );
    clock.advance(Duration::from_millis(10));
    r.update(a, m(6, 10));
    assert_eq!(text(&r), all, "a redraw within 80 ms is skipped");
    r.done(a, Line::new().text("10 functions"));
    assert!(
        text(&r).ends_with("\x1b[1A\r\x1b[J  main lift     10 functions  0.2 s\n"),
        "{:?}",
        text(&r)
    );
}

#[test]
fn live_lines_are_cut_to_the_width() {
    let clock = ManualClock::new();
    let mut r = Reporter::new(
        Vec::new(),
        Box::new(clock),
        Mode::Live,
        false,
        30,
        Verbosity::Normal,
    );
    let a = r.row("a-long-module-name");
    r.begin(a, "discover");
    r.update(a, m(1, 2));
    let last = text(&r).lines().last().unwrap_or_default().to_string();
    assert!(last.chars().count() <= 29, "{last:?}");
}

#[test]
fn byte_counts_and_errors_in_live_mode() {
    let (mut r, clock) = reporter(Mode::Live, false, Verbosity::Normal);
    let a = r.row("romfs");
    r.begin(a, "extract");
    clock.advance(Duration::from_millis(200));
    r.update(
        a,
        Metrics {
            done: 1_500_000,
            total: Some(3_000_000),
            bytes: true,
            note: Some("2/4 files".into()),
            ..Metrics::default()
        },
    );
    assert!(
        text(&r).ends_with("[==========>         ] 1.5 MB/3.0 MB  2/4 files\n"),
        "{:?}",
        text(&r)
    );
    r.error("relift import: boom");
    assert!(
        text(&r).ends_with("\x1b[1A\r\x1b[Jrelift import: boom\n"),
        "{:?}",
        text(&r)
    );
}
