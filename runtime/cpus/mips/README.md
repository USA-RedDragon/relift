# runtime/cpus/mips

The VR4300 CPU layer (the Nintendo 64 CPU): the lifted-code ABI
(`include/relift/mips/abi.h`), CP0 and exceptions (`mips.c`), the TLB and
address translation (`tlb.c`), the FPU slow path (`fpu.c`, `softfloat.c`),
the guard slow paths (`guard.c`) and the run loop. `../../README.md` has the
full description; the frontend is the `vr4300` profile of `cpus/mips`.

## An R3000A layer (planned)

The `r3000a` profile of `cpus/mips` (the PlayStation CPU) is a stub today
and has no runtime layer. Its layer would reuse the shape of this one (the
descriptor for `rl_core_init`, the run loop with redirects, the guard slow
paths, the return address stack of lifted calls) and differ in:

- **Context.** A separate `rl_ctx` and ABI header in the R3000A slot order:
  32-bit general registers, HI and LO, the two load-delay slots (pending
  register and value, see `cpus/mips/README.md`), the R3000A COP0 registers
  and the GTE's 32 data and 32 control registers. No FPU and no TLB, so no
  `fpu.c`, `softfloat.c` or `tlb.c`.
- **COP0.** Status with the three-level KU/IE stack that exception entry
  pushes and RFE pops (no ERET, no EXL or ERL), Cause with BD and the IP
  bits, EPC, BadVaddr and the debug registers (BPC, BDA, JUMPDEST, DCIC,
  BDAM, BPCM); vectors at 0x80000080 and, with BEV, 0xBFC00180. No Count,
  Compare or Random: timers belong to the guest's devices.
- **Translation.** Fixed segments only: KUSEG, KSEG0 and KSEG1 mirror the
  same physical space (KSEG2 holds the cache control register), with the
  address-error checks of user mode. The scratchpad (the data cache used as
  1 KiB of fast RAM) and the cache isolation bit of Status are the guest's
  memory map, reached through I/O accesses like any other device.
- **GTE.** COP2 as a library of entries: MTC2, MFC2, CTC2, CFC2, LWC2 and
  SWC2 move data, and the command word runs the fixed-point operations (RTPS,
  RTPT, NCLIP, MVMVA and the rest) with FLAG saturation. Lifted code would
  call these as intrinsics; the operations are pure functions of the GTE
  registers, so they can be tested without a machine.
- **Timing.** A cycle charge per instruction plus the bus costs the guest's
  memory map gives, with the multiply and divide latencies of the R3000A
  (HI and LO stall the reader until the result is ready).
- **Byte order.** Little-endian data.
