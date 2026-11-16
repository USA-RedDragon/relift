//! End to end for the Nintendo 64 guest: a synthetic ROM whose boot code
//! copies a routine from the cartridge to RDRAM with a PI DMA and calls it
//! there goes through `relift import`, `relift build` (analysis, the
//! per-segment lift, the C emitter, clang and the runtime) and runs
//! headless. The routine is bound by its hash when the DMA brings it in, so
//! the run shows its store with no missed code. A second ROM loads a
//! microcode routine into IMEM with an SP DMA and starts the RSP, whose
//! lifted code stores a word in DMEM and breaks. A third draws with the
//! RDP and shows it through VI, both through parallel-rdp, and checks the
//! picture against the one ares scans out (skipped with a note without a
//! Vulkan device, unless `RELIFT_VK_REQUIRE` is set). Skipped with a note
//! when clang is not on `PATH`.

mod tiny;

use relift_n64_formats::synth;

const T0: u32 = 8;
const T1: u32 = 9;
const T2: u32 = 10;
const T4: u32 = 12;
const T5: u32 = 13;
const T6: u32 = 14;
const RA: u32 = 31;

fn i(op: u32, rs: u32, rt: u32, imm: u16) -> u32 {
    op << 26 | rs << 21 | rt << 16 | u32::from(imm)
}
fn lui(rt: u32, v: u16) -> u32 {
    i(0x0F, 0, rt, v)
}
fn ori(rt: u32, rs: u32, v: u16) -> u32 {
    i(0x0D, rs, rt, v)
}
fn andi(rt: u32, rs: u32, v: u16) -> u32 {
    i(0x0C, rs, rt, v)
}
fn sw(rt: u32, off: u16, base: u32) -> u32 {
    i(0x2B, base, rt, off)
}
fn lw(rt: u32, off: u16, base: u32) -> u32 {
    i(0x23, base, rt, off)
}
fn bne(rs: u32, rt: u32, off: i16) -> u32 {
    i(0x05, rs, rt, off as u16)
}
fn beq(rs: u32, rt: u32, off: i16) -> u32 {
    i(0x04, rs, rt, off as u16)
}
fn jal(t: u32) -> u32 {
    3 << 26 | (t >> 2 & 0x03FF_FFFF)
}
const NOP: u32 = 0;

