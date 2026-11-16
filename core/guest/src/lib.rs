//! The guest contract and vocabulary: the traits a guest implements and the ids
//! every stage shares.

pub mod blob;
pub mod toy;

/// How a handheld guest's program enters, and where a bare save name
/// lives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Entry {
    /// `main`, with `save` a path.
    Main,
    /// `SDL_main` in the shared library of an Android app, with `save` a
    /// file name in the app's files directory.
    Android,
    /// `rl_ios_main`, which the iOS glue's `main` runs once the app has
    /// launched, with `save` a file name in the app's Documents.
    Ios,
}

use relift_ir::{Block, CryptoOp, FpOp, Reg, RegFile, Width};

/// How a guest turns its own symbol names into host names.
///
/// The symbol scheme belongs to the guest (Itanium remangling for the Switch,
/// another scheme or none for a later guest), so the core only sees this
/// trait and never a demangler.
pub trait Naming {
    /// The host identifier for `symbol` in `module`, or `None` when the guest
    /// cannot name it and the caller falls back to an address-based name.
    fn host_name(&self, module: &str, symbol: &str) -> Option<String>;
}

/// Why a frontend could not lift the code at an address.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LiftFailure {
    /// The raw instruction unit, as the frontend reads it (at most 8 bytes).
    pub insn: u64,
    /// What went wrong, for reports.
    pub message: String,
}

/// Where a register slot lives in the lifted-code context.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Home {
    /// A context field: a C lvalue through `c`, such as `c->x[3]`.
    Field(String),
    /// A condition flag: one bit of a packed context word. The plain build
    /// keeps flags in locals only; the flags-stored build packs them there at
    /// every boundary.
    Flag {
        /// The C lvalue of the packed word, such as `c->nzcv`.
        word: String,
        /// The bit of the flag in that word.
        bit: u32,
    },
}

/// The return check of a CPU with a link register ([`Frontend::return_link`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReturnLink {
    /// The runtime function a call site calls when its callee returned
    /// anything but the call's return address, as
    /// `uint64_t f(rl_ctx *c, uint64_t to, uint64_t pc)` with the context
    /// stored, where `to` is what the callee returned and `pc` the call's
    /// address; it returns `RL_PENDING`.
    pub elsewhere: &'static str,
}

/// How the WebAssembly emitter (`relift-emit-wasm`) carries an intrinsic;
/// the counterpart of the C that [`Frontend::intrinsic`] gives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WasmIntrinsic {
    /// A runtime entry imported as `intrinsic.<name>`, which takes the
    /// context and memory base before the operands.
    Import,
    /// No effect; only for intrinsics without a result.
    Nop,
    /// The constant, for intrinsics with a result and no effect.
    Const(u128),
    /// Zeroes the naturally aligned block of this many bytes (a power of
    /// two and a multiple of 8) that holds the one address operand.
    ZeroBlock(u32),
}

/// A CPU frontend: decoding and lifting, plus the facts the C emitter needs
/// about the CPU's half of the runtime ABI.
pub trait Frontend {
    /// The code granule in bytes: the size and alignment of the smallest
    /// instruction.
    fn granule(&self) -> u64;

    /// The register file of every lifted block.
    fn regs(&self) -> RegFile;

    /// The stack pointer, which the landing-pad check compares with the
    /// unwind target's (`c->pend.sp`); `None` for a CPU without one.
    fn stack_pointer(&self) -> Option<Reg>;

