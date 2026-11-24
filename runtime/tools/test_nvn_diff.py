"""Test for nvn_diff.py on synthetic captures, with stand-ins for krun and xlate: the memory overlay, word
differences, zero bands and their image region, the writer chain, and each verdict (match, sync, race).

Run: python3 -I runtime/tools/test_nvn_diff.py
"""

import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvn_diff  # noqa: E402

# A krun stand-in: copies each outf's init to its output with every word plus one (a deterministic program), and
# each imagefile with an imageout to it unchanged. With RACE set, the first word also takes a running count.
FAKE_KRUN = r'''#!/usr/bin/env python3
import os, struct, sys
spec = open(sys.argv[1]).read().splitlines()
count = 0
cf = os.environ.get("RACE")
if cf:
    count = int(open(cf).read()) if os.path.exists(cf) else 0
    open(cf, "w").write(str(count + 1))
last = None
for l in spec:
    w = l.split()
    if w[0] == "outf":
        b = bytearray(open(w[3], "rb").read())
        for i in range(0, len(b), 4):
            struct.pack_into("<I", b, i, (struct.unpack_from("<I", b, i)[0] + 1) & 0xffffffff)
        if cf:
            struct.pack_into("<I", b, 0, count)
        open(w[1], "wb").write(b)
    elif w[0] == "imagefile":
        last = w[8]
    elif w[0] == "imageout":
        open(w[1], "wb").write(open(last, "rb").read())
'''
FAKE_XLATE = "#!/bin/sh\necho ok\n"


def put(path, data):
    with open(path, "wb") as f:
        f.write(data)


