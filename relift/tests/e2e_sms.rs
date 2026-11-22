//! End to end for the Master System guest on its development path: a
//! synthetic `.sms` cartridge goes through `relift import`, then `relift
//! build` with the code taken from an exec trace in the form of the ares
//! oracle's (`RELIFT_SMS_TRACE`), and the program runs headless. The ROM
//! turns on the frame interrupt, whose handler counts in work RAM and
//! writes a tone to the PSG; the run shows the count and a sound with a
//! peak. A build without a trace fails with the reason. Skipped with a note
//! when clang is not on `PATH`.

mod tiny;

/// Reset at 0: `di; im 1; ld sp, $dff0; ld a, $60; out ($bf), a;
/// ld a, $81; out ($bf), a; ei; halt; jr -3`; the handler at 0x38:
/// `push af; in a, ($bf); ld a, ($c000); inc a; ld ($c000), a;
/// ld a, $8e; out ($7f), a; ld a, $07; out ($7f), a; ld a, $90;
/// out ($7f), a; pop af; ei; reti`.
const CODE: &[(usize, &[u8])] = &[
    (
        0x0000,
        &[
            0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF, 0x3E, 0x60, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF,
            0xFB, 0x76, 0x18, 0xFD,
        ],
    ),
    (
        0x0038,
        &[
            0xF5, 0xDB, 0xBF, 0x3A, 0x00, 0xC0, 0x3C, 0x32, 0x00, 0xC0, 0x3E, 0x8E, 0xD3, 0x7F,
            0x3E, 0x07, 0xD3, 0x7F, 0x3E, 0x90, 0xD3, 0x7F, 0xF1, 0xFB, 0xED, 0x4D,
        ],
    ),
];

fn rom() -> Vec<u8> {
    let mut rom = vec![0u8; 0x8000];
    for (at, bytes) in CODE {
        rom[*at..*at + bytes.len()].copy_from_slice(bytes);
    }
    rom[0x7FF0..0x7FF8].copy_from_slice(b"TMR SEGA");
    rom[0x7FFF] = 0x4C;
    rom
}

/// An exec trace of every instruction of [`CODE`].
fn trace() -> String {
    let mut t = String::from("cpu\tpc\tregion\toffset\tfirst_frame\tcount\tbytes\n");
    let rom = rom();
    for (at, bytes) in CODE {
        let mut off = 0;
        while off < bytes.len() {
            let a = at + off;
            let w = &rom[a..a + 4];
            let n = relift_sms::code::length(w, a as u16).expect("decodes");
            t.push_str(&format!(
                "z80\t{a:06x}\trom\t{a:x}\t0\t1\t{:02x}{:02x}{:02x}{:02x}\n",
                w[0], w[1], w[2], w[3]
            ));
            off += usize::from(n);
        }
    }
    t
}

#[test]
fn a_trace_builds_and_the_program_runs_with_sound() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-sms");
    let project = root.join("sample");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("tone.sms");
    relift_cache::write_if_changed(&image, &rom()).expect("the ROM is writable");
    let exec = root.join("exec.tsv");
    relift_cache::write_if_changed(&exec, trace().as_bytes()).expect("the trace is writable");
    let cache = root.join("cache").display().to_string();
    let image = image.display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", &image]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let toml = std::fs::read_to_string(project.join("game.toml")).expect("game.toml");
    assert!(toml.contains("guest = \"sms\""), "{toml}");
    let out = tiny::relift(&project, &["--cache-dir", &cache, "build"]);
    assert!(!out.status.success());
    assert!(
        String::from_utf8_lossy(&out.stderr).contains("RELIFT_SMS_TRACE"),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let exec = exec.display().to_string();
    let out = tiny::relift_env(
        &project,
        &["--cache-dir", &cache, "-j", "4", "build"],
        &[("RELIFT_SMS_TRACE", &exec)],
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(stderr.contains("0 instructions not lifted"), "{stderr}");
    let host = root.join("cache/projects/sample/build/host");
    let ram = root.join("ram.bin").display().to_string();
    let out = relift_proc::command(host.join("sample"))
        .args(["--headless", "--frames", "30", "--dump-ram", &ram])
        .env("RELIFT_REPORT_DIR", &root)
        .output()
        .expect("the program runs");
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success(),
        "{stdout}\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(stdout.starts_with("frames 30 "), "{stdout}");
    let audio = stdout
        .lines()
        .find(|l| l.starts_with("audio frames "))
        .expect("the sound line");
    let peak: i32 = audio
        .split_whitespace()
        .nth(4)
        .and_then(|p| p.parse().ok())
        .expect("a peak");
    assert!(peak > 1000, "{audio}");
    let ram = std::fs::read(&ram).expect("the RAM dump");
    let irqs = ram[0].wrapping_sub(0xAB);
    assert!((28..=30).contains(&irqs), "{irqs} frame interrupts");
    let _ = std::fs::remove_dir_all(&root);
}
