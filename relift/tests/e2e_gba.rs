//! End to end for the GBA guest: a synthetic cartridge whose ROM code copies
//! an ARM routine into IWRAM with the BIOS `CpuSet` and calls it there goes
//! through `relift import`, `relift build` (analysis, the C emitter, clang
//! and the runtime) and runs headless. Code in RAM is lifted with guards that
//! leave for another variant when the code there changes, so the build
//! compiles those exits and the run shows the RAM routine's store. Skipped
//! with a note when clang is not on `PATH`.

mod tiny;

use relift_gba_formats::synth;

/// The ROM code at `0xC0` (`CpuSet` of the four words at `0x08000100` to
/// `0x03000000`, then `mov lr, pc; bx` there and `b .`), its literal pool,
/// and the routine at `0x100` that stores `0x42` at `0x02000000` and returns.
fn rom() -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec::default());
    let code: [u32; 10] = [
        0xE59F_0018,
        0xE3A0_1403,
        0xE59F_2014,
        0xEF0B_0000,
        0xE3A0_1403,
        0xE1A0_E00F,
        0xE12F_FF11,
        0xEAFF_FFFE,
        0x0800_0100,
        0x0400_0004,
    ];
    let ram: [u32; 4] = [0xE3A0_0402, 0xE3A0_1042, 0xE580_1000, 0xE12F_FF1E];
    for (i, w) in code.iter().enumerate() {
        rom[0xC0 + 4 * i..0xC4 + 4 * i].copy_from_slice(&w.to_le_bytes());
    }
    for (i, w) in ram.iter().enumerate() {
        rom[0x100 + 4 * i..0x104 + 4 * i].copy_from_slice(&w.to_le_bytes());
    }
    synth::fix_complement(&mut rom);
    rom
}

#[test]
fn code_copied_to_iwram_builds_and_runs() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-gba-iwram");
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("ram.gba");
    relift_cache::write_if_changed(&image, &rom()).expect("the ROM is writable");
    let cache = root.join("cache").display().to_string();
    let image = image.display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", &image]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(
        stderr.contains("built 2 functions, 0 instructions not lifted"),
        "{stderr}"
    );
    let host = root.join("cache/projects/game/build/host");
    let shard = std::fs::read_to_string(host.join("c/rom/shard0000.c")).expect("the ROM shard");
    assert!(shard.contains("rl_gba_cpu_variant(c"), "{shard}");
    let dump = root.join("dump").display().to_string();
    let out = relift_proc::command(host.join("game"))
        .args([
            "--headless",
            "--no-save",
            "--frames",
            "5",
            "--stats",
            "--dump",
            &dump,
        ])
        .output()
        .expect("the game runs");
    let stdout = String::from_utf8_lossy(&out.stdout);
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stdout}{err}");
    assert!(
        err.contains("swis 1 (0 unknown)") && err.contains("misses 0"),
        "{stdout}{err}"
    );
    let ewram = std::fs::read(root.join("dump.ewram")).expect("the EWRAM dump");
    assert_eq!(&ewram[..4], &0x42u32.to_le_bytes(), "the IWRAM routine ran");
}

/// A cartridge whose ROM code jumps to an address it computes from
/// KEYINPUT (`ldrh` of `0x04000130`, masked to `0x100`, plus the ROM base),
/// which no analysis can follow: the jump at `0x080000D0` reaches
/// `0x08000100`, which nothing else names.
fn missing_rom() -> Vec<u8> {
    missing_rom_at(0x08)
}

/// As [`missing_rom`], with the jump into the ROM image at `top << 24`
/// (`0x08`, or a mirror: `0x0A`, `0x0C`).
fn missing_rom_at(top: u32) -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec::default());
    let code: [u32; 7] = [
        0xE59F_0010,
        0xE1D0_10B0,
        0xE201_1C01,
        0xE281_1400 | top,
        0xE12F_FF11,
        0xEAFF_FFFE,
        0x0400_0130,
    ];
    for (i, w) in code.iter().enumerate() {
        rom[0xC0 + 4 * i..0xC4 + 4 * i].copy_from_slice(&w.to_le_bytes());
    }
    rom[0x100..0x104].copy_from_slice(&0xE3A0_0001u32.to_le_bytes());
    rom[0x104..0x108].copy_from_slice(&0xEAFF_FFFEu32.to_le_bytes());
    synth::fix_complement(&mut rom);
    rom
}

#[test]
fn a_jump_no_analysis_follows_stops_with_a_report_naming_its_site() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-gba-miss");
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("miss.gba");
    relift_cache::write_if_changed(&image, &missing_rom()).expect("the ROM is writable");
    let cache = root.join("cache").display().to_string();
    let image = image.display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", &image]);
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
    let host = root.join("cache/projects/game/build/host");
    let reports = root.join("reports");
    let out = relift_proc::command(host.join("game"))
        .args(["--headless", "--no-save", "--frames", "5"])
        .env("RELIFT_REPORT_DIR", &reports)
        .output()
        .expect("the game runs");
    let err = String::from_utf8_lossy(&out.stderr);
    assert_eq!(out.status.code(), Some(70), "{err}");
    let files: Vec<_> = std::fs::read_dir(&reports)
        .expect("a report folder")
        .map(|e| e.unwrap().path())
        .collect();
    assert_eq!(files.len(), 1, "{files:?}");
    let report = std::fs::read_to_string(&files[0]).unwrap();
    for want in [
        "\nkind: missed code\n",
        "\nguest: gba\n",
        "\ntransfer: jump\ntarget: 0x8000100 rom+0x100\n",
        "\ntarget where: cartridge ROM (wait state 0)\n",
        "\ntarget words 0x8000100: e3a00001 eafffffe ",
        "\nsite: 0x80000d0 rom+0xd0\nsite function: ",
        "  r1  08000100  ",
    ] {
        assert!(report.contains(want), "{want:?} in\n{report}");
    }
    let file = files[0].display().to_string();
    let out = tiny::relift(
        &project,
        &["--cache-dir", &cache, "report", "--no-hint", &file],
    );
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(text.contains("lifted C at the site (c/"), "{text}");
    assert!(text.contains("RL_JUMP("), "{text}");
    if relift_build::find_tool("llvm-mc").is_some() {
        assert!(text.contains("=> 0x80000d0: bx\tr1\n"), "{text}");
    }
}

#[test]
fn a_miss_into_a_rom_mirror_is_named_by_its_rom_offset() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-gba-mirror");
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("mirror.gba");
    relift_cache::write_if_changed(&image, &missing_rom_at(0x0A)).expect("the ROM is writable");
    let cache = root.join("cache").display().to_string();
    let image = image.display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", &image]);
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
    let reports = root.join("reports");
    let out = relift_proc::command(root.join("cache/projects/game/build/host/game"))
        .args(["--headless", "--no-save", "--frames", "5"])
        .env("RELIFT_REPORT_DIR", &reports)
        .output()
        .expect("the game runs");
    assert_eq!(
        out.status.code(),
        Some(70),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let file = std::fs::read_dir(&reports)
        .expect("a report folder")
        .next()
        .expect("a report")
        .unwrap()
        .path();
    let report = std::fs::read_to_string(&file).unwrap();
    for want in [
        "\ntarget: 0xa000100 rom+0x100\n",
        "\ntarget where: cartridge ROM (wait state 1)\n",
    ] {
        assert!(report.contains(want), "{want:?} in\n{report}");
    }
}
