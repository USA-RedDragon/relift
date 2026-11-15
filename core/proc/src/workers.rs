//! The one place that decides how many worker threads a stage may use.
//!
//! The `jobs` setting is resolved once by [`set_jobs`]; every parallel stage
//! of a build (analysis, emit, prediction, hashing, the compile) reads it
//! through [`jobs`] instead of asking the machine for its CPU count, so
//! `-j 2` bounds all of them, and many builds side by side stay inside a
//! task limit. A thread that cannot start is not an error: the workers that
//! did start, or the calling thread, do the work.

use std::sync::atomic::{AtomicUsize, Ordering};

static JOBS: AtomicUsize = AtomicUsize::new(0);

/// The CPU count, 1 when unknown.
fn cpus() -> usize {
    std::thread::available_parallelism().map_or(1, std::num::NonZero::get)
}

/// Fixes the worker count for this process: `jobs`, or one per CPU for 0.
pub fn set_jobs(jobs: u32) {
    JOBS.store(
        usize::try_from(jobs).unwrap_or(usize::MAX),
        Ordering::Relaxed,
    );
}

/// The worker count for a stage: what [`set_jobs`] set, else one per CPU.
pub fn jobs() -> usize {
    match JOBS.load(Ordering::Relaxed) {
        0 => cpus(),
        n => n,
    }
}

/// Runs `f` on the calling thread and on up to `n - 1` more scoped threads
/// (fewer when the system refuses to start them), and gives what each
/// returned. `f` must pull its work from shared state, so any number of
/// copies finishes the job.
pub fn run<R: Send>(n: usize, f: impl Fn() -> R + Sync) -> Vec<R> {
    let f = &f;
    std::thread::scope(|s| {
        let mut started = Vec::new();
        for _ in 1..n.max(1) {
            match std::thread::Builder::new().spawn_scoped(s, f) {
                Ok(h) => started.push(h),
                Err(_) => break,
            }
        }
        let mut out = vec![f()];
        for h in started {
            out.push(h.join().unwrap_or_else(|e| std::panic::resume_unwind(e)));
        }
        out
    })
}

/// `f` of every item of `items`, in order, on up to `n` threads.
pub fn parallel_with<T: Sync, R: Send>(
    n: usize,
    items: &[T],
    f: impl Fn(&T) -> R + Sync,
) -> Vec<R> {
    let n = n.min(items.len());
    if n <= 1 {
        return items.iter().map(f).collect();
    }
    let next = AtomicUsize::new(0);
    let mut out: Vec<(usize, R)> = run(n, || {
        let mut mine = Vec::new();
        loop {
            let i = next.fetch_add(1, Ordering::Relaxed);
            let Some(item) = items.get(i) else { break };
            mine.push((i, f(item)));
        }
        mine
    })
    .into_iter()
    .flatten()
    .collect();
    out.sort_unstable_by_key(|r| r.0);
    out.into_iter().map(|r| r.1).collect()
}

/// [`parallel_with`] on [`jobs`] threads.
pub fn parallel<T: Sync, R: Send>(items: &[T], f: impl Fn(&T) -> R + Sync) -> Vec<R> {
    parallel_with(jobs(), items, f)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;
    use std::sync::Mutex;

    fn threads_used(n: usize) -> (Vec<u64>, usize) {
        let ids = Mutex::new(HashSet::new());
        let items: Vec<u64> = (0..200).collect();
        let out = parallel_with(n, &items, |&x| {
            ids.lock()
                .expect("lock")
                .insert(std::thread::current().id());
            std::thread::sleep(std::time::Duration::from_micros(200));
            x * 3
        });
        (out, ids.into_inner().expect("lock").len())
    }

    #[test]
    fn same_output_for_any_worker_count() {
        let (one, _) = threads_used(1);
        let (eight, _) = threads_used(8);
        assert_eq!(one, eight);
        assert_eq!(one[199], 597);
    }

    #[test]
    fn never_more_threads_than_asked() {
        assert_eq!(threads_used(1).1, 1);
        assert!(threads_used(2).1 <= 2);
        assert!(threads_used(3).1 <= 3);
    }

    #[test]
    fn run_gives_one_result_per_started_worker() {
        let r = run(4, || 7);
        assert!(!r.is_empty() && r.len() <= 4);
        assert_eq!(run(0, || 1).len(), 1);
    }

    #[test]
    fn jobs_follows_the_setting() {
        set_jobs(3);
        assert_eq!(jobs(), 3);
        set_jobs(0);
        assert!(jobs() >= 1);
    }
}
