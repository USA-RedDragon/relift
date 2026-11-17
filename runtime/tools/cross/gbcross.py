#!/usr/bin/env python3
"""Cross-builds the C runtime and a Game Boy, Game Boy Advance or Nintendo 64
test program for one host target.

  python3 -I gbcross.py --target windows-x64 --relift BIN --work DIR [--guest gba|n64] [--run]

The Game Boy program is lifted from a generated 32 KiB ROM that loads the
Mooneye pass registers and stops at `ld b,b`; the Game Boy Advance program
from a generated cartridge that draws a pattern in bitmap mode 3 and loops
(--rom FILE takes another ROM instead); the Nintendo 64 program from a
generated ROM that draws with the RDP, shows it through VI and loops, with
parallel-rdp's C++ in the runtime (linked against the target's libc++). `relift build` runs on this host and
leaves the generated C and its compile commands in its cache; this script
compiles those sources, the runtime and, for the Game Boy, the Rust device
library again for TARGET and links them without the SDL3 window, sound,
libopus or a linked MoltenVK: the host build is made headless by pointing
RELIFT_SDL3_DIR, RELIFT_OPUS_DIR and RELIFT_MOLTENVK_DIR at nothing, so the
host's own libraries never reach the cross build. With --run the program
runs (on this machine for the host target, under wine for Windows on another
host, through adb for the Android targets): the Game Boy program in the
mooneye protocol, the Game Boy Advance and Nintendo 64 programs for a few
frames, whose frame, register and memory hashes must equal those of the host
build (the Nintendo 64 runs, here and there, with no Vulkan driver, so both
take the machine's own scan-out and an RDP that draws nothing). A missing
tool stops the run with what to install. Standard library only.
"""

import argparse
import concurrent.futures as cf
import glob
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ANDROID_API = 30

TARGETS = {
    "windows-x64": dict(rust="x86_64-pc-windows-gnullvm", kind="windows", arch="x86_64", zig="x86_64-windows-gnu", mingw="x86_64-w64-mingw32-clang"),
    "windows-arm64": dict(rust="aarch64-pc-windows-gnullvm", kind="windows", arch="aarch64", zig="aarch64-windows-gnu", mingw="aarch64-w64-mingw32-clang"),
    "android-arm64": dict(rust="aarch64-linux-android", kind="android", arch="aarch64", ndk="aarch64-linux-android"),
    "android-x64": dict(rust="x86_64-linux-android", kind="android", arch="x86_64", ndk="x86_64-linux-android"),
    "macos-x64": dict(rust="x86_64-apple-darwin", kind="apple", arch="x86_64", zig="x86_64-macos", apple="x86_64", sdk="macosx", min="-mmacosx-version-min=12.0"),
    "macos-arm64": dict(rust="aarch64-apple-darwin", kind="apple", arch="aarch64", zig="aarch64-macos", apple="arm64", sdk="macosx", min="-mmacosx-version-min=12.0"),
    "ios-arm64": dict(rust="aarch64-apple-ios", kind="apple", arch="aarch64", apple="arm64", sdk="iphoneos", min="-miphoneos-version-min=15.0"),
    "ios-sim-arm64": dict(rust="aarch64-apple-ios-sim", kind="apple", arch="aarch64", apple="arm64", sdk="iphonesimulator", min="-mios-simulator-version-min=15.0"),
}


class Missing(Exception):
    pass


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


def ndk_roots():
    seen = []
    for var in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK", "ANDROID_NDK_LATEST_HOME"):
        if os.environ.get(var):
            seen.append(os.environ[var])
    sdks = [os.environ.get(v) for v in ("ANDROID_HOME", "ANDROID_SDK_ROOT")] + ["/opt/android-sdk", os.path.expanduser("~/Android/Sdk"), "/usr/local/lib/android/sdk"]
    for sdk in filter(None, sdks):
        seen.append(os.path.join(sdk, "ndk-bundle"))
        found = glob.glob(os.path.join(sdk, "ndk", "*"))
        found.sort(key=lambda p: [int(x) if x.isdigit() else 0 for x in re.split(r"\D+", os.path.basename(p))], reverse=True)
        seen.extend(found)
    seen.append("/opt/android-ndk")
    out = []
    for p in seen:
        if os.path.isfile(os.path.join(p, "source.properties")) and p not in out:
            out.append(p)
    return out


