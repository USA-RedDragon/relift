# dav1d configuration

The headers dav1d's build generates, for dav1d 1.5.3 configured with both
bit depths, no assembly (`-Denable_asm=false`), no run-time CPU detection,
no logging and no tools. The AV1 decoder is plain C: it needs no SIMD and
gives the same pictures on every host relift targets, iOS included (AV1
decoding is exact integer arithmetic). dav1d is BSD-2-Clause licensed; its
license is in `NOTICE`. `relift build` passes this directory and the dav1d
source tree (`runtime/tools/dav1d_src.py`) as include paths, and compiles
the files that depend on the bit depth once for 8 and once for 16 bits.
