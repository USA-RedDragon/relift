"""gen_post_data.py: write runtime/core/post_data.h, the data of the core
picture pipeline: the SPIR-V of every pass in this folder (compiled with
glslc from shaderc, FSR 1's with runtime/third_party/fsr1) and SMAA's area
and search textures, read from runtime/third_party/smaa; and
runtime/core/fsr2_data.h and fsr3_data.h, the passes of FSR 2 and of the FSR 3
upscaler from runtime/third_party/fsr2 and fsr3 with the descriptors each one
reads. Run it from any
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
FSR3 = os.path.join(CORE, "..", "third_party", "fsr3")
OUT = os.path.join(CORE, "post_data.h")
OUT_FSR2 = os.path.join(CORE, "fsr2_data.h")
OUT_FSR3 = os.path.join(CORE, "fsr3_data.h")

FSR2_PASSES = [
    "compute_luminance_pyramid",
    "reconstruct_previous_depth",
    "depth_clip",
    "lock",
    "accumulate",
    "rcas",
]
FSR2_OPTIONS = ["HDR_COLOR_INPUT", "LOW_RESOLUTION_MOTION_VECTORS", "INVERTED_DEPTH", "APPLY_SHARPENING"]
FSR3_PASSES = [
    "prepare_inputs",
    "luma_pyramid",
    "shading_change_pyramid",
    "shading_change",
    "prepare_reactivity",
    "luma_instability",
    "accumulate",
    "rcas",
]
FSR3_OPTIONS = FSR2_OPTIONS

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
    ("taa", "taa.comp", []),
    ("taa_rcas", "taa_rcas.comp", []),
    ("changemask", "changemask.comp", []),
    ("overlay", "overlay.comp", []),
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


def ffx_modules(passes, noptions, compile_pass):
    """The deduplicated modules of passes over noptions option sets: SPIR-V blobs, their descriptor tables, the
    resource names they read, and the module of each pass and option set. compile_pass(name, options, optimize)
    gives a module's words; the unoptimized one keeps the names the optimized one drops."""
    blobs, index, tables, names = [], {}, [], []
    table = []
    for name in passes:
        row = []
        for options in range(noptions):
            words = compile_pass(name, options, True)
            if words not in index:
                used = {(s, b) for s, b, _, _ in reflect(words)}
                named = {(s, b): (k, n) for s, b, k, n in reflect(compile_pass(name, options, False))}
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
    return blobs, tables, names, table


def ffx_header(p, brief, bind_type, passes, options, modules, table_doc):
    """The lines of an FFX data header with prefix p (fsr2, fsr3) from ffx_modules' result."""
    blobs, tables, names, table = modules
    up = p.upper()
    lines = ["/**", f" * @file {p}_data.h", *brief, " */", f"#ifndef RELIFT_CORE_{up}_DATA_H", f"#define RELIFT_CORE_{up}_DATA_H"]
    lines += [
        "",
        "#include <stdint.h>",
        "",
        "/** @brief One descriptor of a module: set, binding, VkDescriptorType and the index of its name. */",
        f"typedef struct rl_{p}_bind {{",
        f"    {bind_type} set, binding, type, name;",
        f"}} rl_{p}_bind;",
        "",
        f"#define RL_{up}_PASSES {len(passes)}",
        f"#define RL_{up}_OPTIONS {1 << len(options)}",
        f"#define RL_{up}_MODULES {len(blobs)}",
        f"#define RL_{up}_NAMES {len(names)}",
        "",
        "/** @brief The resource names the modules read, as the shaders call them. */",
        f"static const char *const rl_{p}_names[RL_{up}_NAMES] = {{",
    ]
    lines += [f'    "{n}",' for n in names]
    lines += ["};", ""]
    for i, words in enumerate(blobs):
        lines.append(f"static const uint32_t rl_{p}_spv_{i}[] = {{")
        lines += rows(words, 8, lambda v: f"0x{v:08x}")
        lines += ["};", ""]
        lines.append(f"static const rl_{p}_bind rl_{p}_binds_{i}[] = {{")
        lines += [f"    {{ {s}, {b}, {k}, {n} }}," for s, b, k, n in tables[i]]
        lines += ["};", ""]
    lines += [
        "/** @brief A module: its SPIR-V and its descriptors. */",
        f"typedef struct rl_{p}_module {{",
        "    const uint32_t *code;",
        "    uint32_t size;",
        f"    const rl_{p}_bind *binds;",
        "    uint32_t nbinds;",
        f"}} rl_{p}_module;",
        "",
        f"static const rl_{p}_module rl_{p}_modules[RL_{up}_MODULES] = {{",
    ]
    lines += [
        f"    {{ rl_{p}_spv_{i}, sizeof rl_{p}_spv_{i}, rl_{p}_binds_{i}, {len(tables[i])} }},"
        for i in range(len(blobs))
    ]
    lines += ["};", "", "/**", *table_doc, " */", f"static const uint8_t rl_{p}_pass_module[RL_{up}_PASSES][RL_{up}_OPTIONS] = {{"]
    lines += ["    { " + ", ".join(str(v) for v in row) + " }," for row in table]
    lines += ["};", "", "#endif", ""]
    return lines