def ndk_revision(root):
    with open(os.path.join(root, "source.properties")) as f:
        m = re.search(r"Pkg.Revision\s*=\s*(\S+)", f.read())
    return m.group(1) if m else "?"


def find_ndk():
    roots = ndk_roots()
    if not roots:
        raise Missing("no Android NDK found: install one (sdkmanager 'ndk;28.2.13676358') or set ANDROID_NDK_HOME")
    root = roots[0]
    bins = glob.glob(os.path.join(root, "toolchains", "llvm", "prebuilt", "*", "bin"))
    if not bins:
        raise Missing(f"{root}: no toolchains/llvm/prebuilt/*/bin")
    return root, bins[0]


class Tool:
    def __init__(self, name, target, cc, ar, cflags, ldflags, libs, note):
        self.name, self.target, self.cc, self.ar = name, target, cc, ar
        self.cflags, self.ldflags, self.libs, self.note = cflags, ldflags, libs, note


def tool_for(name):
    t = TARGETS[name]
    host = platform.system()
    if t["kind"] == "android":
        root, bindir = find_ndk()
        triple = f"{t['ndk']}{ANDROID_API}"
        cc = os.path.join(bindir, f"{triple}-clang")
        if not os.path.isfile(cc):
            raise Missing(f"{cc}: not in the NDK at {root}")
        return Tool(name, t, [cc], [os.path.join(bindir, "llvm-ar")], [], ["-Wl,-z,max-page-size=16384"], ["-lm", "-ldl"], f"NDK r{ndk_revision(root)} at {root}, API {ANDROID_API}")
    if t["kind"] == "windows":
        libs = ["-lm", "-lws2_32", "-luserenv", "-lntdll", "-lbcrypt", "-lunwind"]
        flags = ["-DRL_STATIC_RUNTIME"]
        if shutil.which(t["mingw"]):
            return Tool(name, t, [t["mingw"]], [t["mingw"].replace("clang", "ar")], flags, [], ["-lpthread"] + libs, t["mingw"])
        zig = shutil.which("zig")
        if zig:
            return Tool(name, t, [zig, "cc", "-target", t["zig"]], [zig, "ar"], flags + ["-fno-sanitize=undefined"], [], ["-lpthread"] + libs, f"zig {run([zig, 'version']).stdout.strip()}")
        raise Missing(f"{name}: install zig 0.17.1 (https://ziglang.org/download) or llvm-mingw ({t['mingw']}) and put it on PATH")
    if host == "Darwin":
        sdk = run(["xcrun", "--sdk", t["sdk"], "--show-sdk-path"]).stdout.strip()
        if not sdk:
            raise Missing(f"{name}: Xcode with the {t['sdk']} SDK is needed (xcode-select --install, then install Xcode)")
        cc = ["clang", "-arch", t["apple"], "-isysroot", sdk, t["min"]]
        if t["sdk"] == "iphonesimulator":
            cc[1:1] = ["-target", f"{t['apple']}-apple-ios15.0-simulator"]
            cc.remove("-arch")
            cc.remove(t["apple"])
        return Tool(name, t, cc, ["ar"], [], [], ["-lm", "-liconv"], f"clang with the {t['sdk']} SDK")
    raise Missing(f"{name}: needs macOS with Xcode (Rust std for Apple targets links libiconv from the SDK, which no Linux toolchain ships)")


