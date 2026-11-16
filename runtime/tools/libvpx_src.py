#!/usr/bin/env python3
"""Unpacks the pinned libvpx release whose VP9 decoder `relift build`
compiles into Switch games (for NVDEC video), into the layout relift reads:

  <cache>/sdk/libvpx/<version>/   (the libvpx source tree)

  python3 -I libvpx_src.py --cache DIR

The source is the release tarball, downloaded and checked against its
sha256. relift compiles only plain C files of it (no assembly, no run-time
CPU detection) with the generic configuration in runtime/host/vpx, so the
same sources serve every host. Standard library only.
"""

import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import urllib.request

LIBVPX_VERSION = "1.17.0"
LIBVPX_URL = f"https://github.com/webmproject/libvpx/archive/refs/tags/v{LIBVPX_VERSION}.tar.gz"
LIBVPX_SHA256 = "1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a"


def fetch(url, sha, dest_dir):
    path = os.path.join(dest_dir, f"libvpx-{LIBVPX_VERSION}.tar.gz")
    if not os.path.isfile(path):
        urllib.request.urlretrieve(url, path)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != sha:
        os.remove(path)
        sys.exit(f"{url}: sha256 {h.hexdigest()}, expected {sha}")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cache", required=True, help="relift cache directory (the cache-dir setting)")
    args = ap.parse_args()
    sdk = os.path.join(args.cache, "sdk", "libvpx")
    out = os.path.join(sdk, LIBVPX_VERSION)
    os.makedirs(sdk, exist_ok=True)
    tar = fetch(LIBVPX_URL, LIBVPX_SHA256, sdk)
    work = out + ".part"
    shutil.rmtree(work, ignore_errors=True)
    prefix = f"libvpx-{LIBVPX_VERSION}/"
    with tarfile.open(tar) as t:
        for m in t.getmembers():
            if m.name == prefix.rstrip("/") and m.isdir():
                continue
            if not m.name.startswith(prefix) or ".." in m.name.split("/") or m.issym() or m.islnk():
                sys.exit(f"{tar}: unsafe member {m.name}")
            if not m.isfile():
                continue
            dst = os.path.join(work, m.name[len(prefix):])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with t.extractfile(m) as src, open(dst, "wb") as f:
                shutil.copyfileobj(src, f)
    shutil.rmtree(out, ignore_errors=True)
    os.rename(work, out)
    os.remove(tar)
    print(f"libvpx {LIBVPX_VERSION} -> {out}")


if __name__ == "__main__":
    main()
