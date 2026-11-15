mod common;

use common::{
    JR_SELF, black_screen_then_marker, detail, drive, ldh_all, plain, rom, scratch, serial_sc_edge,
    then_spin, write_new,
};
use relift_gb_devices::{Rlgbd, Verdict, encode_rgb_png};

const STEPS: u64 = 40_000_000;

fn mooneye_code(b: u8) -> Vec<u8> {
    then_spin(&[
        0x06, b, 0x0E, 0x05, 0x16, 0x08, 0x1E, 0x0D, 0x26, 0x15, 0x2E, 0x22, 0x40,
    ])
}

fn ref_mooneye(m: &mut Rlgbd, marker: u8, cap: u64) -> (bool, u64) {
    let mut cycles = 0u64;
    while cycles < cap {
        let pc = m.regs().pc;
        if m.read_memory(pc) == marker {
            return (true, m.now());
        }
        cycles += u64::from(m.step_native());
    }
    (false, m.now())
}

fn ref_serial(
    m: &mut Rlgbd,
    frames: u64,
    pass: &str,
    fails: &[&str],
    sc_edge: bool,
) -> (bool, u64) {
    let budget = frames * 70224;
    let mut cycles = 0u64;
    let mut prev_start = false;
    let mut prev_sb = m.read_memory(0xFF01);
    let mut out: Vec<u8> = Vec::new();
    let min_len = fails
        .iter()
        .map(|f| f.len())
        .chain(std::iter::once(pass.len()))
        .min()
        .unwrap_or(1);
    while cycles < budget {
        let byte = if sc_edge {
            let sc = m.read_memory(0xFF02);
            let start = (sc & 0x80) != 0 && (sc & 0x01) != 0;
            let edge = start && !prev_start;
            prev_start = start;
            edge.then(|| m.read_memory(0xFF01))
        } else {
            let sb = m.read_memory(0xFF01);
            let changed = sb != prev_sb;
            prev_sb = sb;
            changed.then_some(sb)
        };
        if let Some(byte) = byte {
            out.push(byte);
            if out.len() >= min_len {
                let s = String::from_utf8_lossy(&out);
                if s.contains(pass) {
                    return (true, m.now());
                }
                if fails.iter().any(|f| s.contains(f)) {
                    return (false, m.now());
                }
            }
        }
        cycles += u64::from(m.step_native());
    }
    (String::from_utf8_lossy(&out).contains(pass), m.now())
}

#[test]
fn mooneye_passes_on_the_fibonacci_registers_at_the_marker() {
    let image = plain(0, &mooneye_code(3));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=mooneye\nmarker=0x40\n").unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Pass);
    assert_eq!(detail(&m), "ok");
    let mut r = Rlgbd::new(0, &image).unwrap();
    assert_eq!(ref_mooneye(&mut r, 0x40, 250_000_000), (true, m.now()));
}

#[test]
fn mooneye_fails_with_the_registers_in_the_detail() {
    let image = plain(0, &mooneye_code(4));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=mooneye\nmarker=0x40\n").unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Fail);
    assert_eq!(
        detail(&m),
        "regs B=04 C=05 D=08 E=0D H=15 L=22 (want 03 05 08 0D 15 22)"
    );
}

#[test]
fn mooneye_ed_stops_at_the_illegal_opcode_and_asks_for_registers() {
    let mut code = mooneye_code(3);
    code[12] = 0xED;
    let image = plain(0, &code);
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=mooneye_ed\n").unwrap();
    let mut verdict = Verdict::Continue;
    for _ in 0..STEPS {
        let pc = m.regs().pc;
        verdict = m.case_poll(pc);
        if verdict != Verdict::Continue {
            break;
        }
        if let Some(op) = m.begin() {
            m.exec_native(op);
        }
    }
    assert_eq!(verdict, Verdict::NeedRegs);
    let regs = m.regs();
    m.case_regs(&regs);
    assert_eq!(m.case_poll(regs.pc), Verdict::Pass);
}

#[test]
fn mooneye_times_out_without_a_marker() {
    let image = plain(0, &then_spin(&[]));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=mooneye\ncycles=200000\n").unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Fail);
    assert_eq!(detail(&m), "no done-marker (timeout)");
    let mut r = Rlgbd::new(0, &image).unwrap();
    assert_eq!(ref_mooneye(&mut r, 0x40, 200_000), (false, m.now()));
}

