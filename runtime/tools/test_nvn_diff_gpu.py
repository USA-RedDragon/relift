"""GPU test for nvn_diff.py: a capture made from the launch state alone replays to the driver's own output.

Each buffer-only case of the compute known answers (gpus/sm86/tests/kat/compute.txt: programs, inputs and the
outputs the NVIDIA driver gave) becomes a capture as the runtime writes one: c[0] with the buffers' addresses, the
inputs as first-touch spans, the driver's outputs as the game's outputs, and the launch state (workgroup size and
count, every specialization constant, the shared size the runtime chooses) in the dispatch record. nvn_diff.py
then replays it from the capture alone, and every case must match the driver word for word.

Needs a Vulkan device whose name holds KAT_REF (default "NVIDIA"), krun (KRUN, built with
`cc -O1 -o krun gpus/sm86/kat/compute/krun.c -lvulkan`) and the sm86 xlate example (XLATE). The device
description comes from DEVICE_JSON, a runtime's shaders/device.json. The test is skipped without them.

Run: KRUN=... XLATE=... DEVICE_JSON=... python3 -I runtime/tools/test_nvn_diff_gpu.py
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../.."))
sys.path.insert(0, os.path.join(ROOT, "gpus/sm86/kat/compute"))
sys.path.insert(0, HERE)
import kat  # noqa: E402

KRUN = os.environ.get("KRUN", "")
XLATE = os.environ.get("XLATE", "")
DEVICE = os.environ.get("DEVICE_JSON", "")
GPU = os.environ.get("KAT_REF", "NVIDIA")
# Cases whose program reads and writes only buffers (no images, textures or samplers).
CASES = ["bssy", "i64", "i64add", "loopexit", "bcast", "popcinc", "hmma", "hmmaspec", "f64", "membar"]
BASE = 0x7_0000_0000
STRIDE = 0x100_0000


def capture_from_case(d, name, case):
    """Writes the capture of one known-answer case to d; False when the case does not fit (it binds images)."""
    lines, _ = kat.parse_spec(name)
    inputs = case["inputs"]
    if any(l.split()[0] in ("image", "imagefile", "view", "sampler") for l in lines):
        return False
    local, groups, specs, bufs, c0 = [1, 1, 1], [1, 1, 1], {}, [], bytearray(0x968)
    for l in lines:
        w = l.split()
        if w[0] == "local":
            local = [int(x) for x in w[1:4]]
        elif w[0] == "groups":
            groups = [int(x) for x in w[1:4]]
        elif w[0] == "spec":
            specs[int(w[1])] = int(w[2])
        elif w[0] == "buf":
            bufs.append(("in", inputs[int(w[1][2:])], None))
        elif w[0] == "out":
            bufs.append(("out", bytes(int(w[2])), w[1][3:]))
        elif w[0] == "outf":
            bufs.append(("out", inputs[int(w[3][2:])], w[1][3:]))
        elif w[0] == "put" and w[1] == "0" and w[3] == "addr":
            struct.pack_into("<Q", c0, int(w[2], 0), BASE + STRIDE * int(w[4]))
        elif w[0] == "put" and w[1] == "0" and w[3] == "u32":
            struct.pack_into("<I", c0, int(w[2], 0), int(w[4], 0) & 0xFFFFFFFF)
    for k in range(3):
        specs[k] = local[k]
    specs.setdefault(3, 4096)  # the shared words the runtime chooses (krun's default for these cases)
    specs.setdefault(4, 1024)
    arena, spans, ranges = bytearray(), [], []

    def span(what, guest, data):
        spans.append({"what": what, "disp": 0, "guest": hex(guest), "gpu": hex(guest), "size": len(data),
                      "at": len(arena)})
        arena.extend(data)

    for k, (kind, data, out) in enumerate(bufs):
        addr = BASE + STRIDE * k
        span("pre", addr, data)
        if kind == "out" and out is not None and out in case["answers"]:
            span("post", addr, case["answers"][out])
        for off in range(0x10, 0x800, 8):
            if struct.unpack_from("<Q", c0, off)[0] == addr:
                ranges.append({"kind": "base", "slot": off, "gpu": hex(addr), "guest": hex(addr), "size": len(data),
                               "write": 1, "unmapped": 0})
    with open(os.path.join(d, "c0-0.bin"), "wb") as f:
        f.write(c0)
    with open(os.path.join(d, "arena.bin"), "wb") as f:
        f.write(arena)
    disp = {"i": 0, "index": 0, "key": "prog", "queue": "0x0", "submit": 0, "pos": 0, "seq": 0, "groups": groups,
            "indirect": 0, "post_dropped": 0, "exact": 0, "c0": "c0-0.bin", "specs": sorted(specs.items()),
            "push": "", "fixed_push": "", "ranges": ranges, "textures": []}
    with open(os.path.join(d, "capture.json"), "w") as f:
        json.dump({"present": 0, "arena_bytes": len(arena), "dropped_bytes": 0, "commands": 1,
                   "dispatches": [disp], "spans": spans}, f)
    with open(os.path.join(d, "prog.bin"), "wb") as f:
        f.write(case["program"])
    return True


@unittest.skipUnless(KRUN and XLATE and DEVICE and os.path.exists(KRUN), "needs KRUN, XLATE and DEVICE_JSON")
class ReplayMatchesTheDriver(unittest.TestCase):
    def test_known_answers_replay_exactly_from_their_captures(self):
        recs = kat.read_kat(os.path.join(ROOT, "gpus/sm86/tests/kat/compute.txt"))
        tool = os.path.join(HERE, "nvn_diff.py")
        ran = 0
        for name in CASES:
            if name not in recs:
                continue
            with self.subTest(case=name), tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
                cap = os.path.join(d, "cap")
                os.makedirs(cap)
                if not capture_from_case(cap, name, recs[name]):
                    continue
                r = subprocess.run([sys.executable, "-I", tool, cap, "--programs", cap, "--device", DEVICE,
                                    "--out", os.path.join(d, "out"), "--krun", KRUN, "--xlate", XLATE, "--gpu", GPU,
                                    "--all", "--repeat", "2", "--no-spvi"], capture_output=True, text=True)
                with open(os.path.join(d, "out", "report.json")) as f:
                    rep = json.load(f)
                v = rep["dispatches"][0]
                self.assertEqual(v["verdict"], "match", "%s: %s\n%s" % (name, json.dumps(v)[:800], r.stderr[-800:]))
                ran += 1
        self.assertGreaterEqual(ran, 5)


if __name__ == "__main__":
    unittest.main()
