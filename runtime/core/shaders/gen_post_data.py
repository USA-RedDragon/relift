"""gen_post_data.py: write runtime/core/post_data.h, the data of the core
picture pipeline: the SPIR-V of every pass in this folder (compiled with
glslc from shaderc, FSR 1's with runtime/third_party/fsr1) and SMAA's area
and search textures, read from runtime/third_party/smaa; and
runtime/core/fsr2_data.h, FSR 2's passes from runtime/third_party/fsr2 with
the descriptors each one reads. Run it from any
folder after changing a shader or the vendored files; it needs glslc on PATH.
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
FSR1 = os.path.join(CORE, "..", "third_party", "fsr1")
FSR2 = os.path.join(CORE, "..", "third_party", "fsr2")
OUT = os.path.join(CORE, "post_data.h")
OUT_FSR2 = os.path.join(CORE, "fsr2_data.h")

FSR2_PASSES = [
    "compute_luminance_pyramid",
    "reconstruct_previous_depth",
    "depth_clip",
    "lock",
    "accumulate",
    "rcas",
]
FSR2_OPTIONS = ["HDR_COLOR_INPUT", "LOW_RESOLUTION_MOTION_VECTORS", "INVERTED_DEPTH", "APPLY_SHARPENING"]

PASSES = [
    ("fxaa", "fxaa.comp", []),
    ("smaa_edges", "smaa_edges.comp", []),
    ("smaa_weights", "smaa_weights.comp", []),
    ("smaa_weights_fast", "smaa_weights.comp", ["-DSMAA_DISABLE_DIAG_DETECTION", "-DSMAA_DISABLE_CORNER_DETECTION"]),
    ("smaa_blend", "smaa_blend.comp", []),
    ("upscale", "upscale.comp", []),
    ("upscale_buffers", "upscale.comp", ["-DRL_BUFFERS"]),
    ("fsr1", "fsr1.comp", []),
    ("fsr1_buffers", "fsr1.comp", ["-DRL_BUFFERS"]),
]


def spirv(src, defines, work):
    out = os.path.join(work, "pass.spv")
    cmd = ["glslc", "--target-env=vulkan1.0", "-O", "-I", HERE, "-I", SMAA, "-I", FSR1, *defines, os.path.join(HERE, src), "-o", out]
    subprocess.run(cmd, check=True)
    with open(out, "rb") as f:
        data = f.read()
    return struct.unpack(f"<{len(data) // 4}I", data)


def fsr2_spirv(name, options, optimize, work):
    out = os.path.join(work, "fsr2.spv")
    defines = ["-DFFX_GLSL=1", "-DFFX_GPU=1", "-DFFX_HALF=0", "-DFFX_FSR2_OPTION_JITTERED_MOTION_VECTORS=1"]
    for i, opt in enumerate(FSR2_OPTIONS):
        defines.append(f"-DFFX_FSR2_OPTION_{opt}={(options >> i) & 1}")
    src = os.path.join(FSR2, "shaders", f"ffx_fsr2_{name}_pass.glsl")
    cmd = ["glslc", "--target-env=vulkan1.1", "-O" if optimize else "-O0", "-fshader-stage=comp", *defines, src, "-o", out]
    subprocess.run(cmd, check=True)
    with open(out, "rb") as f:
        data = f.read()
    return struct.unpack(f"<{len(data) // 4}I", data)


def reflect(words):
    """The descriptors of a SPIR-V module: (set, binding, Vulkan descriptor type, variable name), by set and binding."""
    names, deco, types, var_type = {}, {}, {}, {}
    i = 5
    while i < len(words):
        op, count = words[i] & 0xFFFF, words[i] >> 16
        args = words[i + 1 : i + count]
        if op == 5:
            raw = struct.pack(f"<{len(args) - 1}I", *args[1:])
            names[args[0]] = raw.split(b"\0")[0].decode()
        elif op == 71 and args[1] in (33, 34):
            deco.setdefault(args[0], {})[args[1]] = args[2]
        elif op in (25, 26, 30, 32):
            types[args[0]] = (op, args)
        elif op == 59:
            var_type[args[1]] = args[0]
        i += count
    out = []
    for var, d in deco.items():
        if 33 not in d:
            continue
        _, ptr = types[var_type[var]]
        op, args = types[ptr[2]]
        if op == 25:
            kind = 3 if args[6] == 2 else 2
        elif op == 26:
            kind = 0
        else:
            kind = 6
        out.append((d.get(34, 0), d[33], kind, names.get(var, "")))
    return sorted(out)


def fsr2(work):
    """The lines of fsr2_data.h."""
    blobs, index, tables, names = [], {}, [], []
    table = []
    for name in FSR2_PASSES:
        row = []
        for options in range(1 << len(FSR2_OPTIONS)):
            words = fsr2_spirv(name, options, True, work)
            if words not in index:
                used = {(s, b) for s, b, _, _ in reflect(words)}
                named = {(s, b): (k, n) for s, b, k, n in reflect(fsr2_spirv(name, options, False, work))}
                binds = []
                for key in sorted(used):
                    kind, var = named[key]
                    if var not in names:
                        names.append(var)
                    binds.append((key[0], key[1], kind, names.index(var)))
                index[words] = len(blobs)
                blobs.append(words)
                tables.append(binds)
            row.append(index[words])
        table.append(row)
    lines = [
        "/**",
        " * @file fsr2_data.h",
        " * @brief The data of FSR 2 (fsr2.c), written by runtime/core/shaders/gen_post_data.py from",
        " * runtime/third_party/fsr2: the SPIR-V of its passes (glslc -O, 32-bit floats, the jittered motion vector",
        " * path compiled in) for every combination of the options each one reads, and the descriptors each module",
        " * reads.",
        " */",
        "#ifndef RELIFT_CORE_FSR2_DATA_H",
        "#define RELIFT_CORE_FSR2_DATA_H",
        "",
        "#include <stdint.h>",
        "",
        "/** @brief One descriptor of a module: set, binding, VkDescriptorType and the index of its name. */",
        "typedef struct rl_fsr2_bind {",
        "    uint8_t set, binding, type, name;",
        "} rl_fsr2_bind;",
        "",
        f"#define RL_FSR2_PASSES {len(FSR2_PASSES)}",
        f"#define RL_FSR2_OPTIONS {1 << len(FSR2_OPTIONS)}",
        f"#define RL_FSR2_MODULES {len(blobs)}",
        f"#define RL_FSR2_NAMES {len(names)}",
        "",
        "/** @brief The resource names the modules read, as the shaders call them. */",
        "static const char *const rl_fsr2_names[RL_FSR2_NAMES] = {",
    ]
    lines += [f'    "{n}",' for n in names]
    lines += ["};", ""]
    for i, words in enumerate(blobs):
        lines.append(f"static const uint32_t rl_fsr2_spv_{i}[] = {{")
        lines += rows(words, 8, lambda v: f"0x{v:08x}")
        lines += ["};", ""]
        lines.append(f"static const rl_fsr2_bind rl_fsr2_binds_{i}[] = {{")
        lines += [f"    {{ {s}, {b}, {k}, {n} }}," for s, b, k, n in tables[i]]
        lines += ["};", ""]
    lines += [
        "/** @brief A module: its SPIR-V and its descriptors. */",
        "typedef struct rl_fsr2_module {",
        "    const uint32_t *code;",
        "    uint32_t size;",
        "    const rl_fsr2_bind *binds;",
        "    uint32_t nbinds;",
        "} rl_fsr2_module;",
        "",
        "static const rl_fsr2_module rl_fsr2_modules[RL_FSR2_MODULES] = {",
    ]
    lines += [
        f"    {{ rl_fsr2_spv_{i}, sizeof rl_fsr2_spv_{i}, rl_fsr2_binds_{i}, {len(tables[i])} }},"
        for i in range(len(blobs))
    ]
    lines += [
        "};",
        "",
        "/**",
        " * @brief The module of each pass (compute_luminance_pyramid, reconstruct_previous_depth, depth_clip, lock,",
        " * accumulate, rcas) for options bit 0 HDR color, bit 1 motion vectors at render size, bit 2 inverted depth,",
        " * bit 3 sharpening (an rcas pass after accumulate).",
        " */",
        "static const uint8_t rl_fsr2_pass_module[RL_FSR2_PASSES][RL_FSR2_OPTIONS] = {",
    ]
    lines += ["    { " + ", ".join(str(v) for v in row) + " }," for row in table]
    lines += ["};", "", "#endif", ""]
    return lines


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
        fsr2_lines = fsr2(work)
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
    with open(OUT_FSR2, "w", encoding="utf-8") as f:
        f.write("\n".join(fsr2_lines))


if __name__ == "__main__":
    main()
