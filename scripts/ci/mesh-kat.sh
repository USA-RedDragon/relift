#!/usr/bin/env bash
# Draw the recorded task and mesh known answers on the pinned lavapipe with
# the native and compute back ends; the two images must agree word for word.
# Then run the recorded compute known answers there for every host preset.
. "$(dirname "$0")/lib.sh"

icd=$(lavapipe_icd)
spirv_val=$(spirv_val_dir)
cargo build --locked --release -p relift-gpu-sm86 --example mesh --example xlate
target=${CARGO_TARGET_DIR:-$ROOT/target}
PATH="$spirv_val:$PATH" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd" CARGO_TARGET_DIR="$target" \
  KAT_DEV=llvmpipe KAT_HOST=lavapipe \
  python3 -I gpus/sm86/kat/mesh/kat.py "$CI_WORK/mesh-kat" --replay gpus/sm86/tests/kat/mesh.txt
PATH="$spirv_val:$PATH" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd" CARGO_TARGET_DIR="$target" \
  KAT_DEVS=llvmpipe \
  python3 -I gpus/sm86/kat/compute/kat.py "$CI_WORK/compute-kat" --replay gpus/sm86/tests/kat/compute.txt
