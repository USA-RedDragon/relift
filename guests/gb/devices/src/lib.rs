//! rustyboi-core's Game Boy devices behind a C ABI.
//!
//! One handle owns the whole bus of a machine: the cartridge, the picture
//! unit, the sound unit, the timer, the serial port, the joypad, the DMA
//! engines, and video, work and high RAM. This is the exact
//! per-instruction-boundary mode: the CPU side calls [`rlgbd_begin`] at each
//! instruction boundary, issues the body's bus accesses through
//! [`rlgbd_read`], [`rlgbd_write`], [`rlgbd_internal`] and [`rlgbd_idu`] in the
//! order and M-cycle placement of the hardware, and ends with [`rlgbd_end`].
//! rustyboi keeps the boundary (interrupt dispatch, HALT, STOP, DMA stalls,
//! the opcode fetch) and the epilogue, so every device observes each access at
//! the exact dot. Time is rustyboi's: [`rlgbd_now`] is the CPU's position in
//! dots of the 4194304 Hz master clock.
//!
//! Every function takes the handle [`rlgbd_new`] returned.

mod case;
mod imaging;

use rustyboi_core_lib::audio::AudioOutput;
use rustyboi_core_lib::cartridge::Cartridge;
use rustyboi_core_lib::cpu::bus::ExtBusState;
use rustyboi_core_lib::gb::{GB, Hardware};
use rustyboi_core_lib::input::ButtonState;
use rustyboi_core_lib::ppu::ColorCorrection;
use std::ffi::{CStr, c_char};
use std::io::Write;
use std::sync::{Arc, Mutex};

pub use case::Verdict;
pub use imaging::encode_rgb_png;

/// Joypad bit: A.
pub const BTN_A: u8 = 0x01;
/// Joypad bit: B.
pub const BTN_B: u8 = 0x02;
/// Joypad bit: Select.
pub const BTN_SELECT: u8 = 0x04;
/// Joypad bit: Start.
pub const BTN_START: u8 = 0x08;
/// Joypad bit: Right.
pub const BTN_RIGHT: u8 = 0x10;
/// Joypad bit: Left.
pub const BTN_LEFT: u8 = 0x20;
/// Joypad bit: Up.
pub const BTN_UP: u8 = 0x40;
/// Joypad bit: Down.
pub const BTN_DOWN: u8 = 0x80;

/// Creation flag: seed the post-boot state with the boot program's residue
/// (the state the SRAM dump references were captured in).
pub const NEW_BOOT_RESIDUE: u32 = 1;
/// Creation flag: the cartridge's SRAM chip select is decoded lazily.
pub const NEW_LAZY_SRAM_CS: u32 = 2;

/// Pixels of a frame.
const FRAME_PIXELS: usize = 160 * 144;

/// Bytes of the clock footer of an MBC3 save.
const RTC_FOOTER: usize = 48;

/// Dots in one frame of the picture unit (154 lines of 456).
pub const FRAME_DOTS: u64 = 70224;

/// The CPU registers, as the lifted code's context holds them.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct RlgbdRegs {
    pub a: u8,
    pub f: u8,
    pub b: u8,
    pub c: u8,
    pub d: u8,
    pub e: u8,
    pub h: u8,
    pub l: u8,
    pub pc: u16,
    pub sp: u16,
    pub ime: u8,
}

/// A machine's devices and bus.
pub struct Rlgbd {
    gb: GB,
    st: ExtBusState,
    mid: bool,
    flags: u32,
    frame: Vec<u8>,
    frames: u32,
    case: Option<Box<case::Case>>,
    detail: Vec<u8>,
    audio: std::collections::VecDeque<(f32, f32)>,
    native_audio: Arc<Mutex<Vec<(f32, f32)>>>,
    native_on: bool,
}

struct Sink(Arc<Mutex<Vec<(f32, f32)>>>);

impl AudioOutput for Sink {
    fn start(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        Ok(())
    }

    fn add_samples(&mut self, samples: &[(f32, f32)]) {
        if let Ok(mut v) = self.0.lock() {
            v.extend_from_slice(samples);
        }
    }
}

