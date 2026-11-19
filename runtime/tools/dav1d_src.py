#!/usr/bin/env python3
"""Unpacks the pinned dav1d release whose AV1 decoder `relift build`
compiles into Switch 2 games (for their video decoder service), into the
layout relift reads:

  <cache>/sdk/dav1d/<version>/   (the dav1d source tree)

  python3 -I dav1d_src.py --cache DIR

The source is the release tarball, downloaded and checked against its
sha256. relift compiles only plain C files of it (no assembly, no run-time
CPU detection) with the generic configuration in runtime/host/dav1d, so the
same sources serve every host. Standard library only.
"""

import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import urllib.request

DAV1D_VERSION = "1.5.3"
DAV1D_URL = f"https://download.videolan.org/pub/videolan/dav1d/{DAV1D_VERSION}/dav1d-{DAV1D_VERSION}.tar.xz"
DAV1D_SHA256 = "732010aa5ef461fa93355ed2c6c5fedb48ddc4b74e697eaabe8907eaeb943011"


def fetch(url, sha, dest_dir):
    path = os.path.join(dest_dir, f"dav1d-{DAV1D_VERSION}.tar.xz")
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
    sdk = os.path.join(args.cache, "sdk", "dav1d")
    out = os.path.join(sdk, DAV1D_VERSION)
    os.makedirs(sdk, exist_ok=True)
    tar = fetch(DAV1D_URL, DAV1D_SHA256, sdk)
    work = out + ".part"
    shutil.rmtree(work, ignore_errors=True)
    prefix = f"dav1d-{DAV1D_VERSION}/"
    with tarfile.open(tar, "r:xz") as t:
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
    print(f"dav1d {DAV1D_VERSION} -> {out}")


if __name__ == "__main__":
    main()