/// The boot code at `0x80001000` (a PI DMA of 32 bytes from cartridge
/// `0x2000` to RDRAM `0x200000`, past the boot's MiB, a wait for it, a call there, a loop) and
/// the routine at ROM `0x2000` that stores `0x42` at `0x80300000`.
fn rom() -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec {
        entry: 0x8000_1000,
        ..synth::Spec::default()
    });
    let boot = [
        lui(T0, 0xA460),
        lui(T1, 0x0020),
        sw(T1, 0, T0),
        lui(T1, 0x1000),
        ori(T1, T1, 0x2000),
        sw(T1, 4, T0),
        ori(T1, 0, 0x1F),
        sw(T1, 0xC, T0),
        lw(T2, 0x10, T0),
        andi(T2, T2, 1),
        bne(T2, 0, -3),
        NOP,
        jal(0x8020_0000),
        NOP,
        bne(0, T0, -1),
        NOP,
    ];
    let routine = [
        lui(T4, 0x8030),
        ori(T5, 0, 0x42),
        sw(T5, 0, T4),
        RA << 21 | 8,
        NOP,
    ];
    for (k, w) in boot.iter().enumerate() {
        rom[0x1000 + 4 * k..0x1004 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    rom[0x1040..0x2000].fill(0);
    for (k, w) in routine.iter().enumerate() {
        rom[0x2000 + 4 * k..0x2004 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    rom[0x2014..0x3000].fill(0);
    rom
}

/// Imports and builds `rom` with `env` set for the build; the scratch root
/// and the directory of the built game, or `None` without clang.
fn build(
    name: &str,
    rom: &[u8],
    env: &[(&str, &str)],
) -> Option<(std::path::PathBuf, std::path::PathBuf)> {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return None;
    }
    let root = tiny::scratch(name);
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = root.join("rom.z64");
    relift_cache::write_if_changed(&image, rom).expect("the ROM is writable");
    let cache = root.join("cache").display().to_string();
    let image = image.display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", &image]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let toml = std::fs::read_to_string(project.join("game.toml")).expect("game.toml");
    assert!(toml.contains("guest = \"n64\""), "{toml}");
    let out = tiny::relift_env(&project, &["--cache-dir", &cache, "-j", "4", "build"], env);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(stderr.contains("0 instructions not lifted"), "{stderr}");
    let host = root.join("cache/projects/game/build/host");
    Some((root, host))
}

/// Imports, builds and runs `rom` headless for 3 frames with `--stats` and
/// `--dump`; the scratch root and the run's stderr, or `None` without clang.
fn build_and_run(name: &str, rom: &[u8]) -> Option<(std::path::PathBuf, String)> {
    let (root, host) = build(name, rom, &[])?;
    let dump = root.join("dump").display().to_string();
    let out = relift_proc::command(host.join("game"))
        .args(["--headless", "--frames", "3", "--stats", "--dump", &dump])
        .output()
        .expect("the game runs");
    let stdout = String::from_utf8_lossy(&out.stdout);
    let err = String::from_utf8_lossy(&out.stderr).into_owned();
    assert!(out.status.success(), "{stdout}{err}");
    Some((root, format!("{stdout}{err}")))
}

/// Runs the built game headless for 3 frames with the call trace asked for
/// at `trace`; whether the run succeeded.
fn run_traced(host: &std::path::Path, trace: &std::path::Path) -> bool {
    relift_proc::command(host.join("game"))
        .args(["--headless", "--frames", "3"])
        .env("RELIFT_N64_TRACE", trace)
        .output()
        .expect("the game runs")
        .status
        .success()
}

#[test]
fn the_call_trace_exists_only_in_builds_that_ask_for_it() {
    let r = rom();
    let Some((root, host)) = build("e2e-n64-trace", &r, &[("RELIFT_N64_TRACE", "1")]) else {
        return;
    };
    let trace = root.join("calls.tsv");
    assert!(run_traced(&host, &trace));
    let text = std::fs::read_to_string(&trace).expect("the trace is written");
    assert!(text.starts_with("call\tframe\tcycle\tcount\tlast_frame\n"));
    assert!(text.lines().count() >= 2, "{text}");
    let Some((root, host)) = build("e2e-n64-no-trace", &r, &[]) else {
        return;
    };
    let trace = root.join("calls.tsv");
    assert!(run_traced(&host, &trace));
    assert!(!trace.exists(), "a default build writes no trace");
}

#[test]
fn code_loaded_by_pi_dma_builds_binds_and_runs() {
    let Some((root, err)) = build_and_run("e2e-n64-dma", &rom()) else {
        return;
    };
    assert!(
        err.contains("interpreted 0, pi dmas 1,")
            && err.contains("mismatches 0")
            && err.contains("misses 0"),
        "{err}"
    );
    let rdram = std::fs::read(root.join("dump.rdram")).expect("the RDRAM dump");
    assert_eq!(&rdram[0x20_0000..0x20_0004], &lui(T4, 0x8030).to_be_bytes());
    assert_eq!(
        &rdram[0x30_0000..0x30_0004],
        &0x42u32.to_be_bytes(),
        "the routine the DMA loaded ran"
    );
}

/// The boot code at `0x80001000` (an SP DMA of the 16-byte routine at
/// RDRAM `0x1800` to IMEM 0, a wait for it, the RSP's start, a wait for its
/// halt, then DMEM word 0 copied to `0x80300000`, a loop) and the routine,
/// which stores `0x1234` at DMEM 0 and breaks.
fn rsp_rom() -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec {
        entry: 0x8000_1000,
        ..synth::Spec::default()
    });
    let boot = [
        lui(T0, 0xA404),
        ori(T1, 0, 0x1000),
        sw(T1, 0, T0),
        ori(T1, 0, 0x1800),
        sw(T1, 4, T0),
        ori(T1, 0, 0xF),
        sw(T1, 8, T0),
        lw(T2, 0x18, T0),
        bne(T2, 0, -2),
        NOP,
        ori(T1, 0, 0x5),
        sw(T1, 0x10, T0),
        lw(T2, 0x10, T0),
        andi(T2, T2, 1),
        beq(T2, 0, -3),
        NOP,
        lui(T4, 0xA400),
        lw(T5, 0, T4),
        lui(T6, 0x8030),
        sw(T5, 0, T6),
        bne(0, T0, -1),
        NOP,
    ];
    let ucode = [ori(1, 0, 0x1234), sw(1, 0, 0), 0x0000_000D, NOP];
    for (k, w) in boot.iter().enumerate() {
        rom[0x1000 + 4 * k..0x1004 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    rom[0x1058..0x3000].fill(0);
    for (k, w) in ucode.iter().enumerate() {
        rom[0x1800 + 4 * k..0x1804 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    rom
}

#[test]
fn microcode_loaded_by_sp_dma_runs_lifted_on_the_rsp() {
    let Some((root, err)) = build_and_run("e2e-n64-rsp", &rsp_rom()) else {
        return;
    };
    assert!(
        err.contains("rsp interpreted 0,")
            && err.contains("breaks 1,")
            && err.contains("misses 0, dp")
            && err.contains("sp dmas 1,"),
        "{err}"
    );
    let dmem = std::fs::read(root.join("dump.dmem")).expect("the DMEM dump");
    assert_eq!(&dmem[0..4], &0x1234u32.to_be_bytes());
    let rdram = std::fs::read(root.join("dump.rdram")).expect("the RDRAM dump");
    assert_eq!(
        &rdram[0x30_0000..0x30_0004],
        &0x1234u32.to_be_bytes(),
        "the CPU read what the RSP stored"
    );
}

/// The display list of [`vi_rom`]: a 320 by 240 RGBA5551 color image at
/// `0x300000`, the scissor around it, fill mode, the whole image filled
/// dark blue, a red and then a green rectangle, a full sync.
const VI_DL: [u32; 20] = [
    0xFF10_013F,
    0x0030_0000,
    0xED00_0000,
    0x0050_03C0,
    0xEF30_0000,
    0x0000_0000,
    0xF700_0000,
    0x0843_0843,
    0xF64F_C3BC,
    0x0000_0000,
    0xF700_0000,
    0xF801_F801,
    0xF647_C35C,
    0x0008_0060,
    0xF700_0000,
    0x07C1_07C1,
    0xF61F_C1BC,
    0x0010_00C0,
    0xE900_0000,
    0x0000_0000,
];

/// The VI registers of [`vi_rom`], in order from VI_CONTROL: 16-bit color
/// with the console's filters, origin `0x300000`, NTSC timing.
const VI_REGS: [u32; 14] = [
    0x0000_3002,
    0x0030_0000,
    320,
    0x200,
    0,
    0x03E5_2239,
    0x20D,
    0xC15,
    0x0C15_0C15,
    0x006C_02EC,
    0x0025_01FF,
    0x000E_0204,
    0x200,
    0x400,
];

/// A ROM that draws with the RDP and shows it through VI: boot code at
/// `0x80001000` that stores [`VI_DL`] at `0x280000` through KSEG1, runs it
/// from RDRAM, sets [`VI_REGS`] and loops; and, in the boot code area, a
/// stand-in boot (a PI DMA of the first MiB to `0x1000` and a jump there)
/// for a reference emulator that starts the cartridge's own boot code.
fn vi_rom() -> Vec<u8> {
    let mut rom = synth::rom(&synth::Spec {
        entry: 0x8000_1000,
        ..synth::Spec::default()
    });
    let stand_in = [
        lui(T0, 0xA460),
        ori(T1, 0, 0x1000),
        sw(T1, 0, T0),
        lui(T1, 0x1000),
        ori(T1, T1, 0x1000),
        sw(T1, 4, T0),
        lui(T1, 0x000F),
        ori(T1, T1, 0xFFFF),
        sw(T1, 0xC, T0),
        lw(T2, 0x10, T0),
        andi(T2, T2, 1),
        bne(T2, 0, -3),
        NOP,
        lui(T4, 0x8000),
        ori(T4, T4, 0x1000),
        T4 << 21 | 8,
        NOP,
    ];
    let mut boot = vec![lui(T0, 0xA028)];
    for (k, w) in VI_DL.iter().enumerate() {
        let off = u16::try_from(4 * k).expect("fits");
        boot.extend([
            lui(T1, (w >> 16) as u16),
            ori(T1, T1, *w as u16),
            sw(T1, off, T0),
        ]);
    }
    let end = 0x28_0000 + 4 * u32::try_from(VI_DL.len()).expect("fits");
    boot.extend([
        lui(T0, 0xA410),
        ori(T1, 0, 1),
        sw(T1, 0xC, T0),
        lui(T1, 0x0028),
        sw(T1, 0, T0),
        lui(T1, (end >> 16) as u16),
        ori(T1, T1, end as u16),
        sw(T1, 4, T0),
        lui(T0, 0xA440),
    ]);
    for (k, w) in VI_REGS.iter().enumerate() {
        let off = u16::try_from(4 * k).expect("fits");
        boot.extend([
            lui(T1, (w >> 16) as u16),
            ori(T1, T1, *w as u16),
            sw(T1, off, T0),
        ]);
    }
    boot.extend([bne(0, T0, -1), NOP]);
    for (k, w) in stand_in.iter().enumerate() {
        rom[0x40 + 4 * k..0x44 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    let code = 0x1000 + 4 * boot.len();
    for (k, w) in boot.iter().enumerate() {
        rom[0x1000 + 4 * k..0x1004 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    rom[code..0x2000].fill(0);
    rom
}

/// The hash (FNV-1a over the RGB bytes, as `--frames` prints it) of the
/// 640 by 240 picture ares (commit
/// `0b928930af4cc59ed99e942996f0d3c815379388`, parallel-rdp with its default
/// options, the boot through the stand-in) scans out of [`vi_rom`] at frame
/// 60, when the picture has long been still.
const VI_ARES: &str = "640x240 hash 88f2675215d3ff3d";

#[test]
fn the_rdp_and_vi_draw_the_picture_ares_draws() {
    let Some((_root, host)) = build("e2e-n64-vi", &vi_rom(), &[]) else {
        return;
    };
    let out = relift_proc::command(host.join("game"))
        .args(["--headless", "--no-audio", "--frames", "40", "--stats"])
        .output()
        .expect("the game runs");
    let stdout = String::from_utf8_lossy(&out.stdout);
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stdout}{err}");
    if err.contains("the RDP draws nothing") {
        assert!(
            std::env::var_os("RELIFT_VK_REQUIRE").is_none(),
            "RELIFT_VK_REQUIRE is set but parallel-rdp did not start: {err}"
        );
        eprintln!("skipped: no Vulkan device for parallel-rdp ({err})");
        return;
    }
    assert!(err.contains("misses 0"), "{err}");
    assert!(stdout.contains(VI_ARES), "{stdout}{err}");
}
