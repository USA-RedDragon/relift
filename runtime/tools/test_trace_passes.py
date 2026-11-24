"""Known-answer test for trace_passes.py on a small synthetic frame trace.

Run: python3 -I runtime/tools/test_trace_passes.py
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_passes as tp  # noqa: E402


def target(vimg, w, h):
    return {"depth": 0, "vimg": vimg, "storage": "0x1000", "format": 1, "vk_format": 37, "width": w, "height": h,
            "level": 0, "pre": "x", "post": "x"}


def draw(i, vimg, w, h, viewport):
    return {"i": i, "kind": "draw", "viewport": viewport, "scissor": [0, 0, 1920, 1080], "scissor_set": 1,
            "targets": [target(vimg, w, h)], "textures": [], "buffers": []}


def dispatch(i, groups, local, w, h, d):
    img = {"width": w, "height": h, "depth": d, "pre": "a", "post": "a"}
    return {"i": i, "kind": "dispatch", "groups": groups, "specs": [[0, local[0]], [1, local[1]], [2, local[2]]],
            "textures": [img], "buffers": []}


def write_frame(root, n, scene_w, scene_h, extra):
    path = os.path.join(root, "frame-%d" % n)
    os.makedirs(path)
    cmds = [draw(k, "0xa", scene_w, scene_h, [0, 0, scene_w, scene_h]) for k in range(25)]
    cmds += extra
    cmds.append(draw(900, "0xb", 1920, 1080, [0, 0, 1920, 1080]))
    with open(os.path.join(path, "trace.jsonl"), "w") as f:
        for c in cmds:
            f.write(json.dumps(c) + "\n")
    return path


class TracePasses(unittest.TestCase):
    def run_report(self, *frames):
        class A:
            min = 0.9
            all = False
            no_data = True

        out, rows, flagged = io.StringIO(), [], 0
        with contextlib.redirect_stdout(out):
            for f in frames:
                flagged += tp.report_frame(f, A, rows)
        return out.getvalue(), rows, flagged

    def test_planted_half_viewport_and_partial_dispatch_are_flagged(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as root:
            f = write_frame(root, 1, 1920, 1080, [
                draw(100, "0xc", 640, 360, [0, 0, 640, 360]),
                draw(101, "0xd", 640, 360, [0, 0, 320, 180]),
                dispatch(102, [8, 8, 6], (8, 8, 8), 64, 64, 48),
                dispatch(103, [4, 4, 3], (8, 8, 8), 64, 64, 48),
            ])
            text, _, flagged = self.run_report(f)
            self.assertEqual(flagged, 2, text)
            self.assertIn("draw      101", text)
            self.assertIn("area 320x180  25%", text)
            self.assertIn("dispatch  103", text)
            self.assertNotIn("draw      100", text)
            self.assertNotIn("dispatch  102", text)

    def test_mip_chain_is_not_flagged(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as root:
            f = write_frame(root, 1, 1920, 1080, [
                draw(100, "0xc", 128, 128, [0, 0, 64, 64]),
                draw(101, "0xc", 128, 128, [0, 0, 32, 32]),
            ])
            _, _, flagged = self.run_report(f)
            self.assertEqual(flagged, 0)

    def test_scene_size_timeline(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as root:
            a = write_frame(root, 1, 1920, 1080, [])
            b = write_frame(root, 2, 854, 480, [])
            _, rows, _ = self.run_report(a, b)
            self.assertEqual([r[1] for r in rows], [(1920, 1080), (854, 480)])
            self.assertEqual(rows[1][2], (1920, 1080))


if __name__ == "__main__":
    unittest.main()
