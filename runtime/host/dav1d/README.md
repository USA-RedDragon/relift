# dav1d configuration

The headers dav1d's build generates, for dav1d 1.5.3 configured with both
bit depths, no logging and no tools. The top directory holds the plain C
configuration (`config.h`, no assembly, no run-time CPU detection): it
needs no SIMD and gives the same pictures on every host relift targets, iOS
included (AV1 decoding is exact integer arithmetic).

`x86_64/` is the configuration of the assembly build (`config.h` with
`HAVE_ASM 1`, and `config.asm`, the NASM defines dav1d's meson build writes).
`relift build` uses it on an x86-64 Linux host with NASM 2.14 or later on the
path: it assembles dav1d's `src/x86/*.asm` (SSE, AVX2 and AVX-512 versions of
the hot loops) and compiles `src/x86/cpu.c`, so dav1d picks the version for
the running CPU at run time and one binary is right on every x86-64 CPU.

`aarch64/` is the configuration of the arm64 assembly build, which an
`android-arm64` build uses: dav1d's `src/arm/64/*.S` (GNU assembler syntax,
NEON with dot product, i8mm and SVE versions) go through the NDK's clang,
with `src/arm/cpu.c` picking among them at run time from `getauxval`. A
host without NASM, and every other host, builds the plain C; the pictures
are identical, the decode is several times slower. dav1d is BSD-2-Clause licensed; its
license is in `NOTICE`. `relift build` passes this directory and the dav1d
source tree (fetched into the cache by the first Switch build) as include paths, and compiles
the files that depend on the bit depth once for 8 and once for 16 bits.