fn hardware(model: u32) -> Hardware {
    match model {
        0 => Hardware::DMG,
        1 => Hardware::CGB,
        2 => Hardware::CGBE,
        3 => Hardware::AGB,
        4 => Hardware::MGB,
        6 => Hardware::DMG0,
        7 => Hardware::SGB,
        8 => Hardware::SGB2,
        9 => Hardware::CGB0,
        _ => Hardware::CGBB,
    }
}

fn buttons(mask: u8) -> ButtonState {
    ButtonState {
        a: mask & BTN_A != 0,
        b: mask & BTN_B != 0,
        start: mask & BTN_START != 0,
        select: mask & BTN_SELECT != 0,
        up: mask & BTN_UP != 0,
        down: mask & BTN_DOWN != 0,
        left: mask & BTN_LEFT != 0,
        right: mask & BTN_RIGHT != 0,
    }
}

impl Rlgbd {
    /// A machine in the state the boot program leaves it in.
    pub fn new(model: u32, rom: &[u8]) -> Option<Self> {
        Self::new_with(model, rom, 0)
    }

    /// A machine as [`Rlgbd::new`] builds it, with the creation `flags`
    /// ([`NEW_BOOT_RESIDUE`], [`NEW_LAZY_SRAM_CS`]).
    pub fn new_with(model: u32, rom: &[u8], flags: u32) -> Option<Self> {
        let cart = Cartridge::from_bytes(rom).ok()?;
        let mut gb = GB::new(hardware(model));
        gb.insert(cart);
        if flags & NEW_LAZY_SRAM_CS != 0 {
            gb.set_cart_sram_cs_lazy(true);
        }
        if flags & NEW_BOOT_RESIDUE != 0 {
            gb.skip_bios_with_boot_residue();
        } else {
            gb.skip_bios();
        }
        Some(Self {
            gb,
            st: ExtBusState::default(),
            mid: false,
            flags,
            frame: vec![0; FRAME_PIXELS * 3],
            frames: 0,
            case: None,
            detail: vec![0],
            audio: std::collections::VecDeque::new(),
            native_audio: Arc::default(),
            native_on: false,
        })
    }

    /// [`Rlgbd::begin`] after loading `pc`, `sp` and `ime` into the CPU: the
    /// boundary of code that keeps its own registers.
    pub fn enter(&mut self, pc: u16, sp: u16, ime: u8) -> Option<u8> {
        let r = self.gb.cpu_registers_mut();
        r.pc = pc;
        r.sp = sp;
        r.ime = ime != 0;
        self.begin()
    }

    /// [`Rlgbd::begin`] after loading `pc` and `sp` into the CPU, keeping
    /// the interrupt enable the library tracks.
    pub fn at(&mut self, pc: u16, sp: u16) -> Option<u8> {
        let r = self.gb.cpu_registers_mut();
        r.pc = pc;
        r.sp = sp;
        self.begin()
    }

    /// The CPU's program counter.
    pub fn pc(&self) -> u16 {
        self.gb.get_cpu_registers().pc
    }

    /// The CPU's stack pointer.
    pub fn sp(&self) -> u16 {
        self.gb.get_cpu_registers().sp
    }

    /// The number of frames taken so far, by [`Rlgbd::frame_poll`] or a case.
    pub fn frames(&self) -> u32 {
        self.frames
    }

    /// Sets the interrupt master enable, as `RETI` does.
    pub fn set_ime(&mut self, on: bool) {
        self.gb.cpu_registers_mut().ime = on;
    }

    /// The instruction boundary; see [`rlgbd_begin`].
    pub fn begin(&mut self) -> Option<u8> {
        let op = self.gb.ext_begin(&mut self.st);
        self.mid = op.is_some();
        op
    }

    /// A bus read inside an instruction.
    pub fn read(&mut self, addr: u16) -> u8 {
        self.gb.ext_read(&mut self.st, addr)
    }

    /// A bus write inside an instruction.
    pub fn write(&mut self, addr: u16, value: u8) {
        self.gb.ext_write(&mut self.st, addr, value);
    }

    /// An internal M-cycle inside an instruction.
    pub fn internal(&mut self) {
        self.gb.ext_internal(&mut self.st);
    }

