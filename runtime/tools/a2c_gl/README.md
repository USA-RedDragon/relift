# a2c_gl

Measures an NVIDIA GPU's alpha to coverage through OpenGL, with the dither
picked by GL_NV_alpha_to_coverage_dither_control. NVN's multisample state
picks the same dither: on is NVN's default, the 2x2 footprint; off is 1x1.
The output is the table format of the runtime's alpha to coverage test
(`RELIFT_A2C_TABLE`, test_nvn_a2c.c). For each sample count and alpha step
of 255 it gives the mask of each pixel of an 8x8 target, rows first. Then it
gives the least alpha (float bits) of each coverage level, found by
bisection.

Build and run on Linux with the NVIDIA driver (headless, EGL device):

    clang -std=c11 -O2 tools/a2c_gl/a2c_gl.c -lEGL -o a2c_gl
    ./a2c_gl on > rtx3090-dither-on.tsv
    ./a2c_gl off > rtx3090-dither-off.tsv

The two tables here are the RTX 3090's (NVIDIA 615.78.08) for 2, 4, 8 and 16
samples. Its OpenGL driver gives 2 samples when asked for 1, so it has no
1-sample table. Dither on matches the table the Vulkan test measures, mask
for mask and threshold for threshold.
