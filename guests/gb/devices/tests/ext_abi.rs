use relift_gb_devices::{
    BTN_A, BTN_START, FRAME_DOTS, NEW_BOOT_RESIDUE, Rlgbd, Verdict, encode_rgb_png,
    rlgbd_case_detail, rlgbd_case_open, rlgbd_case_poll, rlgbd_double_speed, rlgbd_enter,
    rlgbd_frame_hash, rlgbd_frame_poll, rlgbd_frame_rgb, rlgbd_free, rlgbd_load_save, rlgbd_new,
    rlgbd_new_with, rlgbd_read_memory, rlgbd_rom_bank, rlgbd_save_bytes, rlgbd_save_size,
    rlgbd_set_buttons, rlgbd_step_native, rlgbd_write_png,
};
use std::ffi::{CStr, CString};

fn image(cart_type: u8, size_code: u8, ram_code: u8, cgb: u8, code: &[u8]) -> Vec<u8> {
    let mut r = vec![0u8; (2usize << size_code) * 0x4000];
    r[0x143] = cgb;
    r[0x147] = cart_type;
    r[0x148] = size_code;
    r[0x149] = ram_code;
    r[0x100..0x104].copy_from_slice(&[0x00, 0xC3, 0x50, 0x01]);
    r[0x150..0x150 + code.len()].copy_from_slice(code);
    r
}

fn run(m: &mut Rlgbd, steps: u32) {
    for _ in 0..steps {
        if let Some(op) = m.begin() {
            m.exec_native(op);
        }
    }
}

const SPIN: [u8; 2] = [0x18, 0xFE];

#[test]
fn every_hardware_code_builds_a_machine_of_its_own() {
    let rom = image(0, 0, 0, 0x80, &SPIN);
    let state = |model: u32| {
        let mut m = Rlgbd::new(model, &rom).unwrap();
        (m.regs(), m.now(), m.state_hash())
    };
    assert_ne!(state(6), state(0));
    assert_ne!(state(7), state(0));
    assert_ne!(state(8), state(7));
    assert_ne!(state(9), state(1));
    assert_ne!(state(4), state(1));
    assert_eq!(state(5), state(1));
    assert_eq!(state(77), state(5));
}

#[test]
fn enter_matches_begin_with_the_machines_own_registers() {
    let rom = image(
        0,
        0,
        0,
        0,
        &[0x3E, 0x91, 0xE0, 0x40, 0x3C, 0xFB, 0x18, 0xFB],
    );
    let mut a = Rlgbd::new(1, &rom).unwrap();
    let mut b = Rlgbd::new(1, &rom).unwrap();
    for _ in 0..30_000 {
        if let Some(op) = a.begin() {
            a.exec_native(op);
        }
        let r = b.regs();
        if let Some(op) = b.enter(r.pc, r.sp, r.ime) {
            b.exec_native(op);
        }
        assert_eq!((a.regs(), a.now()), (b.regs(), b.now()));
    }
    assert_eq!(a.state_hash(), b.state_hash());
}

#[test]
fn enter_loads_pc_sp_and_ime() {
    let rom = image(0, 0, 0, 0, &[0x00, 0x00, 0x00]);
    let mut m = Rlgbd::new(0, &rom).unwrap();
    let op = m.enter(0x0150, 0xC123, 1);
    assert_eq!(op, Some(0x00));
    m.exec_native(0x00);
    let r = m.regs();
    assert_eq!((r.pc, r.sp, r.ime), (0x0151, 0xC123, 1));
}

#[test]
fn frame_polling_takes_each_completed_frame_once() {
    let rom = image(0, 0, 0, 0, &SPIN);
    let mut m = Rlgbd::new(0, &rom).unwrap();
    assert_eq!(m.frame_hash(), m.frame_hash());
    assert!(m.frame_rgb().iter().all(|&b| b == 0));
    let start = m.now();
    let mut last = 0;
    while m.now() < start + 3 * FRAME_DOTS {
        run(&mut m, 1);
        let n = m.frame_poll();
        assert!(n >= last && n - last <= 1);
        last = n;
    }
    assert!((2..=4).contains(&last), "{last}");
    assert_eq!(m.frame_poll(), last);
    let rgb = m.frame_rgb().to_vec();
    assert_eq!(rgb.len(), 160 * 144 * 3);
    assert!(rgb.iter().any(|&b| b != 0));
    let before = m.frame_hash();
    assert_eq!(m.present_hash(), m.present_hash());
    assert_eq!(m.frame_hash(), before);
    let path = std::env::temp_dir().join(format!(
        "relift-gb-devices-{}-frame.png",
        std::process::id()
    ));
    m.write_png(&path).unwrap();
    m.write_png(&path).unwrap();
    assert_eq!(
        std::fs::read(&path).unwrap(),
        encode_rgb_png(160, 144, &rgb)
    );
    assert!(
        m.write_png(std::path::Path::new("/nonexistent-dir/x.png"))
            .is_err()
    );
}

