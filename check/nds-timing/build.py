#!/usr/bin/env python3
"""build.py OUT.nds [--probes]: the timing test cartridge, built with clang and ld.lld: h9.c and the ARM9
kernels at 0x02000000; h7.c and the ARM7 kernels linked at 0x037F8000 (shared work RAM, where the
SDK build runs them) and copied there by a boot stub at 0x02380000 with the BIOS's CpuFastSet. The
kernels and case tables come from gen.py, written next to OUT.nds."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = """ENTRY(_start)
SECTIONS {{
  . = {base:#x};
  .text : {{ *(.text.start) *(.text*) }}
  .rodata : {{ *(.rodata*) }}
  .data : {{ *(.data*) }}
  .bss : {{ *(.bss*) *(COMMON) }}
  /DISCARD/ : {{ *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }}
}}
"""
CFLAGS = ["-marm", "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib", "-fno-pic", "-fno-exceptions",
          "-fno-unwind-tables", "-fno-jump-tables", "-fno-asynchronous-unwind-tables", "-DHWTEST_BARE", "-I" + HERE, "-w"]


def binary(name, srcs, target, cpu, base, work, defs):
    objs = []
    for s in srcs:
        o = os.path.join(work, f"{name}-{os.path.basename(s)}.o")
        subprocess.run(["clang", f"--target={target}", f"-mcpu={cpu}", *CFLAGS, "-I" + work, *defs, "-c", s, "-o", o], check=True)
        objs.append(o)
    ld, elf, bn = (os.path.join(work, name + x) for x in (".ld", ".elf", ".bin"))
    open(ld, "w").write(SCRIPT.format(base=base))
    subprocess.run(["ld.lld", "-T", ld, "-o", elf, *objs], check=True)
    subprocess.run(["llvm-objcopy", "-O", "binary", "--set-section-flags", ".bss=alloc,load,contents", elf, bn],
                   check=True)
    return open(bn, "rb").read()


def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def put32(r, at, v):
    r[at:at + 4] = v.to_bytes(4, "little")


def main():
    out = os.path.abspath(sys.argv[1])
    a7base = 0x037F8000
    work = os.path.join(os.path.dirname(out), "work")
    os.makedirs(work, exist_ok=True)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "gen.py"), work] + sys.argv[2:3], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "gencopy.py"), work], check=True)
    a9 = binary("h9", [os.path.join(HERE, "h9.c"), os.path.join(work, "k9.s")], "armv5te-none-eabi",
                "arm946e-s", 0x02000000, work, ["-DARM9"])
    a7 = binary("h7", [os.path.join(HERE, "h7.c"), os.path.join(work, "k7.s")], "armv4t-none-eabi",
                "arm7tdmi", a7base, work, ["-DARM7"])
    # a boot stub in main RAM waits for the ARM9 to give it the shared work RAM, then copies the
    # ARM7 program there with the BIOS's CpuFastSet (constant source, destination and length)
    a7 += bytes(-len(a7) % 32)
    n7 = len(a7)
    stub = f"""
    ldr r0, =0x04000241
1:  ldrb r1, [r0]
    and r1, r1, #3
    cmp r1, #3
    bne 1b
    adr r0, 9f
    ldr r1, ={a7base:#x}
    ldr r2, ={n7 // 4:#x}
    swi 0x0c0000
    ldr r0, ={a7base:#x}
    bx r0
    .ltorg
    .balign 256
9:
"""
    sp = os.path.join(work, "stub.s")
    open(sp, "w").write(".syntax unified\n.arm\n.text\n" + stub)
    so, sb = os.path.join(work, "stub.o"), os.path.join(work, "stub.bin")
    se = os.path.join(work, "stub.elf")
    subprocess.run(["clang", "--target=armv4t-none-eabi", "-c", sp, "-o", so], check=True)
    subprocess.run(["ld.lld", "-Ttext=0x02380000", "-e", "0x02380000", "-o", se, so], check=True)
    subprocess.run(["llvm-objcopy", "-O", "binary", "-j", ".text", se, sb], check=True)
    stub_bytes = open(sb, "rb").read()
    assert len(stub_bytes) <= 0x100
    a7 = stub_bytes + bytes(0x100 - len(stub_bytes)) + a7
    load7 = 0x02380000
    r = bytearray(0x200)
    r[:12] = b"RELIFTHWTEST"
    r[0x0C:0x10] = b"HWTS"
    r[0x10:0x12] = b"00"
    off9 = len(r)
    r += a9
    r += bytes(-len(r) % 0x200)
    off7 = len(r)
    r += a7
    r += bytes(-len(r) % 0x200)
    for at, off, ram, ln in ((0x20, off9, 0x02000000, len(a9)), (0x30, off7, load7, len(a7))):
        put32(r, at, off)
        put32(r, at + 4, ram)
        put32(r, at + 8, ram)
        put32(r, at + 12, ln)
    r += bytes(max(0, 0x20000 - len(r)))
    put32(r, 0x80, len(r))
    put32(r, 0x84, 0x200)
    r[0x15C:0x15E] = crc16(r[0xC0:0xC0 + 156]).to_bytes(2, "little")
    r[0x15E:0x160] = crc16(r[:0x15E]).to_bytes(2, "little")
    open(out, "wb").write(bytes(r))
    print(out)


main()