    /// A 16-bit increment or decrement of a register holding `pre`.
    pub fn idu(&mut self, pre: u16) {
        self.gb.ext_idu(&mut self.st, pre);
    }

    /// The end of an instruction body of `total` dots.
    pub fn end(&mut self, total: u32) {
        self.gb.ext_end(&mut self.st, total);
        self.mid = false;
    }

    /// Runs the opcode `begin` returned on rustyboi's own CPU.
    pub fn exec_native(&mut self, op: u8) -> u32 {
        let c = self.gb.ext_exec_native(&mut self.st, op);
        self.mid = false;
        c
    }

    /// One whole instruction on rustyboi's own CPU (the reference run).
    pub fn step_native(&mut self) -> u32 {
        self.gb.step_instruction(self.native_on).1
    }

    /// Runs the devices `dots` dots with no CPU access, between instructions.
    pub fn advance(&mut self, dots: u64) {
        if !self.mid {
            self.gb.ext_advance(dots);
        }
    }

    /// The CPU's position in dots.
    pub fn now(&self) -> u64 {
        self.gb.ext_now()
    }

    /// The CPU registers.
    pub fn regs(&self) -> RlgbdRegs {
        let r = self.gb.get_cpu_registers();
        RlgbdRegs {
            a: r.a,
            f: r.f,
            b: r.b,
            c: r.c,
            d: r.d,
            e: r.e,
            h: r.h,
            l: r.l,
            pc: r.pc,
            sp: r.sp,
            ime: r.ime as u8,
        }
    }

    /// Replaces the CPU registers.
    pub fn set_regs(&mut self, i: &RlgbdRegs) {
        let r = self.gb.cpu_registers_mut();
        r.a = i.a;
        r.f = i.f;
        r.b = i.b;
        r.c = i.c;
        r.d = i.d;
        r.e = i.e;
        r.h = i.h;
        r.l = i.l;
        r.pc = i.pc;
        r.sp = i.sp;
        r.ime = i.ime != 0;
    }

    /// A read with no timing effect: the lazy devices are brought up to date first.
    pub fn peek(&mut self, addr: u16) -> u8 {
        self.gb.sync_lazy_peripherals();
        self.gb.read_memory(addr)
    }

    /// FNV-1a of the last taken frame (zeros before the first).
    pub fn frame_hash(&self) -> u64 {
        fnv(0xcbf2_9ce4_8422_2325, self.frame.iter().copied())
    }

    /// FNV-1a of the frame presented now. Unlike [`Rlgbd::frame_poll`] this
    /// leaves the taken frame alone, but presenting clears the picture unit's
    /// frame-ready flag, so it is for lockstep checks of machines that never
    /// poll frames.
    pub fn present_hash(&mut self) -> u64 {
        let f = self.gb.get_current_frame();
        fnv(0xcbf2_9ce4_8422_2325, f.rgb().iter().copied())
    }

    /// Takes the frame the picture unit presents into the handle's buffer and
    /// counts it. Clears the frame-ready flag.
    fn take_frame(&mut self) {
        let f = self.gb.get_current_frame();
        self.frame.copy_from_slice(f.rgb().as_slice());
        self.frames = self.frames.wrapping_add(1);
    }

    /// Call between instructions: when the picture unit finished an LCD frame
    /// since the last poll, takes it. Returns the count of frames taken.
    pub fn frame_poll(&mut self) -> u32 {
        if self.gb.get_ppu_debug_info().0.frame_ready() {
            self.take_frame();
        }
        self.frames
    }

    /// The RGB888 pixels (160 x 144 x 3) of the last taken frame.
    pub fn frame_rgb(&self) -> &[u8] {
        &self.frame
    }

