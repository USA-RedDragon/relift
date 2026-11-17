//! End to end for the SG-1000 on the development path: a generated test
//! cartridge goes through `relift import` and `relift build` (code from an
//! exec trace, `RELIFT_SMS_TRACE`), and the program runs headless. The ROM
//! loads the TMS9918A's registers, fills VRAM with 16 KiB of pseudo-random
//! bytes, and on every frame interrupt counts, reads the pad, moves a
//! sprite and changes a PSG tone; every 64 frames it switches between
//! graphics 1, graphics 2, text and multicolor. The CRCs of five frames,
//! one per mode, are those the ares SG-1000 core gives for the same ROM
//! (measured with the spike's oracle; the whole run of 600 frames, and its
//! instruction log, matched ares). Skipped with a note when clang is not on
//! `PATH`.

mod tiny;

/// The code: reset, the interrupt handler, the NMI return, the setup and
/// the main loop, each as (address, bytes).
const CODE: &[(usize, &str)] = &[
    (0x0000, "F3ED5631 00C4C3 0001"),
    (0x0038, "F5DBBF 3200C0 3A01C0 3C3201C0 DBDC3202C0 F1FBC9"),
    (0x0066, "ED45"),
    (
        0x0100,
        "210002 01BF10 EDB3 3E00D3BF 3E40D3BF 210010 0EBE 1640 0600EDB3 15 20F9 FB",
    ),
    (
        0x011F,
        "76 3A01C0 E63F 2018 3A03C0 3C E603 3203C0 6F 2600 292929 111002 19 01BF08 EDB3 \
         3E00D3BF 3E5BD3BF 3A01C0 D3BE 87 D3BE 3A01C0 E60F F680 D37F 3E05D37F 3E90D37F 18BD",
    ),
];

/// The register writes: the setup at 0x200, then one per mode at 0x210.
const TABLES: &[(usize, &str)] = &[
    (0x0200, "0080E0810E8280830084368503 86F487"),
    (0x0210, "0080E08180830084"),
    (0x0218, "0280E081FF830384"),
    (0x0220, "0080F08180830084"),
    (0x0228, "0080E88180830084"),
];

fn hex(s: &str) -> Vec<u8> {
    let s: String = s.chars().filter(char::is_ascii_hexdigit).collect();
    (0..s.len() / 2)
        .map(|k| u8::from_str_radix(&s[2 * k..2 * k + 2], 16).expect("hex"))
        .collect()
}

fn rom() -> Vec<u8> {
    let mut rom = vec![0u8; 0x8000];
    for (at, h) in CODE.iter().chain(TABLES) {
        let b = hex(h);
        rom[*at..*at + b.len()].copy_from_slice(&b);
    }
    let mut x: u32 = 0x1234_5678;
    for b in &mut rom[0x1000..0x5000] {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *b = x as u8;
    }
    rom
}

fn trace(rom: &[u8]) -> String {
    let mut t = String::from("cpu\tpc\tregion\toffset\tfirst_frame\tcount\tbytes\n");
    for (at, h) in CODE {
        let len = hex(h).len();
        let mut a = *at;
        while a < at + len {
            let w = &rom[a..a + 4];
            t.push_str(&format!(
                "z80\t{a:06x}\trom\t{a:x}\t0\t1\t{:02x}{:02x}{:02x}{:02x}\n",
                w[0], w[1], w[2], w[3]
            ));
            a += usize::from(relift_sms::code::length(w, a as u16).expect("decodes"));
        }
    }
    t
}

#[test]
fn the_sg1000_test_rom_matches_ares_in_every_mode() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let rom = rom();
    assert_eq!(rom[0x0161], 0xBD);
    let root = tiny::scratch("e2e-sg");
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("modes.sg");
    relift_cache::write_if_changed(&image, &rom).expect("the ROM is writable");
    let exec = root.join("exec.tsv");
    relift_cache::write_if_changed(&exec, trace(&rom).as_bytes()).expect("the trace is writable");
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(
        &project,
        &[
            "--cache-dir",
            &cache,
            "import",
            &image.display().to_string(),
        ],
    );
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let toml = std::fs::read_to_string(project.join("game.toml")).expect("game.toml");
    assert!(toml.contains("console = \"sg\""), "{toml}");
    let exec = exec.display().to_string();
    let out = tiny::relift_env(
        &project,
        &["--cache-dir", &cache, "-j", "4", "build"],
        &[("RELIFT_SMS_TRACE", &exec)],
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(stderr.contains("0 instructions not lifted"), "{stderr}");
    let log = root.join("frames.tsv");
    let out = relift_proc::command(root.join("cache/projects/game/build/host/game"))
        .args(["--headless", "--no-audio", "--frames", "300", "--frame-log"])
        .arg(&log)
        .env("RELIFT_REPORT_DIR", &root)
        .output()
        .expect("the program runs");
    assert!(
        out.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    let log = std::fs::read_to_string(&log).expect("the frame log");
    let crc = |frame: &str| {
        log.lines()
            .find(|l| l.split('\t').next() == Some(frame))
            .and_then(|l| l.split('\t').nth(1))
            .map(str::to_string)
    };
    for (frame, want) in [
        ("59", "507057ca"),
        ("119", "7dce0065"),
        ("179", "a31e0196"),
        ("239", "b21cc470"),
        ("299", "79f7fd54"),
    ] {
        assert_eq!(crc(frame).as_deref(), Some(want), "frame {frame}");
    }
    let _ = std::fs::remove_dir_all(&root);
}
