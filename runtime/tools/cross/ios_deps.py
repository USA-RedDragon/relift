#!/usr/bin/env python3
"""Builds the iOS SDL3 and libopus that `relift build --target ios-*` links,
as static libraries, into the layout relift reads:

  <cache>/sdk/sdl3/<arm64-ios|arm64-ios-sim>/{include,lib/libSDL3.a}
  <cache>/sdk/opus/<arm64-ios|arm64-ios-sim>/{include,lib/libopus.a}

  python3 -I ios_deps.py --cache DIR [--target ios-arm64] [--work DIR]

The sources are the release tarballs pinned in android_deps.py, downloaded
and checked against their sha256. Needs macOS with Xcode, cmake and ninja.
Standard library only.
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
MIN_IOS = "15.0"

TARGETS = {
    "ios-arm64": ("iphoneos", "arm64-ios"),
    "ios-sim-arm64": ("iphonesimulator", "arm64-ios-sim"),
}


def pins():
    spec = importlib.util.spec_from_file_location("android_deps", os.path.join(HERE, "android_deps.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def unpack(tar, into):
    os.makedirs(into, exist_ok=True)
    with tarfile.open(tar) as t:
        for m in t.getmembers():
            if m.name.startswith("/") or ".." in m.name.split("/") or m.issym() or m.islnk():
                sys.exit(f"{tar}: unsafe member {m.name}")
        t.extractall(into)
    return os.path.join(into, os.listdir(into)[0])


def cmake(src, build, prefix, sdk, extra):
    for cmd in (
        ["cmake", "-S", src, "-B", build, "-G", "Ninja", "-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_SYSROOT={sdk}", "-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MIN_IOS}", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO"] + extra,
        ["ninja", "-C", build, "install"],
    ):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{' '.join(cmd[:2])} failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cache", required=True, help="relift's cache directory")
    ap.add_argument("--target", default="ios-arm64", choices=sorted(TARGETS))
    ap.add_argument("--work", help="scratch directory (default: <cache>/ios-deps)")
    args = ap.parse_args()
    if sys.platform != "darwin":
        sys.exit("the iOS libraries build on macOS with Xcode")
    sdk, sdk_dir = TARGETS[args.target]
    for tool in ("cmake", "ninja", "xcrun"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found on PATH")
    p = pins()
    work = os.path.abspath(args.work or os.path.join(args.cache, "ios-deps"))
    dl = os.path.join(work, "dl")
    os.makedirs(dl, exist_ok=True)
    sdl_src = unpack(p.fetch(p.SDL_URL, p.SDL_SHA256, dl), os.path.join(work, "sdl-src"))
    opus_src = unpack(p.fetch(p.OPUS_URL, p.OPUS_SHA256, dl), os.path.join(work, "opus-src"))
    stage = os.path.join(work, "stage", sdk_dir)
    shutil.rmtree(stage, ignore_errors=True)
    cmake(sdl_src, os.path.join(work, "sdl-build", sdk_dir), os.path.join(stage, "sdl3"), sdk,
          ["-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TESTS=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_INSTALL_TESTS=OFF", "-DSDL_EXAMPLES=OFF", "-DSDL_CAMERA=OFF"])
    cmake(opus_src, os.path.join(work, "opus-build", sdk_dir), os.path.join(stage, "opus"), sdk,
          ["-DBUILD_SHARED_LIBS=OFF", "-DOPUS_BUILD_PROGRAMS=OFF", "-DOPUS_BUILD_TESTING=OFF"])
    sdl_out = os.path.join(args.cache, "sdk", "sdl3", sdk_dir)
    opus_out = os.path.join(args.cache, "sdk", "opus", sdk_dir)
    for src, dst, keep in ((os.path.join(stage, "sdl3"), sdl_out, ("include", "lib/libSDL3.a", "share/licenses")), (os.path.join(stage, "opus"), opus_out, ("include", "lib/libopus.a"))):
        shutil.rmtree(dst, ignore_errors=True)
        for k in keep:
            s = os.path.join(src, k)
            if not os.path.exists(s):
                continue
            d = os.path.join(dst, k)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            (shutil.copytree if os.path.isdir(s) else shutil.copy)(s, d)
        for k in keep[:2]:
            if not os.path.exists(os.path.join(dst, k)):
                sys.exit(f"{dst}: the install made no {k}")
    print(f"SDL3 {p.SDL_VERSION} -> {sdl_out}\nlibopus {p.OPUS_VERSION} -> {opus_out}")


if __name__ == "__main__":
    main()