def fsr2(work):
    """The lines of fsr2_data.h."""
    modules = ffx_modules(FSR2_PASSES, 1 << len(FSR2_OPTIONS), lambda n, o, opt: fsr2_spirv(n, o, opt, work))
    brief = [
        " * @brief The data of FSR 2 (fsr2.c), written by runtime/core/shaders/gen_post_data.py from",
        " * runtime/third_party/fsr2: the SPIR-V of its passes (glslc -O, 32-bit floats, the jittered motion vector",
        " * path compiled in) for every combination of the options each one reads, and the descriptors each module",
        " * reads.",
    ]
    doc = [
        " * @brief The module of each pass (compute_luminance_pyramid, reconstruct_previous_depth, depth_clip, lock,",
        " * accumulate, rcas) for options bit 0 HDR color, bit 1 motion vectors at render size, bit 2 inverted depth,",
        " * bit 3 sharpening (an rcas pass after accumulate).",
    ]
    return ffx_header("fsr2", brief, "uint8_t", FSR2_PASSES, FSR2_OPTIONS, modules, doc)


def fsr3_spirv(name, options, optimize, work):
    out = os.path.join(work, "fsr3.spv")
    defines = [
        "-DFFX_GLSL=1",
        "-DFFX_GPU=1",
        "-DFFX_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=1",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
    ]
    for i, opt in enumerate(FSR3_OPTIONS):
        defines.append(f"-DFFX_FSR3UPSCALER_OPTION_{opt}={(options >> i) & 1}")
    src = os.path.join(FSR3, "shaders", f"ffx_fsr3upscaler_{name}_pass.glsl")
    inc = os.path.join(FSR3, "gpu")
    cmd = ["glslc", "--target-env=vulkan1.1", "-O" if optimize else "-O0", "-fshader-stage=comp", "-I", inc, *defines]
    subprocess.run([*cmd, src, "-o", out], check=True)
    with open(out, "rb") as f:
        data = f.read()
    return struct.unpack(f"<{len(data) // 4}I", data)


def fsr3(work):
    """The lines of fsr3_data.h."""
    modules = ffx_modules(FSR3_PASSES, 1 << len(FSR3_OPTIONS), lambda n, o, opt: fsr3_spirv(n, o, opt, work))
    brief = [
        " * @brief The data of the FSR 3 upscaler (fsr3.c), written by runtime/core/shaders/gen_post_data.py from",
        " * runtime/third_party/fsr3: the SPIR-V of its passes (glslc -O, 32-bit floats, the jittered motion vector",
        " * path compiled in, the approximate Lanczos upsample and the reference reprojection) for every combination",
        " * of the options each one reads, and the descriptors each module reads.",
    ]
    doc = [
        " * @brief The module of each pass (prepare_inputs, luma_pyramid, shading_change_pyramid, shading_change,",
        " * prepare_reactivity, luma_instability, accumulate, rcas) for options bit 0 HDR color, bit 1 motion vectors",
        " * at render size, bit 2 inverted depth, bit 3 sharpening (an rcas pass after accumulate).",
    ]
    return ffx_header("fsr3", brief, "uint16_t", FSR3_PASSES, FSR3_OPTIONS, modules, doc)


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
        fsr3_lines = fsr3(work)
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
    with open(OUT_FSR3, "w", encoding="utf-8") as f:
        f.write("\n".join(fsr3_lines))


if __name__ == "__main__":
    main()
