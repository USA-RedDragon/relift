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

use rustyboi_core_lib::cartridge::Cartridge;
use rustyboi_core_lib::cpu::bus::ExtBusState;
use rustyboi_core_lib::gb::{GB, Hardware};

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
}

fn hardware(model: u32) -> Hardware {
    match model {
        0 => Hardware::DMG,
        1 => Hardware::CGB,
        2 => Hardware::CGBE,
        3 => Hardware::AGB,
        4 => Hardware::MGB,
        _ => Hardware::CGBB,
    }
}

impl Rlgbd {
    /// A machine in the state the boot program leaves it in.
    pub fn new(model: u32, rom: &[u8]) -> Option<Self> {
        let cart = Cartridge::from_bytes(rom).ok()?;
        let mut gb = GB::new(hardware(model));
        gb.insert(cart);
        gb.skip_bios();
        Some(Self {
            gb,
            st: ExtBusState::default(),
            mid: false,
        })
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
        self.gb.step_instruction(false).1
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

    /// FNV-1a of the presented frame.
    pub fn frame_hash(&mut self) -> u64 {
        let f = self.gb.get_current_frame();
        fnv(0xcbf2_9ce4_8422_2325, f.rgb().iter().copied())
    }

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
/// anything else CGB-B. Returns NULL when the image is not a cartridge.
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

/// FNV-1a of the presented frame.
///
/// # Safety
/// `h` is a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_frame_hash(h: *mut Rlgbd) -> u64 {
    me(h).frame_hash()
}

/// FNV-1a of video RAM, work RAM, OAM, the I/O registers, high RAM and IE.
///
/// # Safety
/// `h` is a live handle between instructions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rlgbd_state_hash(h: *mut Rlgbd) -> u64 {
    me(h).state_hash()
}
