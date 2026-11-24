"""Known-answer tests for ref_compare.py on a synthetic video (no game data).

Run: python3 -I -m unittest runtime/tools/test_ref_compare.py   (from the repository root, with
runtime/tools on the path, or directly: python3 -I runtime/tools/test_ref_compare.py)
"""

import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ref_compare as rc  # noqa: E402

W, H, FPS, SECONDS = 320, 180, 30, 24


def scene_frame(t):
    """A deterministic synthetic scene: each 3 s block has its own blob layout, which drifts with time."""
    block = int(t // 3)
    rng = np.random.default_rng(1000 + block)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    img = np.zeros((H, W, 3))
    for _ in range(14):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        vx, vy = rng.uniform(-25, 25), rng.uniform(-15, 15)
        r = rng.uniform(10, 40)
        col = rng.uniform(40, 255, 3)
        d = ((xx - cx - vx * (t - 3 * block)) ** 2 + (yy - cy - vy * (t - 3 * block)) ** 2) / (r * r)
        img += np.exp(-d)[..., None] * col
    return np.clip(img, 0, 255).astype(np.uint8)


def make_video(path):
    raw = b"".join(scene_frame(i / FPS).tobytes() for i in range(FPS * SECONDS))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H),
                    "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "12", path],
                   input=raw, check=True)


def degrade(img):
    """Brightness shift, gain and blur, like another renderer's fog and exposure."""
    a = np.asarray(img, np.float64) * 0.7 + 35
    out = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(2.0))
    return out.resize((1280, 720), Image.BILINEAR)


class RefCompare(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="refcmp-test-", dir=os.environ.get("TMPDIR"))
        cls.video = os.path.join(cls.dir, "synthetic.mp4")
        make_video(cls.video)
        rc.build_index(cls.video)

    def test_finds_planted_frame_after_brightness_and_blur(self):
        for t in (4.4, 12.3, 20.1):
            im = degrade(Image.fromarray(scene_frame(t)))
            r = rc.match_images(self.video, [im], rc.DEFAULT_MASK)[0]
            self.assertAlmostEqual(r["time"], t, delta=0.1, msg=str(r))
            self.assertGreater(r["score"], 0.5)
            self.assertIsNotNone(r["second_time"])
            self.assertGreater(r["margin"], 0.05)

    def test_unrelated_capture_is_below_threshold(self):
        rng = np.random.default_rng(7)
        noise = Image.fromarray(rng.integers(0, 255, (H, W, 3), dtype=np.uint8))
        r = rc.match_images(self.video, [noise], rc.DEFAULT_MASK)[0]
        self.assertLess(r["score"], 0.30)

    def test_monotonic_sequence_keeps_order(self):
        times = [2.0, 7.0, 7.5, 15.2, 22.0]
        ims = [degrade(Image.fromarray(scene_frame(t))) for t in times]
        rs = rc.match_images(self.video, ims, rc.DEFAULT_MASK, monotonic=True)
        for t, r in zip(times, rs):
            self.assertAlmostEqual(r["time"], t, delta=0.1, msg=str(r))

    def test_panel_reports_darker_and_softer(self):
        rng = np.random.default_rng(3)
        ref = Image.fromarray(rng.integers(0, 255, (360, 640, 3), dtype=np.uint8))
        cap = degrade(ref).resize((640, 360))
        _, stats = rc.panel(ref, cap, "ref", "cap", [])
        center = [s for s in stats if s["region"] == "center"][0]
        self.assertLess(center["detail_ratio"], 0.7)
        self.assertTrue(any("softer" in w for w in rc.describe(center)))


if __name__ == "__main__":
    unittest.main()