#[test]
fn serial_sc_edge_passes_on_the_text_at_the_same_dot_as_the_runner() {
    let image = plain(0, &then_spin(&serial_sc_edge("Test 01\nPassed\n")));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=serial\nframes=4\ncapture=sc_edge\npass_text=Passed\nfail_text=Failed\nfail_text=Error\n")
        .unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Pass);
    let mut r = Rlgbd::new(0, &image).unwrap();
    assert_eq!(
        ref_serial(&mut r, 4, "Passed", &["Failed", "Error"], true),
        (true, m.now())
    );
}

#[test]
fn serial_fails_early_on_a_failure_text() {
    let image = plain(0, &then_spin(&serial_sc_edge("01: Failed #3\n")));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=serial\nframes=4\n").unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Fail);
    assert_eq!(detail(&m), "serial verdict: 01: Failed");
    let mut r = Rlgbd::new(0, &image).unwrap();
    assert_eq!(
        ref_serial(&mut r, 4, "Passed", &["Failed", "Error"], true),
        (false, m.now())
    );
}

#[test]
fn serial_times_out_with_the_tail_of_the_text() {
    let image = plain(0, &then_spin(&serial_sc_edge("hello")));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=serial\nframes=2\n").unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Fail);
    assert_eq!(detail(&m), "no \"Passed\"; tail: hello");
    let mut silent = Rlgbd::new(0, &plain(0, &JR_SELF)).unwrap();
    silent.case_open("protocol=serial\nframes=2\n").unwrap();
    assert_eq!(drive(&mut silent, STEPS), Verdict::Fail);
    assert_eq!(detail(&silent), "no serial output (timeout)");
}

#[test]
fn serial_text_follows_sb_changes() {
    let image = plain(0, &then_spin(&ldh_all(0x01, b"Test OK!")));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=serial_text\nframes=2\ncapture=sb_change\npass_text=Test OK!\n")
        .unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Pass);
    let mut r = Rlgbd::new(0, &image).unwrap();
    assert_eq!(
        ref_serial(&mut r, 2, "Test OK!", &[], false),
        (true, m.now())
    );
}

#[test]
fn mem_grades_a_byte_after_the_whole_budget() {
    let image = plain(0, &then_spin(&ldh_all(0x82, &[1])));
    let mut m = Rlgbd::new(0, &image).unwrap();
    let start = m.now();
    m.case_open("protocol=mem\nframes=3\naddr=0xFF82\nexpect=0x01\n")
        .unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Pass);
    let elapsed = m.now() - start;
    assert!((3 * 70224..3 * 70224 + 32).contains(&elapsed), "{elapsed}");
    let mut bad = Rlgbd::new(0, &image).unwrap();
    bad.case_open("protocol=mem\nframes=3\naddr=0xFF82\nexpect=0x02\n")
        .unwrap();
    assert_eq!(drive(&mut bad, STEPS), Verdict::Fail);
    assert!(
        detail(&bad).starts_with("FF82=01 (want 02); FF80(actual)="),
        "{}",
        detail(&bad)
    );
}

#[test]
fn png_fixed_passes_against_a_reference_written_from_the_same_run() {
    let image = plain(0x80, &black_screen_then_marker());
    let spec = |extra: &str| format!("protocol=png\nframes=30\nmode=cgb\n{extra}");
    let reference = scratch("cgb-ref.png");
    write_new(
        &reference,
        &encode_rgb_png(160, 144, &[0x55; 160 * 144 * 3]),
    );
    let mut first = Rlgbd::new(1, &image).unwrap();
    first
        .case_open(&spec(&format!("ref={}\n", reference.display())))
        .unwrap();
    assert_eq!(drive(&mut first, STEPS), Verdict::Fail);
    assert!(detail(&first).starts_with("screen did not match PNG "));
    first.write_png(&reference).unwrap();
    let mut again = Rlgbd::new(1, &image).unwrap();
    again
        .case_open(&spec(&format!("ref={}\n", reference.display())))
        .unwrap();
    assert_eq!(drive(&mut again, STEPS), Verdict::Pass);
    assert_eq!(again.frame_hash(), first.frame_hash());
    let mut layout = Rlgbd::new(1, &image).unwrap();
    layout
        .case_open(&format!(
            "protocol=png_layout\nframes=30\nmode=cgb\nref={}\n",
            reference.display()
        ))
        .unwrap();
    assert_eq!(drive(&mut layout, STEPS), Verdict::Pass);
}