def make_rom(path):
    logo = bytes.fromhex("CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E")
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = logo
    rom[0x134:0x13C] = b"RLCROSS\0"
    code = bytes([0x06, 3, 0x0E, 5, 0x16, 8, 0x1E, 13, 0x26, 21, 0x2E, 34, 0x40, 0x18, 0xFE])
    rom[0x150:0x150 + len(code)] = code
    c = 0
    for i in range(0x134, 0x14D):
        c = (c - rom[i] - 1) & 0xFF
    rom[0x14D] = c
    g = sum(rom[:0x14E]) + sum(rom[0x150:])
    rom[0x14E], rom[0x14F] = (g >> 8) & 0xFF, g & 0xFF
    with open(path, "wb") as f:
        f.write(rom)


def make_gba_rom(path):
    """Writes a cartridge with a synthetic logo whose ARM code at 0xC0 turns on
    bitmap mode 3, fills the screen with counting colors and loops."""
    rom = bytearray(0x400)
    for i in range(0xC0, len(rom)):
        rom[i] = i % 251
    rom[0:4] = (0xEA00002E).to_bytes(4, "little")
    rom[4:160] = bytes((i * 7 + 3) % 256 for i in range(156))
    rom[0xA0:0xAC] = b"RLCROSS".ljust(12, b"\0")
    rom[0xAC:0xB0] = b"ARLC"
    rom[0xB0:0xB2] = b"01"
    rom[0xB2] = 0x96
    rom[0xBC] = 0
    code = [0xE3A00301, 0xE3A01B01, 0xE3811003, 0xE1C010B0, 0xE3A02406, 0xE3A03000, 0xE3A04C96,
            0xE0C230B2, 0xE2833001, 0xE2544001, 0x1AFFFFFB, 0xEAFFFFFE]
    for i, w in enumerate(code):
        rom[0xC0 + 4 * i:0xC4 + 4 * i] = w.to_bytes(4, "little")
    rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
    with open(path, "wb") as f:
        f.write(rom)


GBA_ARGS = ["--headless", "--no-save", "--frames", "5"]
N64_ARGS = ["--headless", "--no-save", "--no-audio", "--frames", "5"]
NO_VULKAN = {"VK_ICD_FILENAMES": "none.json", "VK_DRIVER_FILES": "none.json"}


def mips(op, rs, rt, imm):
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF)


def make_n64_rom(path):
    """Writes a ROM whose code at 0x80001000 (the first MiB the boot copies)
    puts an RDP display list (a 320 by 240 16-bit color image at 0x300000,
    fill mode, two filled rectangles, a full sync) in RDRAM through KSEG1,
    runs it, sets VI to show the image and loops."""
    rom = bytearray(0x102000)
    rom[0:4] = (0x80371240).to_bytes(4, "big")
    rom[8:12] = (0x80001000).to_bytes(4, "big")
    rom[0x20:0x34] = b"RLCROSS".ljust(20, b" ")
    rom[0x3B:0x40] = b"NXYE\x01"
    t0, t1 = 8, 9
    lui = lambda rt, v: mips(0x0F, 0, rt, v)
    ori = lambda rt, rs, v: mips(0x0D, rs, rt, v)
    sw = lambda rt, off, base: mips(0x2B, base, rt, off)
    dl = [0xFF10013F, 0x00300000, 0xED000000, 0x005003C0, 0xEF300000, 0, 0xF7000000, 0x08430843, 0xF64FC3BC, 0,
          0xF7000000, 0xF801F801, 0xF647C35C, 0x00080060, 0xE9000000, 0]
    vi = [0x3002, 0x300000, 320, 0x200, 0, 0x03E52239, 0x20D, 0xC15, 0x0C150C15, 0x006C02EC, 0x002501FF, 0x000E0204, 0x200, 0x400]
    code = [lui(t0, 0xA028)]
    for k, w in enumerate(dl):
        code += [lui(t1, w >> 16), ori(t1, t1, w), sw(t1, 4 * k, t0)]
    end = 0x280000 + 4 * len(dl)
    code += [lui(t0, 0xA410), ori(t1, 0, 1), sw(t1, 0xC, t0), lui(t1, 0x28), sw(t1, 0, t0), lui(t1, end >> 16), ori(t1, t1, end), sw(t1, 4, t0), lui(t0, 0xA440)]
    for k, w in enumerate(vi):
        code += [lui(t1, w >> 16), ori(t1, t1, w), sw(t1, 4 * k, t0)]
    code += [mips(0x05, 0, t0, -1), 0]
    for i, w in enumerate(code):
        rom[0x1000 + 4 * i:0x1004 + 4 * i] = w.to_bytes(4, "big")
    with open(path, "wb") as f:
        f.write(rom)


