# NanoBoyAdvance patches for the relift GBA oracle

Against NanoBoyAdvance e8482c1efc6cde3f553e8170a51abea71cee426e (codeberg). `check/oracle/nba/build.sh` applies them in name order (set
`RELIFT_NBA_PATCHES=` empty for stock NBA). Each patch is described by the hardware behavior and the
console evidence from an AGB-001 running relift's CPU probe ROMs: sb1 is a single-boot run of
262,144 instruction records and 365 timing records, p2 a follow-up run of 4,096 records, batch 1 an
earlier run of 1,000 full records and 320 timing records.

## 0001-test-ops-with-rd-pc.patch

Hardware: TST/TEQ/CMP/CMN with Rd=PC and S=1 (the ARM "P" forms) and Thumb format 5 CMP with
H1=1, Rd=PC (0x4587-0x45FF) compute no flags. In a mode with an SPSR they copy the SPSR into the
CPSR and switch the register bank (Thumb keeps the T bit set); in user and system mode the CPSR is
unchanged. PC is never written.

Stock NBA: computes the flags in every mode (Thumb), and for the ARM forms copies SPSR in SPSR
modes but in usr/sys copies the CPSR read through the SPSR alias after the flags were computed, so
the computed flags stay.

Evidence: console single-boot run sb1 (284 Thumb CMP Rd=PC records, rule derived there) and the
follow-up probe p2 (4,096 records: ARM P forms in all modes incl. usr, Thumb hi-reg Rd=PC ops):
console 4096/4096 match the rule. Stock NBA on p2: 1,966 match, 2,130 differ (all five classes);
patched: 4096/4096. sb1 T hi-reg op: stock 13151/0 (agrees with the old table, not the console),
patched 12867/284 = the console's counts.

Unverified: ARM P form with SPSR.T=1 (the probe's SPSRs have T=0 for ARM records); the patch copies
the SPSR as is.

## 0002-prefetch-stalls-when-full.patch

Hardware: when the Game Pak prefetch buffer fills (8 halfwords, 4 ARM opcodes), the prefetcher
stops and does not resume until the CPU has taken every entry; the next opcode fetch after the
buffer ran empty is a non-sequential cartridge access, paid in full by the CPU.

Stock NBA: a full buffer refills an entry for free as soon as the CPU takes one (the countdown
stays expired, so the next Step adds an entry without a fetch), so code in ROM that spends idle
cycles (MUL, LDM/STM to IWRAM) never stalls on the cartridge.

Evidence: sb1 timing kernels, code in ROM, data in IWRAM, prefetch on: a_mul4, a_ldm4, a_stm4 at
ws0 2/1, 3/1, 4/1, 8/1. Console minus stock NBA per 64-iteration run: 259/319/259, 323/383/323,
387/447/387, 643/703/643 cycles, i.e. 64 x (N+2) about once per loop iteration plus one more
access for LDM. Patched NBA: all 12 records equal the console exactly; 358 of 365 records equal
(stock 346), no class or record regresses.

## 0003-psr-reserved-bits-read-zero.patch

Hardware: MSR writes only the flag bits 31:28 and the control byte 7:0 of the CPSR and SPSR; bits
27:8 read back as zero whatever was written.

Stock NBA: stores every bit the field mask selects (x and s fields write bits 23:8, f writes
27:24), so MRS later returns them.

Evidence: console batch 1 (`decoded/batch-0001.json`, full end states): 10 MSR records (5 MSR
CPSR, 5 MSR SPSR) where stock NBA holds e.g. SPSR 0x90968874 / 0x023b36ff / CPSR 0x00ce0051 and
the console 0x90000074 / 0x000000ff / 0x00000051. Patched: batch 1 1000 of 1000 records equal the
console (stock 990). The single-boot runs mask these bits (the model calls them unknown), so they
do not see this.

## Open

- Thumb kernels at ws0 3/1 with prefetch on, code in ROM (cfg 6: t_empty, t_nop, t_add, t_b,
  t_bnt, t_ldr, t_str): NBA is 2 cycles slower than the console, the same for every kernel, so it
  is a one-time cost at kernel entry or exit (ARM `bx r12` from IWRAM into Thumb ROM code, `bx lr`
  back), not per instruction. ws0 2/1, 4/1, 8/1 and 4/2 match. Candidates: the state the prefetch
  unit is in when the CPU branches from IWRAM into ROM (the caller is Thumb code in ROM, its
  prefetch buffer fills while time_kernel runs in IWRAM); how a branch into ROM ends a burst that
  is in progress at the 4-cycle N/2-cycle S setting. Needs a probe that times entry and exit
  separately.

## Test

    ./test.sh <nba-oracle> <work dir>

runs the three probe ROMs through the real BIOS (about 80 s): p2 and sb1 compared per class and per
timing record, batch 1 per record (full end state) and per timing record, with the console
decodes; PASS when nothing disagrees except `known.txt`. Stock NBA fails it (p2: five classes; sb1:
T hi-reg op and 19 timing records; batch 1: 10 MSR records and 16 timing records). Patched with
0001-0003: PASS (p2 4096/4096, sb1 all 52 classes and 358/365 timing, batch 1 1000/1000 and
313/320 timing; the misses are the open cfg 6 Thumb records).