    /// The host function the dispatch table holds at every landing pad, which
    /// is not a function entry: a computed jump to a pad (an unwinder's
    /// install step) must unwind the host stack to the frame that owns it.
    /// `None` when the guest has no landing pads.
    fn pad_resume(&self) -> Option<&'static str> {
        None
    }

    /// The registers the CPU's calling convention keeps across a call, which
    /// analysis may assume unchanged after one.
    fn preserved(&self) -> Vec<Reg>;

    /// The registers a caller may keep in its C locals across a direct
    /// call to a lifted function the emitter proves returns them with
    /// their entry values (`docs/abi.md` section 5.3). Empty by default.
    /// A frontend that names registers here promises that its
    /// [`Frontend::undefined`] call never returns `RL_OK` and that a
    /// guard's slow path writes no register its block does not write.
    fn callee_saved(&self) -> Vec<Reg> {
        Vec::new()
    }

    /// Lifts the instruction at `pc`, whose bytes start `bytes`.
    ///
    /// # Errors
    ///
    /// The bytes are not an instruction the frontend can lift.
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure>;

    /// The runtime headers lifted C includes, such as `relift/arm64/abi.h`.
    fn abi_headers(&self) -> &'static [&'static str];

    /// Where `reg` lives in the context.
    fn home(&self, reg: Reg) -> Home;

    /// The C for intrinsic `name` applied to the C expressions `args`: an
    /// expression for intrinsics with a result, a statement without the
    /// trailing semicolon otherwise. `None` for an intrinsic it does not
    /// know.
    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String>;

    /// How the WebAssembly emitter carries intrinsic `name`; `None` for an
    /// intrinsic it does not know, which the emitter reports as an error.
    fn wasm_intrinsic(&self, name: &str) -> Option<WasmIntrinsic> {
        let _ = name;
        None
    }

    /// An `int` C expression that enters the runtime for trap `code` (the
    /// guest's system call); a call boundary that may return `RL_PENDING`.
    fn trap(&self, code: u32) -> String;

    /// An `int` C expression that reports the unliftable instruction `insn`
    /// at `pc` and returns `RL_PENDING`.
    fn undefined(&self, pc: u64, insn: u64) -> String;

    /// The C expression of the exclusive monitor the `rl_ldx` family takes.
    fn monitor(&self) -> &'static str;

    /// The C expression for floating-point operation `op` from operands of
    /// width `from` to a result of width `to`, over the C expressions
    /// `args`, with the guest's own rounding, NaN and saturation rules;
    /// `reg` names the C local of a register in [`Frontend::fp_state`].
    /// `None` leaves the operation to the emitter's generic form, which only
    /// exists for sign operations ([`FpOp::Neg`], [`FpOp::Abs`]).
    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        reg: &dyn Fn(Reg) -> String,
    ) -> Option<String>;

    /// The C expression for cipher step `op` on the C expression `arg`, or
    /// `None` when the runtime has none (the emitter then refuses the
    /// block).
    fn crypto(&self, op: CryptoOp, arg: &str) -> Option<String> {
        let _ = (op, arg);
        None
    }

    /// The C expression for [`relift_ir::Op::Pac`] on the C expressions of
    /// its data, modifier and key halves, or `None` when the runtime has
    /// none (the emitter then refuses the block).
    fn pac(&self, args: [&str; 4]) -> Option<String> {
        let _ = args;
        None
    }

    /// The C lvalue of the guest cycle clock, an `rl_time` (docs/abi.md
    /// section 13), such as `c->time`; `None` for a CPU without one, whose
    /// blocks hold no tick, I/O access or sync point (the emitter reports
    /// each as an error).
    fn clock(&self) -> Option<&'static str> {
        None
    }

    /// The C lvalue of the preemption clock, an `rl_time` whose budget
    /// another host thread ends to preempt the guest thread (docs/abi.md
    /// section 14.1), such as `c->time`; the emitter then checks it at the
    /// jumps back of loops that call, hold another loop or wait on memory, at computed
    /// jumps and at recursive calls. `None` for a CPU whose threads are not
    /// preempted.
    fn preempt_clock(&self) -> Option<&'static str> {
        None
    }

    /// How lifted returns check their target (docs/abi.md section 5.4):
    /// a lifted function returns the guest address its return goes to, and
    /// a call site goes on only when that is the call's return address;
    /// anything else continues at the target through the runtime. `None`
    /// returns `RL_OK` to the host caller whatever the target.
    fn return_link(&self) -> Option<ReturnLink> {
        None
    }

    /// The C functions a device read and a device write call, with the
    /// signatures of `rl_io_read` and `rl_io_write` (docs/abi.md section
    /// 13), which are the default. A CPU layer that drives its devices
    /// itself names its own.
    fn io_calls(&self) -> (&'static str, &'static str) {
        ("rl_io_read", "rl_io_write")
    }

    /// Whether guest RAM holds the guest's big-endian data as host-order
    /// 32-bit words, the layout N64 emulators and parallel-rdp give RDRAM:
    /// a byte at its address XOR 3, a halfword at its address XOR 2, a
    /// doubleword as its two words in address order. A big-endian access of
    /// 2, 4 or 8 bytes is then naturally aligned and needs no byte swap; one
    /// of another size goes byte by byte. Device accesses keep the guest's
    /// addresses and byte order.
    fn ram_words(&self) -> bool {
        false
    }

    /// The `int` C call of the sync point of `b`, whose exit goes to the
    /// guest address `next` (a C expression); `None` keeps
    /// `rl_sync(c, mem, next)`. The call returns `RL_OK` or `RL_PENDING`
    /// like `rl_sync`.
    fn sync_call(&self, b: &Block, next: &str) -> Option<String> {
        let _ = (b, next);
        None
    }

    /// The registers [`Frontend::fp`] expressions read or write (the FP
    /// control and status registers).
    fn fp_state(&self) -> Vec<Reg>;

    /// The register whose being nonzero is exactly the condition of guards
    /// with slow-path `code`, so the emitter may test it once per function
    /// entry, call return and write instead of per instruction; `None` keeps
    /// the guard's own condition.
    fn guard_state(&self, code: u32) -> Option<Reg>;

    /// The `int` C expression that is the hoisted guard condition of
    /// [`Frontend::guard_state`] register `r`, whose value is the C
    /// expression `value`: nonzero by default. A frontend may let the build
    /// decide the test with a macro.
    fn guard_flag(&self, r: Reg, value: &str) -> String {
        let _ = r;
        format!("{value} != 0")
    }

    /// An `int` C expression that runs the instruction `insn` at `pc` on the
    /// runtime's slow path `code`, with the context fully stored, and
    /// returns `RL_PENDING` when it could not; `None` when the frontend has
    /// no such path (the emitter then reports the instruction as undefined).
    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String>;

    /// An `rl_fn` C expression for guards with slow-path `code` that leave
    /// the function instead: with the context fully stored, the lifted
    /// function tail-calls the function it gives, which runs the guest from
    /// the instruction `insn` at `pc` on in the same host frame. `None`
    /// keeps [`Frontend::guard_slow`].
    fn guard_exit(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        let _ = (code, pc, insn);
        None
    }
}

