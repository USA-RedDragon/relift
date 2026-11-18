"""gen_post_data.py: write runtime/core/post_data.h, the data of the core
picture pipeline: the SPIR-V of every pass in this folder (compiled with
glslc from shaderc) and SMAA's area and search textures, read from
runtime/third_party/smaa. Run it from any folder after changing a shader or
the SMAA files; it needs glslc on PATH.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.dirname(HERE)
SMAA = os.path.join(CORE, "..", "third_party", "smaa")
OUT = os.path.join(CORE, "post_data.h")

PASSES = [
    ("fxaa", "fxaa.comp", []),
    ("smaa_edges", "smaa_edges.comp", []),
    ("smaa_weights", "smaa_weights.comp", []),
    ("smaa_weights_fast", "smaa_weights.comp", ["-DSMAA_DISABLE_DIAG_DETECTION", "-DSMAA_DISABLE_CORNER_DETECTION"]),
    ("smaa_blend", "smaa_blend.comp", []),
    ("upscale", "upscale.comp", []),
    ("upscale_buffers", "upscale.comp", ["-DRL_BUFFERS"]),
]


def spirv(src, defines, work):
    out = os.path.join(work, "pass.spv")
    cmd = ["glslc", "--target-env=vulkan1.0", "-O", "-I", HERE, "-I", SMAA, *defines, os.path.join(HERE, src), "-o", out]
    subprocess.run(cmd, check=True)
    with open(out, "rb") as f:
        data = f.read()
    return struct.unpack(f"<{len(data) // 4}I", data)


def texture(name, array):
    with open(os.path.join(SMAA, name), encoding="utf-8") as f:
        text = f.read()
    body = text[text.index(array) :]
    body = body[body.index("{") + 1 : body.index("}")]
    return bytes(int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body))


def rows(values, per, fmt):
    for i in range(0, len(values), per):
        yield "    " + ", ".join(fmt(v) for v in values[i : i + per]) + ","


def main():
    lines = [
        "/**",
        " * @file post_data.h",
        " * @brief The data of the core picture pipeline (post.c), written by",
        " * runtime/core/shaders/gen_post_data.py: the SPIR-V of each pass (glslc -O",
        " * from runtime/core/shaders) and SMAA's area (160x560, two bytes a texel)",
        " * and search (64x16, one byte a texel) textures from runtime/third_party/smaa.",
        " */",
        "#ifndef RELIFT_CORE_POST_DATA_H",
        "#define RELIFT_CORE_POST_DATA_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR", HERE)) as work:
        for name, src, defines in PASSES:
            words = spirv(src, defines, work)
            lines.append(f"static const uint32_t rl_post_spv_{name}[] = {{")
            lines += rows(words, 8, lambda v: f"0x{v:08x}")
            lines += ["};", ""]
    area = texture("AreaTex.h", "areaTexBytes")
    search = texture("SearchTex.h", "searchTexBytes")
    if len(area) != 160 * 560 * 2 or len(search) != 64 * 16:
        sys.exit(f"unexpected texture sizes {len(area)} {len(search)}")
    for name, data in (("area", area), ("search", search)):
        lines.append(f"static const uint8_t rl_post_smaa_{name}[] = {{")
        lines += rows(data, 24, str)
        lines += ["};", ""]
    lines += ["#endif", ""]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
