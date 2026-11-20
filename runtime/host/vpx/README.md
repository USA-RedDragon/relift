# libvpx configuration

The headers libvpx's `configure` generates, for libvpx 1.17.0 configured as

    configure --target=generic-gnu --disable-vp8 --disable-vp9-encoder \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-multithread --disable-runtime-cpu-detect

so the VP9 decoder is plain C: no assembly, no SIMD, no CPU detection and
no threads. The output does not depend on the host (VP9 decoding is exact
integer arithmetic), and the same files serve every host relift targets,
iOS included. libvpx is BSD-3-Clause licensed; these files carry its
notice. `relift build` passes this directory and the libvpx source tree
(fetched into the cache by the first Switch build) as include paths.
