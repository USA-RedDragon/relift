#!/usr/bin/env python3
"""gen.py OUT: writes the timing kernels and case tables of the timing test ROM into OUT.

k9.s, k7.s: one position-independent ARM function per kernel,
  void kN(unsigned n, unsigned base, unsigned base2), running its body n times
  (r4 = base, r3 = base2; r5 = 1, r6 = r8 = 0, r7 = 1, r9 = the multiplier operand or a
  branch target, r10 = a pointer to two copies of the branch target).
k9tab.h, k7tab.h: the kernel start and end tables.
cases.h: the cases of both CPUs (kernel, data region, code placement, cache mode,
  MPU attribute override, and whether the result may differ on DSi or 3DS in DS mode)."""
import os
import sys

OUT = sys.argv[1]

# regions (shared/hwtest.h has the same numbering)
R = {"none": 0, "dtcm": 1, "itcm": 2, "main": 3, "unc": 4, "io": 5, "vram": 6, "pal": 7, "oam": 8,
     "wram7": 9, "mx": 10, "m2w": 11}
PLACE = {"main": 0, "itcm": 1, "unc": 2, "wram": 3}
MODE = {"warm": 0, "cold": 1, "drain": 2, "colddrain": 3}
ATTR = {"": 0, "cb11": 1, "cb10": 2, "cb01": 3, "cb00": 4}

ALU = [("add", "add r6, r6, r5"), ("add_lsl", "add r6, r6, r5, lsl #2"), ("add_lslr", "add r6, r6, r5, lsl r7"),
       ("mov", "mov r6, r5"), ("adds", "adds r6, r6, r5"), ("condfail", "addvs r6, r6, r5"),
       ("mrs", "mrs r6, cpsr"), ("msr_f", "msr cpsr_f, r8")]
MUL = [("mul", "mul r6, r5, r9"), ("muls", "muls r6, r5, r9"), ("mla", "mla r6, r5, r9, r6"),
       ("mlas", "mlas r6, r5, r9, r6"), ("umull", "umull r6, r8, r5, r9"), ("smull", "smull r6, r8, r5, r9"),
       ("umlal", "umlal r6, r8, r5, r9"), ("smlal", "smlal r6, r8, r5, r9"), ("smulls", "smulls r6, r8, r5, r9")]
MUL9 = [("smulbb", "smulbb r6, r5, r9"), ("smlabb", "smlabb r6, r5, r9, r6"), ("smlalbb", "smlalbb r6, r8, r5, r9"),
        ("smulwb", "smulwb r6, r5, r9"), ("clz", "clz r6, r5"), ("qadd", "qadd r6, r5, r7")]
USE = [("ldr_use", "ldr r6, [r4]\n add r7, r6, #1"), ("ldr_use2", "ldr r6, [r4]\n add r8, r8, #1\n add r7, r6, #1"),
       ("ldrb_use", "ldrb r6, [r4]\n add r7, r6, #1"), ("ldr_mul", "ldr r6, [r4]\n mul r8, r6, r7"),
       ("mul_use", "mul r6, r5, r7\n add r8, r6, #1"), ("mul_mul", "mul r6, r5, r7\n mul r8, r6, r7")]
BR = [("b", "b 2f\n2:"), ("bl", "mov r11, lr\n bl 3f\n mov lr, r11\n b 2f\n3: bx lr\n2:"),
      ("bx_arm", "bx r9\n2:"), ("mov_pc", "mov pc, r9\n2:"), ("ldr_pc", "ldr pc, [r10]\n2:"),
      ("pop_pc", "push {r9}\n pop {pc}\n2:"), ("ldm_pc", "ldmia r10, {r8, pc}\n2:"),
      ("bx_thumb", "bx r11\n2:"), ("bne_nt", "cmp r0, #0\n beq 2f\n2:")]
