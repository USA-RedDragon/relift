"""Test for nvn_trace.py on a synthetic frame trace holding raw command lines beside draws and dispatches.

Run: python3 -I runtime/tools/test_nvn_trace.py
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvn_trace  # noqa: E402


class Summary(unittest.TestCase):
    def test_summary_skips_command_lines_without_programs(self):
        recs = [
            {"kind": "cmd", "before": 0, "name": "nvnCommandBufferSetSamplerPool", "x": ["0x1"]},
            {"i": 0, "kind": "draw", "count": 3, "instances": 1, "programs": {"vertex": {"key": "ab" * 32}},
             "targets": [{"depth": 0, "width": 8, "height": 8}], "textures": [], "buffers": []},
            {"kind": "cmd", "before": 1, "name": "nvnCommandBufferBarrier", "x": []},
            {"i": 1, "kind": "dispatch", "groups": [1, 1, 1], "programs": {"compute": {"key": "cd" * 32}},
             "textures": [], "buffers": []},
        ]
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
            with open(os.path.join(d, "trace.jsonl"), "w") as f:
                f.write("\n".join(json.dumps(r) for r in recs) + "\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                nvn_trace.cmd_summary(d)
            lines = out.getvalue().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertIn("draw", lines[0])
            self.assertIn("dispatch", lines[1])


if __name__ == "__main__":
    unittest.main()