def gba_report(text):
    """The lines of a Game Boy Advance or Nintendo 64 run that must match between hosts."""
    return [l for l in text.splitlines() if l.startswith(("frames ", "regs ", "io ", "vi ", "mem"))]


def ninja_builds(path):
    lines = open(path).read().split("\n")
    for i, line in enumerate(lines):
        m = re.match(r"build (\S+): cc (\S+)", line)
        if m:
            flags = shlex.split(lines[i + 1].split("=", 1)[1]) if i + 1 < len(lines) and lines[i + 1].strip().startswith("flags =") else []
            yield m.group(2), flags, os.path.dirname(os.path.abspath(path))


def host_build(args, env):
    proj = os.path.join(args.work, "proj")
    os.makedirs(proj, exist_ok=True)
    if args.rom:
        rom = os.path.abspath(args.rom)
    else:
        rom = os.path.join(args.work, "rlcross." + ("z64" if args.guest == "n64" else args.guest))
        {"gb": make_rom, "gba": make_gba_rom, "n64": make_n64_rom}[args.guest](rom)
    for cmd in ([args.relift, "-q", "import", rom], [args.relift, "-q", "build"]):
        r = run(cmd, cwd=proj, env=env)
        if r.returncode:
            sys.exit(f"{' '.join(cmd)}: exit {r.returncode}\n{r.stdout}{r.stderr}")
    cache = env["RELIFT_CACHE_DIR"]
    game = glob.glob(os.path.join(cache, "projects", "*", "build", "host", "build.ninja"))
    if not game and not shutil.which("ninja"):
        raise Missing("ninja, which relift needs to leave its compile commands for this script: apt install ninja-build, or brew install ninja")
    if len(game) != 1:
        sys.exit(f"{cache}: expected one game build.ninja, found {game}")
    m = re.search(r"(gba?|n64)-runtime/([0-9a-f]+)/runtime\.o", open(game[0]).read())
    runtime = m and os.path.join(cache, m.group(1) + "-runtime", m.group(2), "build.ninja")
    if not runtime or not os.path.isfile(runtime):
        sys.exit(f"{game[0]}: links no runtime build that has a build.ninja")
    return game[0], runtime


def device_library(args, tool, env):
    if args.device_library:
        return os.path.abspath(args.device_library)
    manifest = os.path.join(ROOT, "guests", "gb", "devices", "Cargo.toml")
    target_dir = os.path.join(args.work, "devlib")
    cmd = ["cargo", "build", "--release", "--locked", "--quiet", "--no-default-features", "--manifest-path", manifest, "--target", tool.target["rust"]]
    e = dict(env, CARGO_TARGET_DIR=target_dir)
    if args.rust_toolchain:
        e["RUSTUP_TOOLCHAIN"] = args.rust_toolchain
    r = run(cmd, env=e)
    if r.returncode:
        if "may not be installed" in r.stderr or "can't find crate for `std`" in r.stderr:
            raise Missing(f"the Rust std for {tool.target['rust']}: run `rustup target add {tool.target['rust']}`")
        sys.exit(f"{' '.join(cmd)}\n{r.stderr}")
    lib = glob.glob(os.path.join(target_dir, tool.target["rust"], "release", "librelift_gb_devices.a")) + glob.glob(os.path.join(target_dir, tool.target["rust"], "release", "relift_gb_devices.lib"))
    if not lib:
        sys.exit(f"no device library under {target_dir}/{tool.target['rust']}/release")
    return lib[0]


