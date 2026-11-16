# runtime/cpus/rsp

The RSP layer (the Nintendo 64 signal processor): the lifted-code ABI
(`include/relift/rsp/abi.h`) and the run of lifted microcode out of IMEM
(`rsp.c`, `rsp.h`). The frontend is `cpus/rsp`, reshaped for this layer by
`guests/n64/guest/src/rsp.rs`; `docs/abi.md` (section 13, the RSP) has the
contract.

The layer knows no machine. The guest layer gives it DMEM, IMEM, the lifted
images and a bus for COP0 and BREAK, and runs it in slices with
`rl_rsp_run`. In the N64 program (`guests/n64/run/cpu.c`) the machine's SP
interface (`guests/n64/sp.c`) brings the RSP up to the CPU's time before
every CPU access outside RDRAM and at a slice event while it runs.

- **Images.** At most 4 KiB of RSP code each, lifted at its own offsets so
  it runs at any IMEM address: the unit, the length, the bytes and three
  successor entries per word (the words its block goes to without a
  computed jump, and whether it holds a delay slot).
- **Binding.** Every IMEM word has a dispatch slot. At an unbound word every
  image word equal to IMEM's there is a candidate (image, base); a slot
  holds lifted code, and its image's base, only after the layer checked
  that IMEM holds the image's bytes at that base at every word that code
  can reach without a computed jump. A write to IMEM (an SP
  DMA) puts the check back in every slot and ends the running slice at its
  next block.
- **Missed code.** A word no image's check accepts stops the program with a
  report: the IMEM address and word, a hash of IMEM, the registers, and
  with `--dump` the IMEM and DMEM contents. Nothing is interpreted.
- **Time.** `c->time` counts RSP cycles. Each instruction costs one cycle
  today (a branch and its delay slot two); stalls are not modeled yet.

```sh
make -C runtime test    # rsp_tests among them
```