    /// Writes the last taken frame to `path` as an RGB PNG, replacing a file there.
    pub fn write_png(&self, path: &std::path::Path) -> std::io::Result<()> {
        match std::fs::remove_file(path) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(e),
            _ => {}
        }
        let mut file = std::fs::File::create_new(path)?;
        file.write_all(&imaging::encode_rgb_png(160, 144, &self.frame))
    }

    /// Sets the joypad state; bits are the `BTN_*` constants.
    pub fn set_buttons(&mut self, mask: u8) {
        self.gb.set_input_state(buttons(mask));
    }

    /// A read with no lazy sync and no timing effect.
    pub fn read_memory(&self, addr: u16) -> u8 {
        self.gb.read_memory(addr)
    }

    /// The ROM bank mapped at 0x4000 to 0x7FFF now.
    pub fn rom_bank(&self) -> u32 {
        self.gb
            .cartridge()
            .map_or(0, |c| (c.rom_bases().1 / 0x4000) as u32)
    }

    /// The ROM bank mapped at 0x0000 to 0x3FFF now (non-zero on MBC1 in banking mode 1).
    pub fn rom_bank0(&self) -> u32 {
        self.gb
            .cartridge()
            .map_or(0, |c| (c.rom_bases().0 / 0x4000) as u32)
    }

    /// Whether a CGB runs at double speed (KEY1 bit 7); false where KEY1 does
    /// not exist (the original hardware, or a CGB running a cartridge without
    /// color support, where it reads 0xFF).
    pub fn double_speed(&self) -> bool {
        self.gb.should_enable_cgb_features() && self.gb.read_memory(0xFF4D) & 0x80 != 0
    }

    fn rtc_footer(&self) -> Vec<u8> {
        self.gb
            .cartridge()
            .and_then(Cartridge::export_rtc)
            .filter(|b| b.len() == RTC_FOOTER)
            .unwrap_or_default()
    }

    /// The size of the battery save in bytes: the cartridge RAM plus the
    /// 48-byte clock footer of an MBC3 with a timer; 0 without a battery.
    pub fn save_size(&self) -> usize {
        match self.gb.cartridge() {
            Some(c) if c.has_battery() => c.save_ram().len() + self.rtc_footer().len(),
            _ => 0,
        }
    }

    /// The battery save as a `.sav` file: the RAM, then the clock footer when
    /// there is one.
    pub fn save_bytes(&self) -> Vec<u8> {
        match self.gb.cartridge() {
            Some(c) if c.has_battery() => {
                let mut out = c.save_ram().to_vec();
                out.extend_from_slice(&self.rtc_footer());
                out
            }
            _ => Vec::new(),
        }
    }

    /// Loads a `.sav` file: the RAM prefix, then the clock footer if present.
    pub fn load_save(&mut self, bytes: &[u8]) -> Result<(), String> {
        let Some(c) = self.gb.cartridge_mut() else {
            return Err("no cartridge".into());
        };
        if !c.has_battery() {
            return Err("the cartridge has no battery".into());
        }
        let ram = c.save_ram().len();
        if ram > 0 {
            c.import_save_ram(&bytes[..bytes.len().min(ram)])?;
        }
        if c.has_rtc()
            && let Some(footer) = bytes.get(ram..ram + RTC_FOOTER)
        {
            c.import_rtc(footer)?;
        }
        Ok(())
    }

    /// Chooses how the picture unit turns a colour palette entry into RGB888:
    /// with the colour correction of the LCD (`true`, the default of a new
    /// machine) or by plain expansion of the 5-bit channels (`false`).
    pub fn set_color_correction(&mut self, on: bool) {
        self.gb.set_cgb_color_conversion(if on {
            ColorCorrection::Lcd
        } else {
            ColorCorrection::Linear
        });
    }

    /// Starts or stops collecting the sound unit's samples.
    pub fn audio_enable(&mut self, on: bool) {
        self.gb.ext_set_audio(on);
        if on && !self.native_on {
            let sink = Sink(self.native_audio.clone());
            self.native_on = self.gb.enable_audio(Box::new(sink)).is_ok();
        } else if !on {
            self.native_on = false;
            self.audio.clear();
            if let Ok(mut v) = self.native_audio.lock() {
                v.clear();
            }
        }
    }

    /// Moves up to `out.len() / 2` collected sample frames into `out` as
    /// interleaved left and right; returns how many.
    pub fn audio_take(&mut self, out: &mut [f32]) -> usize {
        self.audio.extend(self.gb.ext_take_samples());
        if let Ok(mut v) = self.native_audio.lock() {
            self.audio.extend(v.drain(..));
        }
        let (pairs, _) = out.as_chunks_mut::<2>();
        let mut n = 0;
        for pair in pairs {
            let Some((l, r)) = self.audio.pop_front() else {
                break;
            };
            *pair = [l, r];
            n += 1;
        }
        n
    }

    /// Opens the test case `spec` (see the `case` module); before the first instruction.
    pub fn case_open(&mut self, spec: &str) -> Result<(), String> {
        let case = case::Case::open(self, spec)?;
        self.case = Some(Box::new(case));
        self.set_detail("");
        Ok(())
    }

    /// Supplies the CPU registers for a verdict that reads them (mooneye).
    pub fn case_regs(&mut self, regs: &RlgbdRegs) {
        if let Some(c) = self.case.as_mut() {
            c.regs = Some(*regs);
        }
    }

    /// Polls the open case at an instruction boundary with `pc`; see [`Verdict`].
    pub fn case_poll(&mut self, pc: u16) -> Verdict {
        let Some(mut c) = self.case.take() else {
            return Verdict::Fail;
        };
        let v = c.poll(self, pc);
        if v != Verdict::Continue && v != Verdict::NeedRegs {
            let d = c.detail().to_string();
            self.set_detail(&d);
        }
        self.case = Some(c);
        v
    }

    /// The verdict's detail as a NUL-terminated string.
    pub fn case_detail(&self) -> &[u8] {
        &self.detail
    }

    fn set_detail(&mut self, text: &str) {
        self.detail.clear();
        self.detail.extend(text.bytes().filter(|&b| b != 0));
        self.detail.push(0);
    }

    /// The instruction boundary; see [`rlgbd_begin`].
    /// FNV-1a of every readable device-visible cell: video RAM, work RAM, OAM, I/O, high RAM and IE.
    pub fn state_hash(&mut self) -> u64 {
        self.gb.sync_lazy_peripherals();
        let gb = &self.gb;
        let ranges = [
            (0x8000u32, 0x9FFFu32),
            (0xC000, 0xDFFF),
            (0xFE00, 0xFE9F),
            (0xFF00, 0xFFFF),
        ];
        fnv(
            0xcbf2_9ce4_8422_2325,
            ranges
                .iter()
                .flat_map(|&(lo, hi)| (lo..=hi).map(|a| gb.read_memory(a as u16))),
        )
    }
}