#[test]
fn png_on_a_dmg_grades_the_shades_not_the_palette() {
    let image = plain(0, &black_screen_then_marker());
    let black = scratch("black.png");
    let white = scratch("white.png");
    let gray = scratch("gray.png");
    write_new(&gray, &encode_rgb_png(160, 144, &[0x80; 160 * 144 * 3]));
    write_new(&black, &encode_rgb_png(160, 144, &[0; 160 * 144 * 3]));
    write_new(&white, &encode_rgb_png(160, 144, &[0xFF; 160 * 144 * 3]));
    let mut ok = Rlgbd::new(0, &image).unwrap();
    ok.case_open(&format!(
        "protocol=png\nframes=30\nref={}\n",
        black.display()
    ))
    .unwrap();
    assert_eq!(drive(&mut ok, STEPS), Verdict::Pass, "{}", detail(&ok));
    let mut bad = Rlgbd::new(0, &image).unwrap();
    bad.case_open(&format!(
        "protocol=png\nframes=30\nref={}\n",
        white.display()
    ))
    .unwrap();
    assert_eq!(drive(&mut bad, STEPS), Verdict::Fail);
    assert!(
        detail(&bad).contains(
            "23040 differing pixels; first mismatch at (0, 0): actual #000000, expected #FFFFFF"
        ),
        "{}",
        detail(&bad)
    );
    let mut shoot = Rlgbd::new(0, &image).unwrap();
    shoot
        .case_open(&format!(
            "protocol=png_shootout\nframes=10\nref={}\nref={}\n",
            gray.display(),
            black.display()
        ))
        .unwrap();
    assert_eq!(drive(&mut shoot, STEPS), Verdict::Pass);
    let mut shoot_bad = Rlgbd::new(0, &image).unwrap();
    shoot_bad
        .case_open(&format!(
            "protocol=png_shootout\nframes=10\nref={}\n",
            gray.display()
        ))
        .unwrap();
    assert_eq!(drive(&mut shoot_bad, STEPS), Verdict::Fail);
    assert!(detail(&shoot_bad).starts_with("screen did not match shootout PNG "));
    let mut fixed = Rlgbd::new(0, &image).unwrap();
    fixed
        .case_open(&format!(
            "protocol=png_fixed\nframes=4\nref={}\n",
            black.display()
        ))
        .unwrap();
    assert_eq!(drive(&mut fixed, STEPS), Verdict::Pass);
}

#[test]
fn a_missing_reference_fails_with_the_runner_message() {
    let image = plain(0, &black_screen_then_marker());
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=png\nframes=30\nref=/nonexistent/ref.png\n")
        .unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Fail);
    assert!(
        detail(&m).starts_with("reference PNG /nonexistent/ref.png: failed to read PNG: "),
        "{}",
        detail(&m)
    );
}

#[test]
fn gambatte_hex_reads_the_tiles_after_the_last_frame() {
    let mut code = vec![
        0xF0, 0x44, 0xFE, 0x90, 0x38, 0xFA, 0x21, 0x00, 0x80, 0x0E, 0x10, 0x3E, 0xFF,
    ];
    code.extend_from_slice(&[0x22, 0x0D, 0x20, 0xFA]);
    let image = plain(0, &then_spin(&code));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=gambatte_hex\nframes=4\nexpect_hex=0\n")
        .unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Fail);
    assert!(
        detail(&m).starts_with(
            "screen did not match hex output 0: hex output 0 mismatch at tile 0 (0); "
        ),
        "{}",
        detail(&m)
    );
    let mut off = Rlgbd::new(0, &plain(0, &then_spin(&[0xAF, 0xE0, 0x40]))).unwrap();
    off.case_open("protocol=gambatte_hex\nframes=2\nexpect_hex=0\n")
        .unwrap();
    assert_eq!(drive(&mut off, STEPS), Verdict::Fail);
    assert!(
        detail(&off).starts_with("timed out waiting for LCD frame while running frame "),
        "{}",
        detail(&off)
    );
}

#[test]
fn audio_is_not_supported_on_the_ext_path() {
    let mut m = Rlgbd::new(0, &plain(0, &JR_SELF)).unwrap();
    m.case_open("protocol=gambatte_audio\naudible=true\n")
        .unwrap();
    assert_eq!(drive(&mut m, 10), Verdict::Unsupported);
    assert_eq!(detail(&m), "the external-CPU path generates no audio");
}

#[test]
fn run_reports_the_frame_hash_and_the_serial_text() {
    let image = plain(0, &then_spin(&serial_sc_edge("Hi\n")));
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=run\nframes=3\ncapture=sc_edge\n")
        .unwrap();
    assert_eq!(drive(&mut m, STEPS), Verdict::Pass);
    assert_eq!(
        detail(&m),
        format!("frame {:016x} serial Hi\\n", m.frame_hash())
    );
    assert!(m.frame_poll() >= 3);
}

