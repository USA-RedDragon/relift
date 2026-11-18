"""gen_post_data.py: write runtime/core/post_data.h, the SPIR-V of every pass
of the core picture pipeline in this folder, compiled with glslc (shaderc).
Run it from any folder after changing a shader; it needs glslc on PATH.
"""

import os
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.dirname(HERE)
OUT = os.path.join(CORE, "post_data.h")

PASSES = [
    ("fxaa", "fxaa.comp", []),
]


def spirv(src, defines, work):
    out = os.path.join(work, "pass.spv")
    cmd = ["glslc", "--target-env=vulkan1.0", "-O", "-I", HERE, *defines, os.path.join(HERE, src), "-o", out]
    subprocess.run(cmd, check=True)
    with open(out, "rb") as f:
        data = f.read()
    return struct.unpack(f"<{len(data) // 4}I", data)


def rows(values, per, fmt):
    for i in range(0, len(values), per):
        yield "    " + ", ".join(fmt(v) for v in values[i : i + per]) + ","


def main():
    lines = [
        "/**",
        " * @file post_data.h",
        " * @brief The SPIR-V of each pass of the core picture pipeline (post.c),",
        " * written by runtime/core/shaders/gen_post_data.py (glslc -O).",
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
    lines += ["#endif", ""]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