fn fnv(mut h: u64, bytes: impl Iterator<Item = u8>) -> u64 {
    for b in bytes {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x0100_0000_01b3);
    }
    h
}

fn me<'a>(h: *mut Rlgbd) -> &'a mut Rlgbd {
    // SAFETY: every caller of the ABI passes a handle from `rlgbd_new` that has
    // not been freed and is used by one thread at a time.
    unsafe { &mut *h }
}

/// Creates a machine with the cartridge `rom` of `len` bytes, in the state a
/// boot program leaves it. `model`: 0 DMG, 1 CGB-C, 2 CGB-E, 3 AGB, 4 MGB,
/// 5 CGB-B, 6 DMG0, 7 SGB, 8 SGB2, 9 CGB0, anything else CGB-B. Returns NULL
/// when the image is not a cartridge.
///
/// # Safety
/// `rom` must point to `len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_new(model: u32, rom: *const u8, len: usize) -> *mut Rlgbd {
    if rom.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: the caller guarantees `len` readable bytes at `rom`.
    let bytes = unsafe { std::slice::from_raw_parts(rom, len) };
    Rlgbd::new(model, bytes).map_or(std::ptr::null_mut(), |m| Box::into_raw(Box::new(m)))
}

/// As `rlgbd_new` with creation `flags`: bit 0 seeds the boot program's
/// residue (`NEW_BOOT_RESIDUE`), bit 1 decodes the cartridge SRAM chip select
/// lazily (`NEW_LAZY_SRAM_CS`).
///
/// # Safety
/// `rom` must point to `len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_new_with(
    model: u32,
    rom: *const u8,
    len: usize,
    flags: u32,
) -> *mut Rlgbd {
    if rom.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: the caller guarantees `len` readable bytes at `rom`.
    let bytes = unsafe { std::slice::from_raw_parts(rom, len) };
    Rlgbd::new_with(model, bytes, flags)
        .map_or(std::ptr::null_mut(), |m| Box::into_raw(Box::new(m)))
}

