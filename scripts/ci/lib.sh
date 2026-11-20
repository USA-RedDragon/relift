# Shared by every scripts/ci job. Sourced, not run.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

CI_WORK=${CI_WORK:-${RUNNER_TEMP:-${XDG_CACHE_HOME:-$HOME/.cache}/relift-ci}}
mkdir -p "$CI_WORK"
export RELIFT_REPORT_DIR=${RELIFT_REPORT_DIR:-$CI_WORK/reports}

# The docker run options that keep a container on the cores all.sh gave the
# job (CI_CPUSET); empty when the job has the whole host.
DOCKER_CPUS=()
[ -z "${CI_CPUSET:-}" ] || DOCKER_CPUS=(--cpuset-cpus "$CI_CPUSET")

ACTIONLINT_VERSION=1.7.12
ACTIONLINT_SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
CARGO_DENY_VERSION=0.20.2
CARGO_DENY_SHA256=9f12ed4c49936e09b48bf862b595cde2fe64fcbd9d74dfacac6131ca824c8d5f
PANDOC_VERSION=3.13
PANDOC_SHA256=38aa33b325a2bc96a4c39409631212db55702f564250bcdbead180b3bf646389
APPIMAGE_RUNTIME_TAG=20251108
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
SPIRV_TOOLS_VERSION=2026.4
SPIRV_TOOLS_SHA256=92df4c377603f086c3d2a1eb2bd9b8f883a73ca890fb939d78a0cecdcc01686c
# The SPIRV-Headers commit SPIRV-Tools pins in its DEPS file for that release.
SPIRV_HEADERS_COMMIT=496543121ce6419f23d6fa5d7194ba66c36212d2
SPIRV_HEADERS_SHA256=a9bb9c48713245eacf97cc539b6f1d45405a92d8813f8b82e635f0a085ee9898
VULKAN_HEADERS_VERSION=1.4.363
VULKAN_HEADERS_SHA256=cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68

# fetch URL SHA256 OUT: download once into the work dir and check the hash.
fetch() {
  local url=$1 sum=$2 out=$3
  if [ ! -f "$out" ] || ! echo "$sum  $out" | sha256sum -c - >/dev/null 2>&1; then
    mkdir -p "$(dirname "$out")"
    curl -fsSL -o "$out.part" "$url"
    echo "$sum  $out.part" | sha256sum -c -
    mv "$out.part" "$out"
  fi
}

# vulkan_include_dir: unpack the pinned Vulkan-Headers once into the work dir
# and print its include directory, so the known-answer programs do not depend
# on the distribution's headers.
vulkan_include_dir() {
  local dir="$CI_WORK/vulkan-headers-$VULKAN_HEADERS_VERSION"
  if [ ! -f "$dir/include/vulkan/vulkan.h" ]; then
    fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$VULKAN_HEADERS_VERSION.tar.gz" \
      "$VULKAN_HEADERS_SHA256" "$dir.tgz" >&2
    rm -rf "$dir"
    mkdir -p "$dir"
    tar -xzf "$dir.tgz" -C "$dir" --strip-components=1
  fi
  echo "$dir/include"
}

# spirv_val_dir: build the pinned spirv-val once into the work dir and
# print the directory that holds it.
spirv_val_dir() {
  local dir="$CI_WORK/spirv-tools-$SPIRV_TOOLS_VERSION"
  if [ ! -x "$dir/build/tools/spirv-val" ]; then
    fetch "https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/v$SPIRV_TOOLS_VERSION.tar.gz" \
      "$SPIRV_TOOLS_SHA256" "$dir/tools.tgz" >&2
    fetch "https://github.com/KhronosGroup/SPIRV-Headers/archive/$SPIRV_HEADERS_COMMIT.tar.gz" \
      "$SPIRV_HEADERS_SHA256" "$dir/headers.tgz" >&2
    rm -rf "$dir/src" "$dir/headers" "$dir/build"
    mkdir -p "$dir/src" "$dir/headers"
    tar -xzf "$dir/tools.tgz" -C "$dir/src" --strip-components=1
    tar -xzf "$dir/headers.tgz" -C "$dir/headers" --strip-components=1
    local gen=()
    command -v ninja >/dev/null && gen=(-G Ninja)
    cmake -S "$dir/src" -B "$dir/build" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DSPIRV-Headers_SOURCE_DIR="$dir/headers" -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF >/dev/null
    cmake --build "$dir/build" --target spirv-val -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" >/dev/null
  fi
  echo "$dir/build/tools"
}

# Lavapipe from a pinned Mesa release, for the GPU translator's known answers.
# Distribution packages lag: before 26.2 lavapipe draws nothing for the
# translated native mesh pipelines.
MESA_VERSION=26.3.0
MESA_SHA256=91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a
MESON_VERSION=1.9.1
MESON_SHA256=f824ab770c041a202f532f69e114c971918ed2daff7ea56583d80642564598d0
MESON_URL=https://files.pythonhosted.org/packages/9c/07/b48592d325cb86682829f05216e4efb2dc881762b8f1bafb48b57442307a/meson-$MESON_VERSION-py3-none-any.whl

# lavapipe_icd: build lavapipe once into the work dir and print its ICD
# manifest. Needs LLVM, ninja, bison, flex, glslang, python3 with mako and
# pyyaml, and the zlib, expat and zstd headers.
lavapipe_icd() {
  local dir="$CI_WORK/lavapipe-$MESA_SHA256"
  if [ ! -f "$dir/lvp_icd.json" ]; then
    fetch "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz" "$MESA_SHA256" "$dir/mesa.tar.xz" >&2
    fetch "$MESON_URL" "$MESON_SHA256" "$dir/meson.whl" >&2
    rm -rf "$dir/src" "$dir/build" "$dir/lib"
    mkdir -p "$dir/src" "$dir/lib"
    tar -xJf "$dir/mesa.tar.xz" -C "$dir/src" --strip-components=1
    printf '#!/usr/bin/env python3\nimport sys\nsys.path.insert(0, "%s")\nfrom mesonbuild.mesonmain import main\nsys.exit(main())\n' \
      "$dir/meson.whl" >"$dir/meson"
    chmod +x "$dir/meson"
    if ! (
      "$dir/meson" setup "$dir/build" "$dir/src" --buildtype=release -Dgallium-drivers=llvmpipe -Dvulkan-drivers=swrast \
        -Dplatforms= -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dgbm=disabled \
        -Dllvm=enabled -Dshared-llvm=enabled -Dvideo-codecs= -Dtools= -Dbuild-tests=false -Dvulkan-layers= &&
        ninja -C "$dir/build" src/gallium/targets/lavapipe/libvulkan_lvp.so
    ) >"$dir/build.log" 2>&1; then
      tail -n 40 "$dir/build.log" >&2
      return 1
    fi
    cp "$dir/build/src/gallium/targets/lavapipe/libvulkan_lvp.so" "$dir/lib/"
    rm -rf "$dir/src" "$dir/build" "$dir/build.log"
    printf '{"file_format_version": "1.0.1", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' \
      "$dir/lib/libvulkan_lvp.so" >"$dir/lvp_icd.json"
  fi
  echo "$dir/lvp_icd.json"
}