#[test]
fn buttons_reach_the_joypad_register() {
    let rom = image(
        0,
        0,
        0,
        0,
        &[0x3E, 0x10, 0xE0, 0x00, 0xF0, 0x00, 0xE0, 0x80, 0x18, 0xF6],
    );
    let mut m = Rlgbd::new(0, &rom).unwrap();
    run(&mut m, 40);
    assert_eq!(m.read_memory(0xFF80) & 0x0F, 0x0F);
    m.set_buttons(BTN_A | BTN_START);
    run(&mut m, 40);
    assert_eq!(m.read_memory(0xFF80) & 0x0F, 0x06);
    m.set_buttons(0);
    run(&mut m, 40);
    assert_eq!(m.read_memory(0xFF80) & 0x0F, 0x0F);
}

#[test]
fn rom_banks_follow_the_mapper_registers() {
    let mbc5 = image(0x19, 1, 0, 0, &[0x3E, 0x03, 0xEA, 0x00, 0x20, 0x18, 0xFE]);
    let mut m = Rlgbd::new(0, &mbc5).unwrap();
    assert_eq!((m.rom_bank0(), m.rom_bank()), (0, 1));
    run(&mut m, 20);
    assert_eq!((m.rom_bank0(), m.rom_bank()), (0, 3));

    let mbc1 = image(
        1,
        5,
        0,
        0,
        &[0x3E, 0x01, 0xEA, 0x00, 0x60, 0xEA, 0x00, 0x40, 0x18, 0xFE],
    );
    let mut m = Rlgbd::new(0, &mbc1).unwrap();
    run(&mut m, 20);
    assert_ne!(m.rom_bank0(), 0);
    assert_eq!(m.rom_bank(), m.rom_bank0() + 1);
}

#[test]
fn the_battery_save_is_a_raw_sav_file() {
    let code = [
        0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x77, 0xEA, 0x00, 0xA0, 0x18, 0xFE,
    ];
    let rom = image(0x1B, 0, 2, 0, &code);
    let mut m = Rlgbd::new(0, &rom).unwrap();
    run(&mut m, 40);
    assert_eq!(m.save_size(), 8192);
    let sav = m.save_bytes();
    assert_eq!((sav.len(), sav[0]), (8192, 0x77));
    let mut fresh = Rlgbd::new(0, &rom).unwrap();
    assert_ne!(fresh.save_bytes()[0], 0x77);
    fresh.load_save(&sav).unwrap();
    assert_eq!(fresh.save_bytes(), sav);
    let mut short = Rlgbd::new(0, &rom).unwrap();
    short.load_save(&[0x42; 16]).unwrap();
    assert_eq!(
        &short.save_bytes()[..17],
        &[&[0x42u8; 16][..], &[short.save_bytes()[16]][..]].concat()[..]
    );

    let timer = image(0x10, 0, 2, 0, &SPIN);
    let mut clock = Rlgbd::new(0, &timer).unwrap();
    assert_eq!(clock.save_size(), 8192 + 48);
    let sav = clock.save_bytes();
    assert_eq!(sav.len(), 8192 + 48);
    clock.load_save(&sav).unwrap();
    assert_eq!(clock.save_size(), 8192 + 48);

    let none = image(0, 0, 0, 0, &SPIN);
    let mut plain = Rlgbd::new(0, &none).unwrap();
    assert_eq!(plain.save_size(), 0);
    assert!(plain.save_bytes().is_empty());
    assert!(plain.load_save(&[1, 2, 3]).is_err());
}

#[test]
fn double_speed_follows_key1_and_instructions_report_their_dots() {
    let code = [
        0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00, 0x3E, 0x91, 0xE0, 0x40, 0x21, 0x00, 0xC0, 0x3C, 0x22,
        0x00, 0x18, 0xFA,
    ];
    let rom = image(0, 0, 0, 0x80, &code);
    let mut m = Rlgbd::new(1, &rom).unwrap();
    assert!(!m.double_speed());
    let mut fast = 0;
    for _ in 0..40_000 {
        let before = m.now();
        let cycles = m.step_native();
        assert_eq!(m.now() - before, u64::from(cycles));
        fast += u32::from(m.double_speed());
    }
    assert!(fast > 0 && fast < 40_000, "{fast}");
}