def adjust(flags, tool):
    out = []
    skip = False
    for f in flags:
        if skip:
            skip = False
            continue
        if f in ("-DRL_HOST_VIDEO", "-fPIC") or "SDL3" in f:
            continue
        if f == "-mcx16" and tool.target["arch"] != "x86_64":
            continue
        out.append(f)
    if tool.target["arch"] == "x86_64" and "-mcx16" not in out:
        out.append("-mcx16")
    if "c++" in out and tool.cc[1:2] == ["cc"] and "-lc++" not in out:
        out.append("-lc++")
    return out + tool.cflags


def compile_all(args, tool, ninjas):
    out = os.path.abspath(os.path.join(args.work, "obj", args.target))
    jobs = []
    for kind, path in ninjas:
        for src, flags, cwd in ninja_builds(path):
            if src.endswith("window_sdl3.c"):
                continue
            o = os.path.join(out, kind, re.sub(r"[^A-Za-z0-9_.]", "_", src[-100:]) + ".o")
            jobs.append((src, adjust(flags, tool), o, cwd))
    os.makedirs(os.path.join(out, "game"), exist_ok=True)
    os.makedirs(os.path.join(out, "rt"), exist_ok=True)
    failures = []

    def one(j):
        src, flags, o, cwd = j
        r = run(tool.cc + flags + ["-MD", "-MF", o + ".d", "-c", src, "-o", o], cwd=cwd)
        return j, r

    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for j, r in ex.map(one, jobs):
            if r.returncode:
                failures.append((j[0], r.stderr))
    if failures:
        for src, err in failures[:5]:
            print(f"compile failed: {src}\n" + "\n".join(diagnostics(err, head=10, tail=5)), file=sys.stderr)
        sys.exit(f"{len(failures)} of {len(jobs)} files failed to compile for {args.target}")
    return [j[2] for j in jobs], len(jobs)


def link(args, tool, objs, devlib):
    exe_name = "game.exe" if tool.target["kind"] == "windows" else "game"
    exe = os.path.join(args.work, "out", args.target, exe_name)
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    rsp = exe + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join(objs + ([devlib] if devlib else [])))
    r = run(tool.cc + tool.cflags + tool.ldflags + [f"@{rsp}", "-o", exe] + tool.libs)
    if r.returncode:
        sys.exit(f"link failed for {args.target}:\n{r.stderr[-3000:]}")
    return exe


def check_alignment(exe):
    tool = shutil.which("llvm-readelf") or shutil.which("readelf")
    if not tool:
        print("readelf not found: segment alignment not checked")
        return
    out = run([tool, "-lW", exe]).stdout
    aligns = [int(m.group(1), 16) for m in re.finditer(r"^\s*LOAD\s.*\s(0x[0-9a-fA-F]+)\s*$", out, re.M)]
    if not aligns or min(aligns) < 0x4000:
        sys.exit(f"{exe}: LOAD segments aligned to {[hex(a) for a in aligns]}, need 0x4000 or more")
    print(f"LOAD alignment {[hex(a) for a in sorted(set(aligns))]}")


def adb_run(exe, work, serial, gba=None, prog=GBA_ARGS, extra=None):
    adb = ["adb"] + (["-s", serial] if serial else [])
    d = "/data/local/tmp/relift-hosts-wave0"
    case = os.path.join(work, "case.txt")
    with open(case, "w") as f:
        f.write("protocol=mooneye\nframes=0xf\nmarker=0x40\n")
    steps = [adb + ["shell", f"rm -rf {d}; mkdir -p {d}"], adb + ["push", exe, f"{d}/game"], adb + ["push", case, f"{d}/case.txt"], adb + ["shell", f"chmod 755 {d}/game"]]
    for s in steps:
        r = run(s)
        if r.returncode:
            sys.exit(f"{' '.join(s)}: {r.stderr}{r.stdout}")
    try:
        if gba is not None:
            env = " ".join(f"{k}={v}" for k, v in (extra or {}).items())
            r = run(adb + ["shell", f"cd {d} && RELIFT_VIDEO=headless {env} HOME={d} ./game {' '.join(prog)}; echo exit=$?"])
            print(r.stdout + r.stderr)
            return "exit=0" in r.stdout and gba_report(r.stdout) == gba
        r = run(adb + ["shell", f"cd {d} && RELIFT_VIDEO=headless HOME={d} ./game --case case.txt --model dmg --stats; echo exit=$?"])
        print(r.stdout + r.stderr)
        return "relift-gb: pass" in r.stdout and "exit=0" in r.stdout
    finally:
        run(adb + ["shell", f"rm -rf {d}"])


