#!/usr/bin/env python3
"""Cross-builds the C runtime and a Game Boy test program for one host target.

  python3 -I gbcross.py --target windows-x64 --relift BIN --work DIR [--run]

The program is lifted from a generated 32 KiB ROM that loads the Mooneye pass
registers and stops at `ld b,b`. `relift build` runs on this host and leaves
the generated C and its compile commands in its cache; this script compiles
those sources, the runtime and the Rust device library again for TARGET and
links them without the SDL3 window. With --run the program runs in the
mooneye protocol (on this machine for the host target, through adb for the
Android targets). A missing tool stops the run with what to install.
Standard library only.
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
    rom = os.path.join(args.work, "rlcross.gb")
    make_rom(rom)
    for cmd in ([args.relift, "-q", "import", rom], [args.relift, "-q", "build"]):
        r = run(cmd, cwd=proj, env=env)
        if r.returncode:
            sys.exit(f"{' '.join(cmd)}: exit {r.returncode}\n{r.stdout}{r.stderr}")
    cache = env["RELIFT_CACHE_DIR"]
    game = glob.glob(os.path.join(cache, "projects", "*", "build", "host", "build.ninja"))
    runtime = glob.glob(os.path.join(cache, "gb-runtime", "*", "build.ninja"))
    if not game and not shutil.which("ninja"):
        raise Missing("ninja, which relift needs to leave its compile commands for this script: apt install ninja-build, or brew install ninja")
    if len(game) != 1 or len(runtime) != 1:
        sys.exit(f"{cache}: expected one game and one runtime build.ninja, found {game} {runtime}")
    return game[0], runtime[0]


def device_library(args, tool, env):
    if args.device_library:
        return os.path.abspath(args.device_library)
    manifest = os.path.join(ROOT, "guests", "gb", "devices", "Cargo.toml")
    target_dir = os.path.join(args.work, "devlib")
    cmd = ["cargo", "build", "--release", "--locked", "--quiet", "--manifest-path", manifest, "--target", tool.target["rust"]]
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
        f.write("\n".join(objs + [devlib]))
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


def adb_run(exe, work, serial):
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
        r = run(adb + ["shell", f"cd {d} && RELIFT_VIDEO=headless HOME={d} ./game --case case.txt --model dmg --check --stats; echo exit=$?"])
        print(r.stdout + r.stderr)
        return "relift-gb: pass" in r.stdout and "exit=0" in r.stdout
    finally:
        run(adb + ["shell", f"rm -rf {d}"])


def wine_run(exe, work):
    wine = shutil.which("wine") or shutil.which("wine64")
    if not wine:
        raise Missing("wine, to run the Windows program on Linux (apt install wine64)")
    case = os.path.join(work, "case.txt")
    with open(case, "w") as f:
        f.write("protocol=mooneye\nframes=0xf\nmarker=0x40\n")
    env = dict(os.environ, RELIFT_VIDEO="headless", WINEDEBUG="-all", WINEPREFIX=os.path.join(work, "wineprefix"))
    r = run([wine, exe, "--case", case, "--model", "dmg", "--check", "--stats"], env=env)
    print(r.stdout + r.stderr)
    return r.returncode == 0 and "relift-gb: pass" in r.stdout


def host_run(exe, work):
    case = os.path.join(work, "case.txt")
    with open(case, "w") as f:
        f.write("protocol=mooneye\nframes=0xf\nmarker=0x40\n")
    r = run([exe, "--case", case, "--model", "dmg", "--check", "--stats"], env=dict(os.environ, RELIFT_VIDEO="headless"))
    print(r.stdout + r.stderr)
    return r.returncode == 0 and "relift-gb: pass" in r.stdout


def runtime_libs(args, tool):
    t = TARGETS[args.target]
    cmd = ["make", "-C", os.path.join(ROOT, "runtime"), f"-j{args.jobs}", "CC=" + " ".join(map(shlex.quote, tool.cc)), "AR=" + " ".join(map(shlex.quote, tool.ar)), f"BUILD={os.path.join(args.work, 'rt-' + args.target)}"]
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
        env = dict(os.environ, RELIFT_CACHE_DIR=os.path.join(args.work, "cache"), RELIFT_JOBS=str(args.jobs), RELIFT_PACKAGE_APPIMAGE="never")
        game, rt = host_build(args, env)
        runtime_libs(args, tool)
        print("runtime libraries build")
        devlib = device_library(args, tool, env)
        objs, n = compile_all(args, tool, [("game", game), ("rt", rt)])
        print(f"{n} files compile")
        exe = link(args, tool, objs, devlib)
        print(f"linked {exe}")
        guard = [sys.executable, "-I", os.path.join(HERE, "release_guard.py"), "--sources", os.path.join(ROOT, "runtime"), "--objects", os.path.join(args.work, "obj", args.target), "--binary", exe]
        r = run(guard)
        print((r.stdout + r.stderr).strip())
        if r.returncode:
            sys.exit(f"{args.target}: the release guard failed")
        if TARGETS[args.target]["kind"] == "android":
            check_alignment(exe)
        if args.run:
            kind = TARGETS[args.target]["kind"]
            if kind == "android":
                ok = adb_run(exe, args.work, args.serial)
            elif kind == "windows" and platform.system() != "Windows":
                ok = wine_run(exe, args.work)
            else:
                ok = host_run(exe, args.work)
            if not ok:
                sys.exit(f"{args.target}: the program did not pass")
            print(f"{args.target}: program passes")
    except Missing as m:
        sys.exit(f"missing: {m}")


if __name__ == "__main__":
    main()