/// Frees a machine; NULL is allowed.
///
/// # Safety
/// `h` is NULL or a handle from `rlgbd_new` not freed yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_free(h: *mut Rlgbd) {
    if !h.is_null() {
        // SAFETY: `h` came from `Box::into_raw` in `rlgbd_new` and is freed once.
        drop(unsafe { Box::from_raw(h) });
    }
}

/// The instruction boundary. Returns the opcode (0 to 255) to run: its fetch is
/// charged and `pc` is past it; run the body, then `rlgbd_end`. Returns -1 when
/// the boundary consumed the step (interrupt dispatch, HALT, STOP, DMA stall).
/// The CPU registers stay in the machine; the caller loads them with
/// `rlgbd_get_regs` and stores the changed ones with `rlgbd_set_regs`.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_begin(h: *mut Rlgbd) -> i32 {
    me(h).begin().map_or(-1, i32::from)
}

/// A bus read at `addr`, one M-cycle.
///
/// # Safety
/// `h` is a live handle between `rlgbd_begin` and `rlgbd_end`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_read(h: *mut Rlgbd, addr: u16) -> u8 {
    me(h).read(addr)
}

/// A bus write, one M-cycle.
///
/// # Safety
/// As `rlgbd_read`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_write(h: *mut Rlgbd, addr: u16, value: u8) {
    me(h).write(addr, value);
}

/// An internal M-cycle with no bus access (CALL, RST, PUSH, RET cc).
///
/// # Safety
/// As `rlgbd_read`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_internal(h: *mut Rlgbd) {
    me(h).internal();
}

/// A 16-bit increment or decrement of a register that held `pre` (the original
/// Game Boy's OAM corruption trigger).
///
/// # Safety
/// As `rlgbd_read`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_idu(h: *mut Rlgbd, pre: u16) {
    me(h).idu(pre);
}

/// The end of a body of `total` dots, fetch included.
///
/// # Safety
/// As `rlgbd_read`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_end(h: *mut Rlgbd, total: u32) {
    me(h).end(total);
}

/// Runs the opcode `rlgbd_begin` returned on rustyboi's own CPU (for forms the
/// caller does not run itself); the instruction ends. Returns its dots.
///
/// # Safety
/// As `rlgbd_read`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_exec_native(h: *mut Rlgbd, op: u8) -> u32 {
    me(h).exec_native(op)
}

/// One whole instruction on rustyboi's own CPU, for the reference run of a
/// lockstep check. Returns its dots.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_step_native(h: *mut Rlgbd) -> u32 {
    me(h).step_native()
}

/// Runs the devices `dots` dots with no CPU access. Ignored inside an instruction.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_advance(h: *mut Rlgbd, dots: u64) {
    me(h).advance(dots);
}

/// Runs the devices to dot `when` when it is ahead; returns the position.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_run_to(h: *mut Rlgbd, when: u64) -> u64 {
    let m = me(h);
    let now = m.now();
    if when > now {
        m.advance(when - now);
    }
    m.now()
}

/// The CPU's position in dots.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_now(h: *mut Rlgbd) -> u64 {
    me(h).now()
}

/// Copies the CPU registers into `out`.
///
/// # Safety
/// `out` points to a writable `RlgbdRegs`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_get_regs(h: *mut Rlgbd, out: *mut RlgbdRegs) {
    // SAFETY: the caller guarantees `out` is writable.
    unsafe { *out = me(h).regs() };
}

/// Replaces the CPU registers.
///
/// # Safety
/// `inp` points to a readable `RlgbdRegs`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_set_regs(h: *mut Rlgbd, inp: *const RlgbdRegs) {
    // SAFETY: the caller guarantees `inp` is readable.
    me(h).set_regs(unsafe { &*inp });
}

/// A read for a debugger or a check: no timing effect.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_peek(h: *mut Rlgbd, addr: u16) -> u8 {
    me(h).peek(addr)
}

