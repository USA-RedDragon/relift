//! The Nintendo DS guest: the frontend of both CPUs over `relift-cpu-arm32`
//! (the ARM9 lifted for ARMv5TE, the ARM7 for ARMv4T) for the runtime's NDS
//! layer (`runtime/guests/nds/run`), and a cartridge's units, analyzed by
//! `relift-nds-analyze`, lifted into C units with the program files that
//! run them.

pub mod cycles;
pub mod frontend;
pub mod program;