MEM = [("ldr", "ldr r6, [r4]"), ("ldrh", "ldrh r6, [r4]"), ("ldrb", "ldrb r6, [r4]"), ("ldrsh", "ldrsh r6, [r4]"),
       ("str", "str r6, [r4]"), ("strh", "strh r6, [r4]"), ("strb", "strb r6, [r4]"), ("swp", "swp r6, r6, [r4]"),
       ("ldr2", "ldr r6, [r4]\n ldr r7, [r4, #4]"), ("str2", "str r6, [r4]\n str r7, [r4, #4]"),
       ("str_ldr", "str r6, [r4]\n ldr r7, [r4, #4]"), ("ldr_str", "ldr r6, [r4]\n str r7, [r4, #4]"),
       ("ldr_alu", "ldr r6, [r4]\n add r8, r8, r5"), ("str_alu", "str r6, [r4]\n add r8, r8, r5"),
       ("str_alu4", "str r6, [r4]" + "\n add r8, r8, r5" * 4), ("str_alu16", "str r6, [r4]" + "\n add r8, r8, r5" * 16)]
MEM9 = [("ldrd", "ldrd r6, r7, [r4]"), ("strd", "strd r6, r7, [r4]")]
for k in (1, 2, 4, 6, 8):
    rl = "r6" if k == 1 else f"r6-r{5 + k}" if 5 + k <= 12 else "r6-r12"
    if k == 8:
        rl = "r5-r12"
    MEM += [(f"ldm{k}", f"ldmia r4, {{{rl}}}"), (f"stm{k}", f"stmia r4, {{{rl}}}")]
STREAM = [("s_ldr4", "ldr r6, [r4], #4"), ("s_ldr32", "ldr r6, [r4], #32"), ("s_str4", "str r6, [r4], #4"),
          ("s_str32", "str r6, [r4], #32"), ("s_ldm8", "ldmia r4!, {r5-r12}"), ("s_stm8", "stmia r4!, {r5-r12}"),
          ("copy", "ldr r6, [r4], #4\n str r6, [r3], #4")]
THUMB = [("t_alu", "adds r6, r6, r5"), ("t_mul", "muls r6, r5, r6"), ("t_ldr", "ldr r6, [r4]"),
         ("t_str", "str r6, [r4]"), ("t_ldr_use", "ldr r6, [r4]\n adds r7, r6, #1"),
         ("t_push", "push {r6, r7}\n pop {r6, r7}"), ("t_b", "b 2f\n2:"),
         ("t_bl", "mov r3, lr\n bl 3f\n mov lr, r3\n b 2f\n3: bx lr\n2:")]
MULV = ["0x1", "0xFF", "0x100", "0xFFFF", "0x10000", "0xFFFFFF", "0x1000000", "0x7FFFFFFF", "0xFFFFFFFF", "0xFFFFFF00"]


