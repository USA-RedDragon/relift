//! End to end for the Game Boy guest: a synthetic cartridge whose code
//! jumps (`jp hl`) to an address it builds from the joypad register goes
//! through `relift import` and `relift build`, and the release build stops
//! at the jump with a stop report that names the site: a release build has
//! no CPU interpreter to run the unlifted code on. The `--dev` build runs
//! it on the device library's CPU. Skipped with a note when clang is not
//! on `PATH`.

mod tiny;

use relift_gb_formats::synth;

/// `ld a, $30; ldh ($00), a; ldh a, ($00); and $0F; ld h, $C0; ld l, a;
/// jp hl` at `0x150`: with no button down the joypad reads `$0F`, so the
/// jump at `0x15B` goes to `0xC00F` in work RAM, which holds no lifted code.
fn rom() -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec::default());
    let code = [
        0x3E, 0x30, 0xE0, 0x00, 0xF0, 0x00, 0xE6, 0x0F, 0x26, 0xC0, 0x6F, 0xE9, 0x18, 0xFE,
    ];
    rom[0x150..0x150 + code.len()].copy_from_slice(&code);
    synth::fix_checksums(&mut rom);
    rom
}

#[test]
fn a_release_build_stops_at_unlifted_code_with_a_report_naming_the_jump() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-gb-miss");
    let project = root.join("sample");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("miss.gb");
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
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let host = root.join("cache/projects/sample/build/host");
    let reports = root.join("reports");
    let out = relift_proc::command(host.join(format!("sample{}", std::env::consts::EXE_SUFFIX)))
        .args(["--headless", "--no-save", "--no-audio", "--frames", "5"])
        .env("RELIFT_REPORT_DIR", &reports)
        .output()
        .expect("the program runs");
    let err = String::from_utf8_lossy(&out.stderr);
    assert_eq!(out.status.code(), Some(70), "{err}");
    let files: Vec<_> = std::fs::read_dir(&reports)
        .expect("a report folder")
        .map(|e| e.unwrap().path())
        .collect();
    assert_eq!(files.len(), 1, "{files:?}");
    let report = std::fs::read_to_string(&files[0]).unwrap();
    for want in [
        "\nkind: missed code\nsummary: no lifted code at guest 0xc00f (RAM); no lifted code for the instruction in RAM (opcode 0x",
        "\nguest: gb\n",
        "\ntransfer: jump\ntarget: 0xc00f\n",
        "\ntarget where: WRAM\n",
        "\nsite: 0x15b rom0+0x15b\nsite function: ",
        "\nsite words 0x157: 0f 26 c0 6f e9 18 fe ",
        "\n  af 0f",
    ] {
        assert!(report.contains(want), "{want:?} in\n{report}");
    }
    let file = files[0].display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "report", &file]);
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(text.contains("=> 0x015b: e9"), "{text}");
    assert!(text.contains("lifted C at the site (c/"), "{text}");
    assert!(text.contains("added ram c00f 000 to "), "{text}");
    let out = tiny::relift(
        &project,
        &["--cache-dir", &cache, "-j", "4", "build", "--dev"],
    );
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = relift_proc::command(host.join(format!("sample{}", std::env::consts::EXE_SUFFIX)))
        .args(["--headless", "--no-save", "--no-audio", "--frames", "5"])
        .env("RELIFT_REPORT_DIR", root.join("reports-dev"))
        .output()
        .expect("the program runs");
    assert!(
        !root.join("reports-dev").exists(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
}

/// An MBC1 cartridge whose code at `0x150` maps bank 1 (`ld a, 1; ld
/// ($2000), a`) and calls `0x4000` there; bank 1 builds an address from the
/// joypad register and jumps to it (`jp hl` at `0x400B`), into work RAM.
fn banked_rom() -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec {
        cart_type: 0x01,
        rom_size: 0x01,
        ..synth::Spec::default()
    });
    let fixed = [0x3E, 0x01, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0x18, 0xFE];
    rom[0x150..0x150 + fixed.len()].copy_from_slice(&fixed);
    let bank1 = [
        0x3E, 0x30, 0xE0, 0x00, 0xF0, 0x00, 0xE6, 0x0F, 0x26, 0xC0, 0x6F, 0xE9,
    ];
    rom[0x4000..0x4000 + bank1.len()].copy_from_slice(&bank1);
    synth::fix_checksums(&mut rom);
    rom
}

