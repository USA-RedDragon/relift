//! Interrupt facts of the SM83 that the lifted code and the runtime share.
//!
//! The interrupt master enable lives in the runtime (`rl_ctx.ime`), not in a
//! register of the IR. Lifted code reaches it through the intrinsics named
//! here; the frontend gives each its C and WebAssembly form.

/// Enable interrupts now. The `EI` delay is the lifter's: it places this
/// after the instruction that follows `EI`.
pub const EI: &str = "ei";

/// Disable interrupts.
pub const DI: &str = "di";

/// Enable interrupts now, for `RETI`.
pub const RETI: &str = "reti";

/// The `HALT` instruction: gives one bit, 1 when it did not wait because an
/// enabled interrupt was already requested (the HALT bug case), else waits
/// for one and gives 0.
pub const HALT: &str = "halt";

/// `STOP`: does the speed switch or enters stop mode as the machine's state
/// says; gives 1 when the byte after `STOP` is not skipped (it runs as the
/// next instruction), else 0.
pub const STOP: &str = "stop";

/// Guest cycles an interrupt entry costs: five machine cycles.
pub const ENTRY_CYCLES: u32 = 20;

/// The interrupt `pending` (the requested and enabled lines, bits 0 to 4)
/// would take: the lowest line and its vector.
pub fn take(pending: u8) -> Option<(u8, u16)> {
    let pending = pending & 0x1F;
    if pending == 0 {
        return None;
    }
    let line = pending.trailing_zeros() as u8;
    Some((line, 0x40 + 8 * u16::from(line)))
}
