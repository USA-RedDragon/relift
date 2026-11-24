# runtime/gpuapi

GPU-API forwarding: a guest's Vulkan, OpenGL, GLX and EGL calls run on the
host's libraries and driver. The generated C comes from
`relift-vkgen` (`gpus/apigen`) and the pinned Khronos registries; the
rest is hand-written. Design: `.work/design/017-gpu-api-forwarding.md`.

| Path | What |
|---|---|
| `vk/gen/i386/` | Generated from `gpus/apigen/vk-i386.toml`: Vulkan 1.0 to 1.3 thunks and converters for an i386 guest (`vki.c`), the shared types (`vkg_types.h`) and the i386 layouts as static asserts (`vki_layout.c`) |
| `gl/gen/i386/` | Generated from `gpus/apigen/gl.toml` (gl.xml and glx.xml): OpenGL 1.0 to 2.1 thunks and the GLX table (`gl.c`) |
| `egl/gen/i386/` | Generated from `gpus/apigen/egl.toml` (egl.xml): EGL 1.5 thunks (`egl.c`) |
| `i386/tg.h`, `i386/tg.c` | The i386 core: per-call state, argument slots, pointers, strings, handle tokens, callback pools, host binding, calls into the guest's lifted libraries, the `tg_lib` table the Linux guest layer binds |
| `i386/gl_hand.c` | Buffer offsets in pointer arguments (`tg_gl_offset`, decided by the bound buffer) and buffer mapping through a guest copy |
| `i386/glx_hand.c` | Every GLX command: the bridge from the guest's lifted X11 display and visuals to a host connection |
| `i386/egl_hand.c` | eglGetDisplay, eglGetPlatformDisplay (the guest's X11 display), eglGetProcAddress (guest thunk addresses) |
| `i386/vulkan_hand.c` | vkGetInstanceProcAddr and vkGetDeviceProcAddr (guest thunk addresses); vkMapMemory fails until the guest layer places host mappings in the guest window |
| `i386/tests/` | `gl_check.sh` (GL over EGL), `glx_check.sh` (GL over GLX on Xvfb), `vulkan_check.sh`: each scenario built natively for i386 and through the thunks, reports compared (`scripts/ci/tests-gpuapi.sh`) |

Regenerate after a change to `gpus/apigen` or a configuration:

```sh
cargo run -p relift-gpu-apigen -- --xml vk.xml --config gpus/apigen/vk-i386.toml --out runtime/gpuapi/vk/gen/i386
cargo run -p relift-gpu-apigen -- --xml gl.xml --xml glx.xml --config gpus/apigen/gl.toml --out runtime/gpuapi/gl/gen/i386
cargo run -p relift-gpu-apigen -- --xml egl.xml --config gpus/apigen/egl.toml --out runtime/gpuapi/egl/gen/i386
```

The registry files are at the URLs and sha256 sums in each configuration;
`scripts/ci/lint.sh` fetches them and checks every output is fresh.
