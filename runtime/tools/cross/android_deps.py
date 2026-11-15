#!/usr/bin/env python3
"""Builds the Android SDL3 and libopus that `relift build --target android-*`
links, into the layout relift reads:

  <cache>/sdk/sdl3/<arch>-android/{include,lib/libSDL3.so,java/SDL3.jar}
  <cache>/sdk/opus/<arch>-android/{include,lib/libopus.a}

  python3 -I android_deps.py --cache DIR [--target android-arm64] [--work DIR]

The sources are the release tarballs of SDL3 and libopus below, downloaded
and checked against the sha256 pinned here. Needs cmake, ninja and an Android NDK.
Standard library only.
"""

import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

SDL_VERSION = "3.4.20"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz"
SDL_SHA256 = "f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f"
OPUS_VERSION = "1.6.1"
OPUS_URL = f"https://downloads.xiph.org/releases/opus/opus-{OPUS_VERSION}.tar.gz"
OPUS_SHA256 = "6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
API = 30

TARGETS = {
    "android-arm64": ("arm64-v8a", "aarch64-android"),
    "android-x64": ("x86_64", "x86_64-android"),
}


def ndk_root():
    cands = [os.environ.get(v) for v in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK")]
    for sdk in (os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"), "/opt/android-sdk"):
        if sdk:
            cands.append(os.path.join(sdk, "ndk-bundle"))
            cands += sorted(glob.glob(os.path.join(sdk, "ndk", "*")), reverse=True)
    cands.append("/opt/android-ndk")
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "build", "cmake", "android.toolchain.cmake")):
            return c
    sys.exit("no Android NDK: set ANDROID_NDK_HOME")


def fetch(url, sha, dest_dir):
    path = os.path.join(dest_dir, os.path.basename(url))
    if not os.path.isfile(path):
        urllib.request.urlretrieve(url, path)
    h = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if h != sha:
        os.remove(path)
        sys.exit(f"{url}: sha256 {h}, expected {sha}")
    return path


def unpack(tar, into):
    os.makedirs(into, exist_ok=True)
    with tarfile.open(tar) as t:
        t.extractall(into, filter="data")
    return os.path.join(into, os.listdir(into)[0])


def cmake(src, build, prefix, abi, extra):
    tool = os.path.join(ndk_root(), "build", "cmake", "android.toolchain.cmake")
    for cmd in (
        ["cmake", "-S", src, "-B", build, "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={tool}", f"-DANDROID_ABI={abi}", f"-DANDROID_PLATFORM=android-{API}", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384", "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384"] + extra,
        ["ninja", "-C", build, "install"],
    ):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{' '.join(cmd[:2])} failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cache", required=True, help="relift's cache directory")
    ap.add_argument("--target", default="android-arm64", choices=sorted(TARGETS))
    ap.add_argument("--work", help="scratch directory (default: <cache>/android-deps)")
    args = ap.parse_args()
    abi, sdk_dir = TARGETS[args.target]
    work = os.path.abspath(args.work or os.path.join(args.cache, "android-deps"))
    dl = os.path.join(work, "dl")
    os.makedirs(dl, exist_ok=True)
    for tool in ("cmake", "ninja"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found on PATH")
    sdl_src = unpack(fetch(SDL_URL, SDL_SHA256, dl), os.path.join(work, "sdl-src"))
    opus_src = unpack(fetch(OPUS_URL, OPUS_SHA256, dl), os.path.join(work, "opus-src"))
    sdl_out = os.path.join(args.cache, "sdk", "sdl3", sdk_dir)
    opus_out = os.path.join(args.cache, "sdk", "opus", sdk_dir)
    stage = os.path.join(work, "stage", sdk_dir)
    shutil.rmtree(stage, ignore_errors=True)
    cmake(sdl_src, os.path.join(work, "sdl-build", sdk_dir), os.path.join(stage, "sdl3"), abi,
          ["-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TESTS=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_INSTALL_TESTS=OFF"])
    cmake(opus_src, os.path.join(work, "opus-build", sdk_dir), os.path.join(stage, "opus"), abi,
          ["-DBUILD_SHARED_LIBS=OFF", "-DOPUS_BUILD_PROGRAMS=OFF", "-DOPUS_BUILD_TESTING=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"])
    jar = glob.glob(os.path.join(stage, "sdl3", "share", "java", "SDL3", f"SDL3-{SDL_VERSION}.jar"))
    if not jar:
        sys.exit("SDL3's install made no Java jar")
    os.makedirs(os.path.join(stage, "sdl3", "java"), exist_ok=True)
    shutil.copy(jar[0], os.path.join(stage, "sdl3", "java", "SDL3.jar"))
    for src, dst, keep in ((os.path.join(stage, "sdl3"), sdl_out, ("include", "lib/libSDL3.so", "java", "share/licenses")), (os.path.join(stage, "opus"), opus_out, ("include", "lib/libopus.a"))):
        shutil.rmtree(dst, ignore_errors=True)
        for k in keep:
            s = os.path.join(src, k)
            d = os.path.join(dst, k)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            (shutil.copytree if os.path.isdir(s) else shutil.copy)(s, d)
    print(f"SDL3 {SDL_VERSION} -> {sdl_out}\nlibopus {OPUS_VERSION} -> {opus_out}")


if __name__ == "__main__":
    main()