def wine_run(exe, work, gba=None, prog=GBA_ARGS, extra=None):
    wine = shutil.which("wine") or shutil.which("wine64")
    if not wine:
        raise Missing("wine, to run the Windows program on Linux (apt install wine64)")
    if gba is not None:
        env = dict(os.environ, RELIFT_VIDEO="headless", WINEDEBUG="-all", WINEPREFIX=os.path.join(work, "wineprefix"), **(extra or {}))
        r = run([wine, exe] + prog, env=env)
        print(r.stdout + r.stderr)
        return r.returncode == 0 and gba_report(r.stdout) == gba
    case = os.path.join(work, "case.txt")
    with open(case, "w") as f:
        f.write("protocol=mooneye\nframes=0xf\nmarker=0x40\n")
    env = dict(os.environ, RELIFT_VIDEO="headless", WINEDEBUG="-all", WINEPREFIX=os.path.join(work, "wineprefix"))
    r = run([wine, exe, "--case", case, "--model", "dmg", "--stats"], env=env)
    print(r.stdout + r.stderr)
    return r.returncode == 0 and "relift-gb: pass" in r.stdout


def host_run(exe, work, gba=None, prog=GBA_ARGS, extra=None):
    if gba is not None:
        r = run([exe] + prog, env=dict(os.environ, RELIFT_VIDEO="headless", **(extra or {})))
        print(r.stdout + r.stderr)
        return r.returncode == 0 and gba_report(r.stdout) == gba
    case = os.path.join(work, "case.txt")
    with open(case, "w") as f:
        f.write("protocol=mooneye\nframes=0xf\nmarker=0x40\n")
    r = run([exe, "--case", case, "--model", "dmg", "--stats"], env=dict(os.environ, RELIFT_VIDEO="headless"))
    print(r.stdout + r.stderr)
    return r.returncode == 0 and "relift-gb: pass" in r.stdout


def cxx_runtime(tool):
    """The C++ runtime libraries a program with C++ in it links on the target."""
    if tool.target["kind"] == "android":
        return ["-static-libstdc++", "-lc++_static", "-lc++abi"]
    return ["-lc++"]


def runtime_libs(args, tool):
    """Builds the runtime libraries and tests with the runtime Makefile for the target.

    Apple targets build without MoltenVK: the Makefile would otherwise link
    the host's Homebrew MoltenVK, which is built for macOS on this machine's
    architecture only.
    """
    t = TARGETS[args.target]
    cmd = ["make", "-C", os.path.join(ROOT, "runtime"), f"-j{args.jobs}", "CC=" + " ".join(map(shlex.quote, tool.cc)), "AR=" + " ".join(map(shlex.quote, tool.ar)), f"BUILD={os.path.join(args.work, 'rt-' + args.target)}"]
    if t["kind"] == "apple":
        cmd.append("MOLTENVK_DIR=")
    if t["kind"] == "apple" and platform.system() == "Darwin":
        cmd.append("TARGET_FLAGS=" + " ".join(tool.cc[1:]))
    elif t["kind"] != "android" and len(tool.cc) > 1:
        cmd.append(f"TARGET={t['zig']}")
    goal = ["libs"] + (["portable"] if t["kind"] == "windows" else ["all"])
    r = run(cmd + goal)
    if r.returncode:
        sys.exit(f"make {' '.join(goal)} failed for {args.target}:\n" + "\n".join(diagnostics(r.stdout + "\n" + r.stderr)))