class Kernels:
    def __init__(self):
        self.k = []
        self.index = {}

    def add(self, key, body, thumb=False, mulv="0x1", pad=None):
        if key in self.index:
            return self.index[key]
        self.index[key] = len(self.k)
        self.k.append((body, thumb, mulv, pad))
        return self.index[key]

    def asm(self, prefix):
        L = [".syntax unified", ".text"]
        for i, (body, thumb, mulv, pad) in enumerate(self.k):
            n = f"{prefix}{i}"
            L += [".arm", ".align 5", f".global {n}", f".type {n}, %function", f"{n}:",
                  " push {r4-r11, lr}", " mov r4, r1", " mov r3, r2", " mov r5, #1", " mov r6, #0", " mov r7, #1",
                  " mov r8, #0", f" ldr r9, ={mulv}"]
            if "2:" in body and not thumb:
                L += [" adr r9, 2f", " sub sp, sp, #8", " str r9, [sp]", " str r9, [sp, #4]", " mov r10, sp",
                      " adr r11, 7f", " orr r11, r11, #1"]
            if pad is not None:
                L += [" b 5f", ".align 5", f".space {4 * pad}", "5:"]
            if thumb:
                L += [" adr r12, 6f", " orr r12, r12, #1", " bx r12", ".thumb", ".align 2", "6:",
                      f"1:\n {body}\n subs r0, r0, #1\n bne 1b", ".align 2", " bx pc", " nop", ".arm"]
            else:
                L += [f"1:\n {body}\n subs r0, r0, #1\n bne 1b"]
            if "2:" in body and not thumb:
                L += [" add sp, sp, #8"]
            L += [" pop {r4-r11, lr}", " bx lr"]
            if "2:" in body and not thumb:
                L += [".thumb", ".align 2", "7: bx pc", " nop", ".arm", " b 2b"]
            L += [".ltorg", f".global {n}_end", f"{n}_end:"]
        return "\n".join(L) + "\n"

    def tab(self, prefix):
        n = len(self.k)
        d = "\n".join(f"void {prefix}{i}(unsigned, unsigned, unsigned); extern const char {prefix}{i}_end[];"
                      for i in range(n))
        return (f"{d}\nstatic void (*const {prefix}fn[])(unsigned, unsigned, unsigned) = {{"
                + ", ".join(f"{prefix}{i}" for i in range(n)) + "};\n"
                + f"static const char *const {prefix}end[] = {{" + ", ".join(f"{prefix}{i}_end" for i in range(n))
                + "};\n")


def risky(region, place, attr):
    return int(region in ("main", "unc", "mx", "m2w") or place in ("main", "unc") or attr != "")


cases = []  # (cpu, name, kernel, region, place, mode, attr)
k9, k7 = Kernels(), Kernels()


def c9(name, body, region="none", place="main", mode="warm", attr="", thumb=False, mulv="0x1", pad=None):
    ki = k9.add((body, thumb, mulv, pad), body, thumb, mulv, pad)
    cases.append((9, name, ki, region, place, mode, attr))


def c7(name, body, region="none", place="wram", mode="warm", thumb=False, mulv="0x1"):
    ki = k7.add((body, thumb, mulv, None), body, thumb, mulv)
    cases.append((7, name, ki, region, place, mode, ""))


# ARM9
for place in ("main", "itcm", "unc"):
    c9("empty", "mov r0, r0", place=place)
    for nm, b in ALU + MUL + MUL9 + BR:
        if place != "main" and nm not in ("add", "mul", "b", "bl", "ldr_pc", "add_lslr"):
            continue
        c9(nm, b, place=place)
    for nm, b in THUMB:
        if "[r4]" in b:
            continue
        if place != "main" and nm not in ("t_alu", "t_b"):
            continue
        c9(nm, b, place=place, thumb=True)
for nm, b in USE:
    c9(nm, b, "dtcm")
    c9(nm, b, "main")
for rg in ("dtcm", "itcm", "main", "unc", "io", "vram", "pal", "oam"):
    for nm, b in MEM + MEM9:
        if rg == "io" and nm.startswith("swp"):
            continue
        c9(nm, b, rg)
    for nm, b in THUMB:
        if "[r4]" in b or "push" in b:
            c9(nm, b, rg, thumb=True)
for rg in ("dtcm", "main", "unc"):
    for nm, b in STREAM:
        c9(nm, b, rg)
        if rg == "main":
            c9(nm, b, rg, mode="cold")
for nm, b in [("ldr", "ldr r6, [r4]"), ("ldr_use", "ldr r6, [r4]\n add r7, r6, #1"), ("ldm8", "ldmia r4, {r5-r12}")]:
    c9(nm, b, "main", mode="cold")