/// FNV-1a of the last frame taken by `rlgbd_frame_poll` or a case (the bytes
/// of `rlgbd_frame_rgb`); consumes nothing.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_frame_hash(h: *mut Rlgbd) -> u64 {
    me(h).frame_hash()
}

/// FNV-1a of the frame the picture unit presents now, for lockstep checks of
/// machines that never poll frames. It does not touch the taken frame, but
/// presenting clears the picture unit's frame-ready flag.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_present_hash(h: *mut Rlgbd) -> u64 {
    me(h).present_hash()
}

/// `rlgbd_begin` after loading `pc`, `sp` and `ime` (0 or 1) into the CPU: for
/// code that keeps its registers elsewhere and calls this at every boundary.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_enter(h: *mut Rlgbd, pc: u16, sp: u16, ime: u8) -> i32 {
    me(h).enter(pc, sp, ime).map_or(-1, i32::from)
}

/// `rlgbd_begin` after loading `pc` and `sp` into the CPU, keeping the
/// interrupt enable the library tracks (`EI`, `DI` and `RETI` run through
/// the library): the boundary of code that keeps its other registers.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_at(h: *mut Rlgbd, pc: u16, sp: u16) -> i32 {
    me(h).at(pc, sp).map_or(-1, i32::from)
}

/// The CPU's program counter.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_pc(h: *mut Rlgbd) -> u16 {
    me(h).pc()
}

/// The CPU's stack pointer.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_sp(h: *mut Rlgbd) -> u16 {
    me(h).sp()
}

/// The number of frames taken so far by `rlgbd_frame_poll` or a case, without
/// taking one.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_frames(h: *mut Rlgbd) -> u32 {
    me(h).frames()
}

/// Sets the interrupt master enable (0 or 1), as `RETI` does.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_set_ime(h: *mut Rlgbd, on: u8) {
    me(h).set_ime(on != 0);
}

/// Call between instructions: when the picture unit completed an LCD frame
/// since the last poll, takes the presented frame into the handle. Returns the
/// number of frames taken so far.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_frame_poll(h: *mut Rlgbd) -> u32 {
    me(h).frame_poll()
}

/// The 160 x 144 x 3 RGB888 bytes of the last taken frame (zeros before the
/// first). Valid until the handle is freed; the content changes at each take.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_frame_rgb(h: *mut Rlgbd) -> *const u8 {
    me(h).frame_rgb().as_ptr()
}

/// Writes the last taken frame to `path` as an RGB PNG. Returns 0, or -1.
///
/// # Safety
/// `path` is a NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_write_png(h: *mut Rlgbd, path: *const c_char) -> i32 {
    let Some(path) = cstr(path) else {
        return -1;
    };
    match me(h).write_png(std::path::Path::new(path)) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Sets the joypad state. Bits: 0x01 A, 0x02 B, 0x04 Select, 0x08 Start,
/// 0x10 Right, 0x20 Left, 0x40 Up, 0x80 Down; a set bit is a held button.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_set_buttons(h: *mut Rlgbd, mask: u8) {
    me(h).set_buttons(mask);
}

/// A read with no lazy sync and no timing effect (what a test runner polls).
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_read_memory(h: *mut Rlgbd, addr: u16) -> u8 {
    me(h).read_memory(addr)
}

/// The ROM bank mapped at 0x4000 to 0x7FFF now.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_rom_bank(h: *mut Rlgbd) -> u32 {
    me(h).rom_bank()
}

/// The ROM bank mapped at 0x0000 to 0x3FFF now (non-zero only for MBC1 in its
/// banking mode 1).
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_rom_bank0(h: *mut Rlgbd) -> u32 {
    me(h).rom_bank0()
}

/// The size of the battery save, a raw `.sav` file: the cartridge RAM, then
/// the 48-byte clock footer of an MBC3 with a timer. 0 without a battery.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_save_size(h: *mut Rlgbd) -> usize {
    me(h).save_size()
}

/// Copies the battery save into `out` (at most `cap` bytes); returns the bytes
/// copied.
///
/// # Safety
/// `out` points to `cap` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_save_bytes(h: *mut Rlgbd, out: *mut u8, cap: usize) -> usize {
    let bytes = me(h).save_bytes();
    let n = bytes.len().min(cap);
    if n > 0 {
        // SAFETY: the caller guarantees `cap` writable bytes at `out`, and `n <= cap`.
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, n) };
    }
    n
}