#[test]
fn a_miss_from_a_switchable_bank_names_the_site_in_its_bank() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-gb-bank-miss");
    let project = root.join("sample");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("bank.gb");
    relift_cache::write_if_changed(&image, &banked_rom()).expect("the ROM is writable");
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
    let host = root.join("cache/projects/sample/build/host");
    let reports = root.join("reports");
    let out = relift_proc::command(host.join(format!("sample{}", std::env::consts::EXE_SUFFIX)))
        .args(["--headless", "--no-save", "--no-audio", "--frames", "5"])
        .env("RELIFT_REPORT_DIR", &reports)
        .output()
        .expect("the program runs");
    let err = String::from_utf8_lossy(&out.stderr);
    assert_eq!(out.status.code(), Some(70), "{err}");
    let file = std::fs::read_dir(&reports)
        .expect("a report folder")
        .next()
        .expect("a report")
        .unwrap()
        .path();
    let report = std::fs::read_to_string(&file).unwrap();
    for want in [
        "\nkind: missed code\n",
        "\ntransfer: jump\ntarget: 0xc00f\n",
        "\nsite: 0x400b bank001+0x400b\nsite function: 0x4001 bank001+0x4001 gb_b001_4001",
    ] {
        assert!(report.contains(want), "{want:?} in\n{report}");
    }
}

/// `ld a, $20; ldh ($00), a` selects the d-pad, then the code at `0x154`
/// reads the joypad until a direction is down (`and $0F; cp $0F; jr z`)
/// and jumps (`jp hl` at `0x15F`) to `0xC000` plus the low nibble: with
/// right down, `0xC00E`.
fn pad_rom() -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec::default());
    let code = [
        0x3E, 0x20, 0xE0, 0x00, 0xF0, 0x00, 0xE6, 0x0F, 0xFE, 0x0F, 0x28, 0xF8, 0x26, 0xC0, 0x6F,
        0xE9, 0x18, 0xFE,
    ];
    rom[0x150..0x150 + code.len()].copy_from_slice(&code);
    synth::fix_checksums(&mut rom);
    rom
}

#[test]
fn an_input_script_holds_a_button_in_a_headless_run() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-gb-script");
    let project = root.join("sample");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("pad.gb");
    relift_cache::write_if_changed(&image, &pad_rom()).expect("the ROM is writable");
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
    let program = root
        .join("cache/projects/sample/build/host")
        .join(format!("sample{}", std::env::consts::EXE_SUFFIX));
    let out = relift_proc::command(&program)
        .args(["--headless", "--no-save", "--no-audio", "--frames", "20"])
        .env("RELIFT_REPORT_DIR", root.join("reports-none"))
        .output()
        .expect("the program runs");
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let reports = root.join("reports");
    let out = relift_proc::command(&program)
        .args(["--headless", "--no-save", "--no-audio", "--frames", "20"])
        .env("RELIFT_REPORT_DIR", &reports)
        .env("RELIFT_INPUT_SCRIPT", "5+100:p1.right")
        .output()
        .expect("the program runs");
    let err = String::from_utf8_lossy(&out.stderr);
    assert_eq!(out.status.code(), Some(70), "{err}");
    let files: Vec<_> = std::fs::read_dir(&reports)
        .expect("a report folder")
        .map(|e| e.unwrap().path())
        .collect();
    assert_eq!(files.len(), 1, "{files:?}");
    let report = std::fs::read_to_string(&files[0]).unwrap();
    assert!(report.contains("\ntarget: 0xc00e\n"), "{report}");
    let _ = std::fs::remove_dir_all(&root);
}