#[test]
fn region_dump_compares_oam_and_skips_ranges_for_sram() {
    let oam: Vec<u8> = (1..=160).map(|v| v as u8).collect();
    let mut code = vec![
        0xAF, 0xE0, 0x40, 0x21, 0x00, 0xFE, 0x3E, 0x01, 0x22, 0x3C, 0xFE, 0xA1, 0x20, 0xFA,
    ];
    code.extend_from_slice(&JR_SELF);
    let image = plain(0, &code);
    let want = scratch("oam.dump");
    write_new(&want, &oam);
    let mut ok = Rlgbd::new(0, &image).unwrap();
    ok.case_open(&format!(
        "protocol=region_dump\nframes=1\nregion=oam\nref={}\n",
        want.display()
    ))
    .unwrap();
    assert_eq!(drive(&mut ok, STEPS), Verdict::Pass, "{}", detail(&ok));
    let mut wrong = oam.clone();
    wrong[7] = 0;
    write_new(&want, &wrong);
    let mut bad = Rlgbd::new(0, &image).unwrap();
    bad.case_open(&format!(
        "protocol=region_dump\nframes=1\nregion=oam\nref={}\n",
        want.display()
    ))
    .unwrap();
    assert_eq!(drive(&mut bad, STEPS), Verdict::Fail);
    assert_eq!(
        detail(&bad),
        format!(
            "region dump {} (Oam @ 0xFE00): first mismatch at offset 0x0007: expected 0x00, got 0x08",
            want.display()
        )
    );
}

#[test]
fn sram_dump_compares_cartridge_ram_outside_the_skip_ranges() {
    let mut code = vec![0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x77, 0xEA, 0x00, 0xA0];
    code.extend_from_slice(&JR_SELF);
    let image = rom(0x1B, 0, 2, 0, &code);
    let mut probe = Rlgbd::new(0, &image).unwrap();
    probe
        .case_open("protocol=region_dump\nregion=vram\nref=/x\n")
        .unwrap();
    for _ in 0..100 {
        if let Some(op) = probe.begin() {
            probe.exec_native(op);
        }
    }
    let mut want = probe.save_bytes();
    assert_eq!(want.len(), 8192);
    assert_eq!(want[0], 0x77);
    let dump = scratch("sram.bin");
    write_new(&dump, &want);
    let run = |extra: &str| {
        let mut m = Rlgbd::new(0, &image).unwrap();
        m.case_open(&format!(
            "protocol=sram\nframes=1\nref={}\n{extra}",
            dump.display()
        ))
        .unwrap();
        let v = drive(&mut m, STEPS);
        (v, detail(&m))
    };
    assert_eq!(run("").0, Verdict::Pass);
    want[10] ^= 0xFF;
    write_new(&dump, &want);
    let (v, d) = run("");
    assert_eq!(v, Verdict::Fail);
    assert!(d.contains("first mismatch at offset 0x000A"), "{d}");
    assert_eq!(run("skip=8-12\n").0, Verdict::Pass);
}

#[test]
fn scripted_input_presses_buttons_at_the_frame_windows() {
    let image = plain(
        0,
        &[0x3E, 0x10, 0xE0, 0x00, 0xF0, 0x00, 0xE0, 0x80, 0x18, 0xF6],
    );
    let run = |frames: u32| {
        let mut m = Rlgbd::new(0, &image).unwrap();
        m.case_open(&format!(
            "protocol=png_fixed\nframes={frames}\nref=/nonexistent.png\ninput=1:-,2:a+start\n"
        ))
        .unwrap();
        assert_eq!(drive(&mut m, STEPS), Verdict::Fail);
        m.read_memory(0xFF80) & 0x0F
    };
    assert_eq!(run(2), 0x0F);
    assert_eq!(run(4), 0x06);
}

#[test]
fn a_stopped_cpu_still_spends_the_budget() {
    let image = plain(0, &[0x3E, 0x30, 0xE0, 0x00, 0x10, 0x00, 0x18, 0xFE]);
    let mut m = Rlgbd::new(0, &image).unwrap();
    m.case_open("protocol=mem\nframes=2\naddr=0xFF82\nexpect=0\n")
        .unwrap();
    assert_ne!(drive(&mut m, 100_000), Verdict::Continue);
}

#[test]
fn spec_errors_are_reported() {
    let mut m = Rlgbd::new(0, &plain(0, &JR_SELF)).unwrap();
    assert!(
        m.case_open("protocol=nope\n")
            .unwrap_err()
            .contains("unknown protocol")
    );
    assert!(
        m.case_open("bogus=1\nprotocol=run\n")
            .unwrap_err()
            .contains("unknown spec key")
    );
    assert!(m.case_open("protocol=mem\n").unwrap_err().contains("addr"));
    assert!(m.case_open("protocol=png\n").unwrap_err().contains("ref"));
}
