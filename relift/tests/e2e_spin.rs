//! End to end: the program of `tiny/spin.s` spins on its main thread
//! (priority 44, core 0) until a priority 32 thread of the same core, woken
//! from a 10 ms sleep, stores the word. Under the strict scheduler the
//! woken thread gets the core only when the spinner reaches a preemption
//! point (docs/abi.md section 14.1); without one the program never ends
//! and the test fails at its 30 s limit.

use std::io::Read;
use std::process::Stdio;
use std::time::{Duration, Instant};

mod tiny;

#[test]
fn a_spin_wait_gives_its_core_to_a_woken_higher_priority_thread() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-spin");
    let project = tiny::dump_spin(&root);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let mut child = relift_proc::command(root.join("cache/projects/tiny/build/host/tiny"))
        .current_dir(&project)
        .env_remove("RELIFT_SCHED")
        .stdout(Stdio::piped())
        .spawn()
        .expect("the program runs");
    let mut pipe = child.stdout.take().expect("stdout is piped");
    let reader = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = pipe.read_to_string(&mut s);
        s
    });
    let start = Instant::now();
    let status = loop {
        if let Some(s) = child.try_wait().expect("the game can be waited for") {
            break Some(s);
        }
        if start.elapsed() > Duration::from_secs(30) {
            let _ = child.kill();
            let _ = child.wait();
            break None;
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    let stdout = reader.join().unwrap_or_default();
    let status = status.expect("the spinner gave its core up within 30 s");
    assert!(status.success(), "{status:?} {stdout}");
    assert_eq!(stdout, "spin done\n");
}

#[test]
fn the_spin_program_binary_is_what_its_source_assembles_to() {
    match tiny::spin_matches_source() {
        Some(same) => assert!(same, "run `sh relift/tests/tiny/spin.sh`"),
        None => eprintln!("skipped: no llvm-mc or llvm-objcopy on PATH"),
    }
}
