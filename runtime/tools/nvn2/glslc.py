#!/usr/bin/env python3
"""glslc.py: the transform feedback section of NVN2 GLSLC program headers (magic 0x98761234), as the driver reads it.

  glslc.py xfb <file> [hex offset]          decode every header in a file (or the one at offset)
  glslc.py scan <scan dir> <import dir>     every header in the payloads that hold the scan's programs, with xfb

What the driver does (hw sdk.nso, the GLSLC header accessor vtable at 0xdd1558, 0x5e5bc0 and 0x619a40):
  header+0x54 u8  buffer count n (0: no transform feedback)
  header+0x55 u8  (kept, unused for layout)
  header+0xa7 u8  the entries are hardware layout-select bytes already (copied as they are)
  header+0xc4 u32 offset of the section from the header; the section is
      u32 version (must be 1), u16 stride[n] (bytes), u8 stream[n], u8 count[n],
      then per buffer: s8 attr[count], u8 first_component[count], u8 component_count[count]
Per buffer i the driver writes SET_STREAM_OUT_CONTROL_STREAM(i) = stream, _COMPONENT_COUNT(i) = stride / 4,
_STRIDE(i) = stride, and SET_STREAM_OUT_LAYOUT_SELECT(i, *) with the bytes: either attr[] as is (raw flag), or one
byte per component, attr's base attribute index (driver table) + first_component + k, 0xff for an attr below -2.
"""
import json
import os
import struct
import sys
import zlib

MAGIC = b"\x34\x12\x76\x98"


def section(b, h):
    """The decoded xfb section of the header at h in b, or None."""
    if h + 0xC8 > len(b):
        return None
    n = b[h + 0x54]
    if not n or n > 4:
        return None
    raw = b[h + 0xA7]
    off = struct.unpack_from("<I", b, h + 0xC4)[0]
    s = h + off
    if s + 4 > len(b) or struct.unpack_from("<I", b, s)[0] != 1:
        return {"buffers": n, "error": "section version not 1 at +%#x" % off}
    p = s + 4
    strides = list(struct.unpack_from("<%dH" % n, b, p))
    p += 2 * n
    streams = list(b[p: p + n])
    p += n
    counts = list(b[p: p + n])
    p += n
    bufs = []
    for i in range(n):
        c = counts[i]
        attr = [x - 256 if x > 127 else x for x in b[p: p + c]]
        p += c
        first = list(b[p: p + c])
        p += c
        ncomp = list(b[p: p + c])
        p += c
        bufs.append({"stride": strides[i], "stream": streams[i], "attr": attr, "first": first, "ncomp": ncomp})
    return {"buffers": n, "raw": raw, "section": off, "layout": bufs}


def headers(b):
    i = b.find(MAGIC)
    while i >= 0:
        yield i
        i = b.find(MAGIC, i + 1)


def chain(f, at):
    """The payload of the zlib chain at `at` (streams at most 16 bytes apart), as relift-shaders' sweep decodes it."""
    out = bytearray()
    pos = at
    while True:
        d = zlib.decompressobj()
        f.seek(pos)
        tail = b""
        try:
            while not d.eof:
                piece = f.read(1 << 20)
                if not piece:
                    break
                out += d.decompress(piece)
                pos += len(piece)
            tail = d.unused_data
        except zlib.error:
            break
        if not d.eof:
            break
        pos -= len(tail)
        f.seek(pos)
        head = f.read(18)
        nxt = next((g for g in range(17) if g + 1 < len(head) and head[g] == 0x78
                    and head[g + 1] in (0x01, 0x5E, 0x9C, 0xDA)), -1)
        if nxt < 0:
            break
        pos += nxt
    return bytes(out)


def main():
    cmd = sys.argv[1]
    if cmd == "xfb":
        b = open(sys.argv[2], "rb").read()
        hs = [int(sys.argv[3], 16)] if len(sys.argv) > 3 else headers(b)
        for h in hs:
            x = section(b, h)
            if x:
                print("%#x %s" % (h, json.dumps(x)))
        return
    if cmd == "scan":
        scan, imp = sys.argv[2], sys.argv[3]
        srcs = set()
        for dp, _, fn in os.walk(os.path.join(scan, "jobs")):
            for f in fn:
                for p in json.load(open(os.path.join(dp, f))).get("programs", []):
                    if p[1] == "sph" and p[2] in (1, 2, 3, 4):
                        for s in p[6]:
                            parts = s.split("@")
                            if len(parts) >= 2:
                                srcs.add((parts[0], int(parts[1].split(":")[0], 16)))
        print("chains %d" % len(srcs), file=sys.stderr)
        files = {}
        for k, (name, at) in enumerate(sorted(srcs)):
            path = os.path.join(imp, name)
            if path not in files:
                files[path] = open(path, "rb")
            b = chain(files[path], at)
            for h in headers(b):
                x = section(b, h)
                if x:
                    print("%s@%#x +%#x %s" % (name, at, h, json.dumps(x)), flush=True)
            if k % 1000 == 0:
                print("%d/%d" % (k, len(srcs)), file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