#[test]
fn creation_flags_seed_the_boot_residue() {
    let rom = image(0, 0, 0, 0x80, &SPIN);
    let mut plain = Rlgbd::new(1, &rom).unwrap();
    let mut residue = Rlgbd::new_with(1, &rom, NEW_BOOT_RESIDUE).unwrap();
    assert_ne!(plain.state_hash(), residue.state_hash());
    let mut opened = Rlgbd::new(1, &rom).unwrap();
    opened
        .case_open("protocol=sram\nref=/x\nmode=cgb\n")
        .unwrap();
    assert_eq!(opened.state_hash(), residue.state_hash());
}

#[test]
fn the_c_functions_drive_a_case_end_to_end() {
    let code = [
        0x06, 0x03, 0x0E, 0x05, 0x16, 0x08, 0x1E, 0x0D, 0x26, 0x15, 0x2E, 0x22, 0x40, 0x18, 0xFE,
    ];
    let rom = image(0x1B, 0, 2, 0, &code);
    // SAFETY: `rom` has `rom.len()` readable bytes.
    let h = unsafe { rlgbd_new(0, rom.as_ptr(), rom.len()) };
    assert!(!h.is_null());
    // SAFETY: a NULL image is rejected.
    assert!(unsafe { rlgbd_new_with(0, std::ptr::null(), 0, 0) }.is_null());
    let spec = CString::new("protocol=mooneye\n").unwrap();
    let bad = CString::new("protocol=what\n").unwrap();
    // SAFETY: `h` is live and `spec` is NUL-terminated.
    unsafe {
        assert_eq!(rlgbd_case_open(h, bad.as_ptr()), -1);
        assert!(
            CStr::from_ptr(rlgbd_case_detail(h))
                .to_str()
                .unwrap()
                .contains("unknown protocol")
        );
        assert_eq!(rlgbd_case_open(h, spec.as_ptr()), 0);
    }
    let mut verdict = 0;
    for _ in 0..1000 {
        // SAFETY: `h` is live and between instructions.
        let pc = unsafe { (*h).regs().pc };
        // SAFETY: `h` is live and between instructions.
        verdict = unsafe { rlgbd_case_poll(h, pc) };
        if verdict != 0 {
            break;
        }
        // SAFETY: `h` is live and between instructions.
        unsafe { rlgbd_step_native(h) };
    }
    assert_eq!(verdict, Verdict::NeedRegs as i32);
    // SAFETY: `h` is live; the machine's registers are read by the safe API.
    unsafe {
        let regs = (*h).regs();
        (*h).case_regs(&regs);
        assert_eq!(rlgbd_case_poll(h, regs.pc), 1);
        assert_eq!(CStr::from_ptr(rlgbd_case_detail(h)).to_str().unwrap(), "ok");
        assert_eq!(rlgbd_frame_hash(h), (*h).frame_hash());
        assert!(rlgbd_frame_poll(h) <= 1);
        assert!(!rlgbd_frame_rgb(h).is_null());
        rlgbd_set_buttons(h, BTN_A);
        assert_eq!(rlgbd_read_memory(h, 0x0150), 0x06);
        assert_eq!(rlgbd_rom_bank(h), 1);
        assert_eq!(rlgbd_double_speed(h), 0);
        assert_eq!(rlgbd_enter(h, 0x0150, 0xFFFE, 0), 0x06);
        let size = rlgbd_save_size(h);
        assert_eq!(size, 8192);
        let mut out = vec![0u8; size];
        assert_eq!(rlgbd_save_bytes(h, out.as_mut_ptr(), 100), 100);
        assert_eq!(rlgbd_save_bytes(h, out.as_mut_ptr(), size), size);
        assert_eq!(rlgbd_load_save(h, out.as_ptr(), out.len()), 0);
        assert_eq!(rlgbd_load_save(h, std::ptr::null(), 0), 0);
        let path =
            std::env::temp_dir().join(format!("relift-gb-devices-{}-c.png", std::process::id()));
        let c = CString::new(path.to_str().unwrap()).unwrap();
        assert_eq!(rlgbd_write_png(h, c.as_ptr()), 0);
        assert_eq!(rlgbd_write_png(h, std::ptr::null()), -1);
        rlgbd_free(h);
        rlgbd_free(std::ptr::null_mut());
    }
}