def exe(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def make_capture(d, game_post, image=None, exact=None):
    """One dispatch over a 64-byte storage buffer at guest 0x1000 (GPU 0x9000) whose input is words 0..15 and whose
    game output is game_post; optionally a second dispatch writing a 32x16 RGBA8 image from it."""
    pre = b"".join(struct.pack("<I", i) for i in range(16))
    arena = bytearray()
    spans = []

    def span(what, disp, guest, data, tex=None):
        at = len(arena)
        arena.extend(data)
        s = {"what": what, "disp": disp, "guest": hex(guest), "gpu": hex(guest + 0x8000), "size": len(data), "at": at}
        if tex is not None:
            s["tex"] = tex
        spans.append(s)

    c0 = bytearray(0x968)
    struct.pack_into("<Q", c0, 0x10, 0x9000)
    put(os.path.join(d, "c0-0.bin"), c0)
    put(os.path.join(d, "c0-1.bin"), c0)
    disp = [{"i": 0, "index": 0, "key": "aa" * 16, "queue": "0x1", "submit": 0, "pos": 0, "seq": 0,
             "groups": [1, 1, 1], "indirect": 0, "post_dropped": 0, "c0": "c0-0.bin", "specs": [[0, 16]], "push": "",
             "ranges": [{"kind": "storage", "slot": 0, "gpu": "0x9000", "guest": "0x1000", "size": 64, "write": 1,
                         "unmapped": 0}], "textures": []}]
    span("pre", 0, 0x1000, pre)
    if exact is not None:
        disp[0]["exact"] = 1
        span("exact", 0, 0x1000, exact)
    span("post", 0, 0x1000, game_post)
    if image is not None:
        before, after = image
        t = {"id": 5, "slot": 0, "kind": 0, "dim": 1, "image": 1, "storage": "0x50000", "format": 0, "vk_format": 37,
             "width": 32, "height": 16, "depth": 1, "levels": 1, "layers": 1, "target": 1, "level": 0, "layer": 0,
             "copy_levels": 1, "bytes": len(before), "view": "00" * 17}
        disp.append({"i": 1, "index": 1, "key": "bb" * 16, "queue": "0x1", "submit": 0, "pos": 1, "seq": 1,
                     "groups": [1, 1, 1], "indirect": 0, "post_dropped": 0, "c0": "c0-1.bin", "specs": [],
                     "push": "", "ranges": [{"kind": "storage", "slot": 0, "gpu": "0x9000", "guest": "0x1000",
                                             "size": 64, "write": 0, "unmapped": 0}], "textures": [t]})
        span("pre_image", 1, 0x50000, before, 0)
        span("post_image", 1, 0x50000, after, 0)
    put(os.path.join(d, "arena.bin"), arena)
    with open(os.path.join(d, "capture.json"), "w") as f:
        json.dump({"present": 7, "arena_bytes": len(arena), "dropped_bytes": 0, "commands": len(disp),
                   "dispatches": disp, "spans": spans}, f)


def plus_one(b):
    return b"".join(struct.pack("<I", (struct.unpack_from("<I", b, i)[0] + 1) & 0xffffffff) for i in range(0, len(b), 4))


class Pieces(unittest.TestCase):
    def test_overlay_and_word_diff(self):
        m = nvn_diff.Mem()
        m.under(0x100, b"\x01" * 16)
        m.put(0x104, b"\x02" * 4)
        m.under(0x100, b"\x09" * 32)  # first touch of more bytes: only fills what nothing held
        b = m.read(0x100, 32)
        self.assertEqual(b[:4], b"\x01" * 4)
        self.assertEqual(b[4:8], b"\x02" * 4)
        self.assertEqual(b[16:20], b"\x09" * 4)
        self.assertEqual(nvn_diff.word_diff(b"\0" * 12, b"\0" * 4 + b"\1" + b"\0" * 7), [4])

    def test_zero_bands_and_region(self):
        t = {"width": 64, "height": 32, "layers": 1, "level": 0, "image": 1}
        prev = bytes([7]) * (64 * 32 * 4)
        cur = bytearray(prev)
        for y in range(0, 20):
            for x in range(10, 16):
                cur[(y * 64 + x) * 4:(y * 64 + x) * 4 + 4] = b"\0\0\0\0"
        self.assertEqual(nvn_diff.zero_bands(bytes(cur), t, 4, prev), [(10, 16, 0, 20)])
        offs = nvn_diff.word_diff(prev, bytes(cur))
        r = nvn_diff.image_region(offs, t, 4)
        self.assertEqual(r["x"], [10, 16])
        self.assertEqual(r["y"], [0, 20])
        self.assertEqual(r["tile_cols_128"], [0])


class Verdicts(unittest.TestCase):
    def run_tool(self, game_post, race=False, image=None, extra=(), exact=None):
        d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        cap = os.path.join(d, "cap")
        os.makedirs(cap)
        make_capture(cap, game_post, image, exact)
        exe(os.path.join(d, "krun"), FAKE_KRUN)
        exe(os.path.join(d, "xlate"), FAKE_XLATE)
        env = dict(os.environ)
        if race:
            env["RACE"] = os.path.join(d, "count")
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nvn_diff.py")
        r = subprocess.run([sys.executable, "-I", tool, cap, "--programs", d, "--device", "dev.json", "--out",
                            os.path.join(d, "out"), "--krun", os.path.join(d, "krun"), "--xlate",
                            os.path.join(d, "xlate"), "--repeat", "3"] + list(extra),
                           capture_output=True, text=True, env=env)
        with open(os.path.join(d, "out", "report.json")) as f:
            return r, json.load(f)

    def test_match(self):
        pre = b"".join(struct.pack("<I", i) for i in range(16))
        r, rep = self.run_tool(plus_one(pre), extra=["--all"])
        self.assertEqual(rep["dispatches"][0]["verdict"], "match", r.stdout + r.stderr)
        self.assertIsNone(rep["first_divergent"])

    def test_sync_when_only_the_game_differs(self):
        pre = b"".join(struct.pack("<I", i) for i in range(16))
        bad = bytearray(plus_one(pre))
        bad[20:24] = b"\0\0\0\0"
        r, rep = self.run_tool(bytes(bad), extra=["--all"])
        d0 = rep["dispatches"][0]
        self.assertEqual(d0["verdict"], "sync", r.stdout + r.stderr)
        self.assertEqual(d0["game_vs_gpu"][0]["first_byte"], 20)
        self.assertEqual(d0["game_vs_gpu"][0]["words"], 1)
        self.assertEqual(rep["first_divergent"], 0)

    def test_sync_when_inputs_changed_before_the_dispatch(self):
        pre = b"".join(struct.pack("<I", i) for i in range(16))
        seen = bytearray(pre)
        seen[8:12] = b"\0\0\0\0"  # someone outside the captured dispatches zeroed word 2 before it ran
        r, rep = self.run_tool(plus_one(bytes(seen)), exact=bytes(seen), extra=["--all"])
        d0 = rep["dispatches"][0]
        self.assertEqual(d0["verdict"], "sync", r.stdout + r.stderr)
        self.assertEqual(d0["inputs_changed"][0]["when"], "before")
        self.assertEqual(d0["inputs_changed"][0]["bytes"], [8, 12])
        self.assertEqual(d0["game_vs_gpu"], [], "replayed from what it really read, it matches the game")

    def test_race_when_replays_differ(self):
        pre = b"".join(struct.pack("<I", i) for i in range(16))
        r, rep = self.run_tool(plus_one(pre), race=True, extra=["--all"])
        self.assertEqual(rep["dispatches"][0]["verdict"], "race", r.stdout + r.stderr)

    def test_writer_chain_of_the_zero_band_image(self):
        pre = b"".join(struct.pack("<I", i) for i in range(16))
        before = bytes([9]) * (32 * 16 * 4)
        after = bytearray(before)
        for y in range(16):
            for x in range(4, 8):
                after[(y * 32 + x) * 4:(y * 32 + x) * 4 + 4] = b"\0\0\0\0"
        r, rep = self.run_tool(plus_one(pre), image=(before, bytes(after)))
        self.assertEqual(rep["zero_bands"]["dispatch"], 1, r.stdout + r.stderr)
        self.assertEqual(rep["selected"], [0, 1])
        d1 = rep["dispatches"][1]
        # The stand-in copies the image through: the game's zeros are not in the replay.
        self.assertEqual(d1["verdict"], "sync")
        self.assertEqual(d1["game_vs_gpu"][0]["region"]["x"], [4, 8])
        self.assertEqual(rep["first_divergent"], 1)


if __name__ == "__main__":
    unittest.main()
