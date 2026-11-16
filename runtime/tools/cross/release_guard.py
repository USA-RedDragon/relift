#!/usr/bin/env python3
"""Checks that a release build loads no code at run time.

  python3 -I release_guard.py [--sources DIR] [--objects DIR]... [--binary FILE]...

- sources: no file under DIR (the runtime, tests excluded) maps executable
  memory (PROT_EXEC, MAP_JIT, PAGE_EXECUTE*, FILE_MAP_EXECUTE) or calls
  dlopen, LoadLibrary or adrenotools_open_libvulkan, except hostlib.c and its
  header (host libraries such as the Vulkan loader) and devload.c (the
  hot-load path, built only with RL_HOTLOAD).
- objects: no object file under DIR holds the hot-load connection
  (RELIFT_DEV_SOCKET), and only hostlib objects import dlopen, LoadLibrary or
  adrenotools_open_libvulkan.

A release loads no game code at run time. What hostlib.c loads are GPU and
system libraries: the host's Vulkan loader, and on Android the Vulkan driver
package the user installed and picked with video.gpu_driver, which
libadrenotools opens next to the system loader (core/gpu_driver.h).
- binary: ELF, PE or Mach-O with no hot-load connection and no segment or
  section that is both writable and executable.
- no-interpreter (with binary): the binary holds none of the names of the
  Game Boy device library's CPU interpreter (rlgbd_exec_native,
  rlgbd_step_native, rustyboi's cpu::opcodes and step_instruction), which a
  release build leaves out: every instruction it runs is lifted. A stripped
  binary has no names to find, so the check is for unstripped builds.

Exit status 0 when every check passes. Standard library only.
"""

import argparse
import os
import re
import struct
import sys

EXEC_API = re.compile(rb"\b(PROT_EXEC|MAP_JIT|PAGE_EXECUTE\w*|FILE_MAP_EXECUTE|VirtualProtect\w*Exec\w*)\b")
LOAD_API = re.compile(rb"\b(dlopen|LoadLibrary[AWX]?(?:Ex[AW]?)?|LoadPackagedLibrary|adrenotools_open_libvulkan)\b")
ALLOWED_LOAD = {"hostlib.c", "hostlib.h", "devload.c"}
HOTLOAD = b"RELIFT_DEV_SOCKET"
INTERPRETER = (b"rlgbd_exec_native", b"rlgbd_step_native", b"3cpu7opcodes", b"16step_instruction")


def scan_sources(root):
    bad = []
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in ("tests", "devtests", "build", "gen")]
        for fn in fns:
            if not fn.endswith((".c", ".h", ".S", ".inc")):
                continue
            path = os.path.join(dp, fn)
            with open(path, "rb") as f:
                data = f.read()
            for m in EXEC_API.finditer(data):
                bad.append(f"{path}: maps executable memory ({m.group(0).decode()})")
            if fn not in ALLOWED_LOAD:
                for m in LOAD_API.finditer(data):
                    line = data.count(b"\n", 0, m.start()) + 1
                    prefix = data[max(0, m.start() - 4):m.start()]
                    if prefix.endswith(b"_"):
                        continue
                    bad.append(f"{path}:{line}: {m.group(0).decode()} outside hostlib.c")
    return bad


def scan_objects(root):
    bad = []
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if not fn.endswith((".o", ".obj")):
                continue
            path = os.path.join(dp, fn)
            with open(path, "rb") as f:
                data = f.read()
            if HOTLOAD in data:
                bad.append(f"{path}: holds the hot-load connection")
            if "hostlib" not in fn and re.search(rb"(?<![A-Za-z_])(dlopen|LoadLibrary[AW]|adrenotools_open_libvulkan)\b", data):
                bad.append(f"{path}: imports a library loader and is not hostlib")
    return bad


def wx_elf(data):
    if data[4] != 2:
        return []
    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    out = []
    for i in range(e_phnum):
        p_type, p_flags = struct.unpack_from("<II", data, e_phoff + i * e_phentsize)
        if p_type == 1 and p_flags & 1 and p_flags & 2:
            out.append(f"LOAD segment {i} is writable and executable")
    return out


def wx_pe(data):
    pe, = struct.unpack_from("<I", data, 0x3C)
    nsec, = struct.unpack_from("<H", data, pe + 6)
    opt, = struct.unpack_from("<H", data, pe + 20)
    base = pe + 24 + opt
    out = []
    for i in range(nsec):
        o = base + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        ch, = struct.unpack_from("<I", data, o + 36)
        if ch & 0x20000000 and ch & 0x80000000:
            out.append(f"section {name} is writable and executable")
    return out


def wx_macho(data):
    magic, = struct.unpack_from("<I", data, 0)
    if magic != 0xFEEDFACF:
        return []
    ncmds, = struct.unpack_from("<I", data, 16)
    off = 32
    out = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 0x19:
            name = data[off + 8:off + 24].rstrip(b"\0").decode()
            maxprot, initprot = struct.unpack_from("<ii", data, off + 56)
            if initprot & 2 and initprot & 4:
                out.append(f"segment {name} is writable and executable")
        off += size
    return out


def scan_binary(path, no_interpreter=False):
    with open(path, "rb") as f:
        data = f.read()
    bad = []
    if HOTLOAD in data:
        bad.append(f"{path}: holds the hot-load connection")
    if no_interpreter:
        for name in INTERPRETER:
            if name in data:
                bad.append(f"{path}: holds the CPU interpreter ({name.decode()})")
    if data[:4] == b"\x7fELF":
        found = wx_elf(data)
    elif data[:2] == b"MZ":
        found = wx_pe(data)
    elif data[:4] in (b"\xcf\xfa\xed\xfe",):
        found = wx_macho(data)
    else:
        return bad + [f"{path}: not an ELF, PE or 64-bit Mach-O file"]
    return bad + [f"{path}: {w}" for w in found]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sources", action="append", default=[])
    ap.add_argument("--objects", action="append", default=[])
    ap.add_argument("--binary", action="append", default=[])
    ap.add_argument("--no-interpreter", action="store_true", help="also check the binaries hold no CPU interpreter")
    args = ap.parse_args()
    bad = []
    for d in args.sources:
        bad += scan_sources(d)
    for d in args.objects:
        bad += scan_objects(d)
    for b in args.binary:
        bad += scan_binary(b, args.no_interpreter)
    for b in bad:
        print(f"release guard: {b}", file=sys.stderr)
    checked = len(args.sources) + len(args.objects) + len(args.binary)
    if not checked:
        sys.exit("release guard: nothing to check")
    if bad:
        sys.exit(1)
    print(f"release guard: {checked} inputs clean")


if __name__ == "__main__":
    main()