/// Loads a `.sav` file: the RAM prefix, then the clock footer when there is
/// one. Returns 0, or -1 (no battery, or a footer that does not fit).
///
/// # Safety
/// `data` points to `len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_load_save(h: *mut Rlgbd, data: *const u8, len: usize) -> i32 {
    let bytes: &[u8] = if len == 0 {
        &[]
    } else {
        // SAFETY: the caller guarantees `len` readable bytes at `data`.
        unsafe { std::slice::from_raw_parts(data, len) }
    };
    if me(h).load_save(bytes).is_ok() {
        0
    } else {
        -1
    }
}

/// 1 while a CGB runs at double speed (KEY1 bit 7), else 0.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_double_speed(h: *mut Rlgbd) -> u8 {
    u8::from(me(h).double_speed())
}

/// Applies (nonzero `on`, the default of a new machine) or leaves out the LCD
/// colour correction of the picture unit's RGB888 output. A corpus case in
/// progress grades with the conversion its reference needs, whatever this says.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_set_color_correction(h: *mut Rlgbd, on: u8) {
    me(h).set_color_correction(on != 0);
}

/// Starts (nonzero `on`) or stops collecting the sound unit's samples; stopping
/// drops the ones not yet taken. Collection is off in a new machine.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_audio_enable(h: *mut Rlgbd, on: u8) {
    me(h).audio_enable(on != 0);
}

/// Moves up to `cap` collected sample frames into `out` as interleaved left and
/// right `f32` at 44100 Hz, in order, and returns how many.
///
/// # Safety
/// `h` is a live handle between instructions; `out` is writable for `2 * cap` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_audio_take(h: *mut Rlgbd, out: *mut f32, cap: usize) -> usize {
    if out.is_null() || cap == 0 {
        return 0;
    }
    // SAFETY: the caller guarantees `out` holds `2 * cap` floats.
    let out = unsafe { std::slice::from_raw_parts_mut(out, cap * 2) };
    me(h).audio_take(out)
}

/// Opens a test case before the first instruction. `spec` is newline-separated
/// `key=value` lines (see the case module). Returns 0, or -1 with the reason
/// in `rlgbd_case_detail`.
///
/// # Safety
/// `spec` is a NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_case_open(h: *mut Rlgbd, spec: *const c_char) -> i32 {
    let m = me(h);
    let Some(text) = cstr(spec) else {
        m.set_detail("the spec is not valid UTF-8");
        return -1;
    };
    match m.case_open(text) {
        Ok(()) => 0,
        Err(e) => {
            m.set_detail(&e);
            -1
        }
    }
}

/// Polls the open case at an instruction boundary, before the instruction at
/// `pc` runs (every boundary, repeated ones of HALT and stalls included).
/// Returns 0 continue, 1 pass, 2 fail, 3 not supported by this machine, 4 the
/// verdict needs the CPU registers: pass them to `rlgbd_case_regs` and poll
/// again at the same boundary. After a nonzero verdict the answer is final.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_case_poll(h: *mut Rlgbd, pc: u16) -> i32 {
    me(h).case_poll(pc) as i32
}

/// Supplies the CPU registers (`pc` and `sp` ignored) for a verdict that reads
/// them, after `rlgbd_case_poll` returned 4.
///
/// # Safety
/// `regs` points to a readable `RlgbdRegs`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_case_regs(h: *mut Rlgbd, regs: *const RlgbdRegs) {
    // SAFETY: the caller guarantees `regs` is readable.
    me(h).case_regs(unsafe { &*regs });
}

/// The NUL-terminated detail of the verdict (or of a failed `rlgbd_case_open`);
/// valid until the next case call.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_case_detail(h: *mut Rlgbd) -> *const c_char {
    me(h).case_detail().as_ptr().cast()
}

fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    // SAFETY: the caller of the ABI passes a NUL-terminated string.
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// FNV-1a of video RAM, work RAM, OAM, the I/O registers, high RAM and IE.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_state_hash(h: *mut Rlgbd) -> u64 {
    me(h).state_hash()
}