for at in ("cb11", "cb10", "cb01", "cb00"):
    for nm, b in [("str", "str r6, [r4]"), ("strh", "strh r6, [r4]"), ("stm8", "stmia r4, {r5-r12}"),
                  ("str_alu4", "str r6, [r4]" + "\n add r8, r8, r5" * 4),
                  ("str_alu16", "str r6, [r4]" + "\n add r8, r8, r5" * 16),
                  ("s_str4", "str r6, [r4], #4"), ("s_str32", "str r6, [r4], #32"), ("s_stm8", "stmia r4!, {r5-r12}"),
                  ("ldr", "ldr r6, [r4]"), ("s_ldr32", "ldr r6, [r4], #32"), ("str_ldr", "str r6, [r4]\n ldr r7, [r4, #4]")]:
        for md in ("warm", "drain", "cold", "colddrain"):
            if md.startswith("cold") and not nm.startswith(("s_", "ldr")):
                continue
            c9(nm, b, "mx", mode=md, attr=at)
for v in MULV:
    for nm, b in MUL[:4] + MUL[4:6]:
        c9(f"{nm}={v}", b, mulv=v)
for pad in (0, 5, 6, 7):
    for nm, b in [("add", "add r6, r6, r5"), ("b", "b 2f\n2:"), ("ldr", "ldr r6, [r4]")]:
        c9(f"{nm}+{pad}", b, "dtcm", pad=pad)
for pad in (0, 13, 14, 15):
    for nm, b in [("t_alu", "adds r6, r6, r5"), ("t_b", "b 2f\n2:")]:
        c9(f"{nm}+{pad}", b, pad=pad, thumb=True)
for kb in (2, 4, 6, 7, 8, 9, 12, 16):
    c9(f"big{kb}k", "\n".join(["add r6, r6, r5"] * (kb * 256 - 8)))
    c9(f"big{kb}k", "\n".join(["add r6, r6, r5"] * (kb * 256 - 8)), mode="cold")

# ARM7
c7("copy_m2w", "ldr r6, [r4], #4\n str r6, [r3], #4", "m2w")
c7("copy_m2w", "ldr r6, [r4], #4\n str r6, [r3], #4", "m2w", place="main")
for place in ("wram", "main"):
    c7("empty", "mov r0, r0", place=place)
    for nm, b in ALU + MUL + BR:
        c7(nm, b, place=place)
    for nm, b in THUMB:
        if "[r4]" not in b and "push" not in b:
            c7(nm, b, place=place, thumb=True)
    for v in MULV:
        for nm, b in MUL[:4] + MUL[4:]:
            c7(f"{nm}={v}", b, place=place, mulv=v)
    for rg in ("main", "wram7", "io"):
        for nm, b in MEM + USE[:4]:
            if rg == "io" and nm.startswith("swp"):
                continue
            c7(nm, b, rg, place=place)
        for nm, b in THUMB:
            if "[r4]" in b or "push" in b:
                c7(nm, b, rg, place=place, thumb=True)
        if rg != "io":
            for nm, b in STREAM:
                c7(nm, b, rg, place=place)

open(os.path.join(OUT, "k9.s"), "w").write(k9.asm("k9_"))
open(os.path.join(OUT, "k7.s"), "w").write(k7.asm("k7_"))
open(os.path.join(OUT, "k9tab.h"), "w").write(k9.tab("k9_"))
open(os.path.join(OUT, "k7tab.h"), "w").write(k7.tab("k7_"))
with open(os.path.join(OUT, "cases.h"), "w") as f:
    f.write("/* generated by gen.py */\n")
    for cpu in (9, 7):
        cs = [c for c in cases if c[0] == cpu]
        f.write(f"#define NCASES{cpu} {len(cs)}\n")
        f.write(f"static const hw_case cases{cpu}[] = {{\n")
        for _, nm, ki, rg, pl, md, at in cs:
            f.write(f'    {{"{nm}", {ki}, {R[rg]}, {PLACE[pl]}, {MODE[md]}, {ATTR[at]}, {risky(rg, pl, at)}}},\n')
        f.write("};\n")
print("arm9", len(k9.k), "kernels", sum(c[0] == 9 for c in cases), "cases; arm7", len(k7.k), "kernels",
      sum(c[0] == 7 for c in cases), "cases")