/// What a module section holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SectionKind {
    /// Executable code.
    Code,
    /// Read-only data.
    ReadOnly,
    /// Writable data; the part past the image is zero-filled.
    Data,
}

/// One section of a loaded module.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Section {
    /// The guest address.
    pub addr: u64,
    /// The size in bytes.
    pub size: u64,
    /// What it holds.
    pub kind: SectionKind,
}

/// Why an address is a function entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum RootKind {
    /// The module's entry point.
    Entry,
    /// A defined function symbol.
    Symbol,
    /// The start of an unwind-table entry (an `.eh_frame` FDE).
    Unwind,
    /// A code address stored in data by a relocation.
    Pointer,
    /// A code address a dev run reached that analysis had not found (a
    /// hot-load hint).
    Hint,
}

/// A function entry the guest knows before analysis.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Root {
    /// The guest address.
    pub addr: u64,
    /// Why it is an entry.
    pub kind: RootKind,
    /// The symbol name, when there is one.
    pub name: Option<String>,
}

/// A landing pad of an unwind table: where the unwinder resumes a function
/// when an exception passes through a call.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Pad {
    /// The first address of the call-site range the pad serves.
    pub site: u64,
    /// The length of the call-site range in bytes.
    pub len: u64,
    /// The landing pad.
    pub addr: u64,
}

/// One guest module as analysis sees it: its bytes at their load address,
/// its sections, and the entries the guest's formats name.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Module {
    /// The module name.
    pub name: String,
    /// The load address of `image[0]`.
    pub base: u64,
    /// The loaded bytes from `base` (without zero-filled data).
    pub image: Vec<u8>,
    /// The sections.
    pub sections: Vec<Section>,
    /// The known entries.
    pub roots: Vec<Root>,
    /// The landing pads of the unwind tables, by call-site address.
    pub pads: Vec<Pad>,
}

impl Module {
    /// The bytes at guest address `addr` to the end of the image, if `addr`
    /// lies inside it.
    pub fn bytes_at(&self, addr: u64) -> Option<&[u8]> {
        let off = usize::try_from(addr.checked_sub(self.base)?).ok()?;
        self.image.get(off..)
    }

    /// Whether `addr` lies in a code section.
    pub fn is_code(&self, addr: u64) -> bool {
        self.sections
            .iter()
            .any(|s| s.kind == SectionKind::Code && addr >= s.addr && addr - s.addr < s.size)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn module_bytes_and_code_ranges() {
        let m = Module {
            name: "m".into(),
            base: 0x1000,
            image: vec![1, 2, 3, 4, 5, 6, 7, 8],
            sections: vec![Section {
                addr: 0x1000,
                size: 4,
                kind: SectionKind::Code,
            }],
            roots: Vec::new(),
            pads: Vec::new(),
        };
        assert_eq!(m.bytes_at(0x1006), Some(&[7u8, 8][..]));
        assert_eq!(m.bytes_at(0xfff), None);
        assert!(m.is_code(0x1003));
        assert!(!m.is_code(0x1004));
    }
}
