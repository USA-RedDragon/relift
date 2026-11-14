# Native oracle harness

`harness.c` runs A64 instruction words on a real AArch64 CPU, or under QEMU
user mode, and reports the state after them, so `relift-check` can compare
lifted code against hardware instead of Unicorn alone. Unicorn and QEMU use
the same softfloat code, so only hardware settles an FP disagreement between
them.

Build both binaries with zig (no Xcode or macOS SDK needed):

    check/oracle/build.sh <path to zig> <out dir>

which writes `harness-linux` (static, AArch64 Linux) and `harness-macos`.

Run the checker against them:

    # QEMU user mode as the Switch's Cortex-A57, or QEMU's max CPU
    RELIFT_CHECK_HARNESS_LINUX=<out>/harness-linux relift-check forms all --oracle qemu
    RELIFT_CHECK_HARNESS_LINUX=<out>/harness-linux relift-check forms all --oracle qemu-max

    # an Apple silicon Mac over ssh (host from RELIFT_CHECK_SSH, default mac)
    scp <out>/harness-macos mac:<dir>/harness
    RELIFT_CHECK_HARNESS_REMOTE=<dir>/harness relift-check forms all --oracle m2

`--oracle cmd:<argv>` runs any other command that speaks the protocol. One
child process serves the whole run; the checker streams every state of a
form before it reads the results, so an ssh round trip is paid once per
form, not once per state.

How a run works:
- The code region (2 MiB around `CODE`) and the data window sit at the
  addresses in `check/src/emu.rs`, above 0x70_0000_0000 because macOS
  refuses fixed mappings below that.
- Entry: the harness executes a BRK; the SIGTRAP handler writes the whole
  guest state (x0-x30, SP, PC, NZCV, FPCR, FPSR, v0-v31) into the signal
  context, and the return from the handler starts the words with exactly
  that state.
- Exit: the zero word after the words is `UDF #0`. Its SIGILL handler, or
  the fault handler for a fetch from an unmapped address after a branch,
  reads the state back from the signal context and leaves with siglongjmp.
  Handlers run on an alternate stack because the guest SP points into the
  data window. A branch to itself ends on a 50 ms timer.
- When the last word is a branch or system instruction, the words before
  it run first and the last word then runs alone, so a branch back into
  the sequence ends on a zero word. Other sequences run in one go, which
  keeps the exclusive monitor.
- SVC, HVC and SMC are refused, since they would reach the host kernel.

Known limits:
- macOS zeroes x18 on every return to user mode, so with `--oracle m2`
  states start with x18 zero and checks ignore x18.
- QEMU must run without SVE and SME (`--oracle qemu-max` passes
  `sve=off,sme=off`): with live SVE state the kernel restores the Z
  registers from the signal frame, not the V registers. The harness exits
  with a message if it sees that.
- A store to a random address that happens to be mapped in the harness
  itself would corrupt it; the generated states only point base registers
  at the data window.