DIAGNOSTIC = re.compile(r'^error:|: (fatal )?error:|undefined (symbol|reference)|^ld: |^Undefined symbols|^\s+"\S+", referenced from:|\*\*\* .*Error \d')


def diagnostics(text, head=10, tail=30):
    """Returns the compiler, linker and make diagnostics in a build log: the first `head` and the last `tail`.

    Compiler command lines carry -Werror, so a plain search for "error" would
    keep the commands and miss the diagnostics. Without any match it returns
    the tail of the log instead.
    """
    lines = text.splitlines()
    hits = [l for l in lines if DIAGNOSTIC.search(l)]
    if not hits:
        return lines[-tail:]
    if len(hits) <= head + tail:
        return hits
    return hits[:head] + [f"... {len(hits) - head - tail} more ..."] + hits[-tail:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True, choices=sorted(TARGETS))
    ap.add_argument("--relift", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--guest", choices=["gb", "gba", "n64"], default="gb")
    ap.add_argument("--rom", help="build this ROM instead of the generated one")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--serial")
    ap.add_argument("--device-library", help="a prebuilt librelift_gb_devices.a for the target, instead of cargo build")
    ap.add_argument("--rust-toolchain")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()
    args.relift = os.path.abspath(args.relift)
    args.work = os.path.abspath(args.work)
    os.makedirs(args.work, exist_ok=True)
    try:
        tool = tool_for(args.target)
        print(f"{args.target}: {tool.note}")
        none = os.path.join(args.work, "no-host-libraries")
        env = dict(os.environ, RELIFT_CACHE_DIR=os.path.join(args.work, "cache"), RELIFT_JOBS=str(args.jobs), RELIFT_PACKAGE_APPIMAGE="never",
                   RELIFT_SDL3_DIR=none, RELIFT_OPUS_DIR=none, RELIFT_MOLTENVK_DIR=none)
        game, rt = host_build(args, env)
        runtime_libs(args, tool)
        print("runtime libraries build")
        devlib = device_library(args, tool, env) if args.guest == "gb" else None
        objs, n = compile_all(args, tool, [("game", game), ("rt", rt)])
        print(f"{n} files compile")
        if args.guest == "n64":
            tool.libs = tool.libs + cxx_runtime(tool)
        exe = link(args, tool, objs, devlib)
        print(f"linked {exe}")
        guard = [sys.executable, "-I", os.path.join(HERE, "release_guard.py"), "--sources", os.path.join(ROOT, "runtime"), "--objects", os.path.join(args.work, "obj", args.target), "--binary", exe, "--no-interpreter"]
        r = run(guard)
        print((r.stdout + r.stderr).strip())
        if r.returncode:
            sys.exit(f"{args.target}: the release guard failed")
        if TARGETS[args.target]["kind"] == "android":
            check_alignment(exe)
        if args.run:
            kind = TARGETS[args.target]["kind"]
            gba = None
            prog, extra = (N64_ARGS, NO_VULKAN) if args.guest == "n64" else (GBA_ARGS, None)
            if args.guest in ("gba", "n64"):
                host_exe = os.path.join(os.path.dirname(game), "game.exe" if platform.system() == "Windows" else "game")
                r = run([host_exe] + prog, env=dict(os.environ, RELIFT_VIDEO="headless", **(extra or {})))
                gba = gba_report(r.stdout)
                if r.returncode or not gba:
                    sys.exit(f"{host_exe}: the host build did not run: exit {r.returncode}\n{r.stdout}{r.stderr}")
                print("host build:\n" + "\n".join(gba))
            if kind == "android":
                ok = adb_run(exe, args.work, args.serial, gba, prog, extra)
            elif kind == "windows" and platform.system() != "Windows":
                ok = wine_run(exe, args.work, gba, prog, extra)
            else:
                ok = host_run(exe, args.work, gba, prog, extra)
            if not ok:
                sys.exit(f"{args.target}: the program did not pass")
            print(f"{args.target}: program passes")
    except Missing as m:
        sys.exit(f"missing: {m}")


if __name__ == "__main__":
    main()
