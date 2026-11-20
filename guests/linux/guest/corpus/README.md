# Linux guest corpus

Programs built from the C and C++ in this folder, run on real arm64 Linux
to record what a program prints and returns, and then lifted by relift and
run again to compare.

Each folder is one program: `main.c` (or `main.cpp`, built with `zig c++`),
and optionally

- `args`: one argument per line, after the program name; a blank line is an
  empty argument.
- `env`: one `NAME=value` per line, added to the environment.
- `variants`: extra build variants, one per line. Every program builds as
  `static` (`-O2`, fixed addresses); `pie` is a static-PIE build.
- `expected.stdout` and `expected.exit`: what real arm64 Linux gave, written
  by `record.sh`.

`build.sh <out-dir>` builds every program for aarch64 with musl, static
(`zig cc -target aarch64-linux-musl -static`), as `<out-dir>/<name>.<variant>`.
`record.sh <out-dir>` pushes each `static` build to the shared arm64 test
device in its own folder under `/data/local/tmp/relift-elf`, runs it and
writes the two expected files; it takes the lock `RELIFT_ADB_LOCK` names (default: a file under
`$XDG_RUNTIME_DIR` or the output directory) around every use of the
device, picks the device with `ANDROID_SERIAL`, and removes what it pushed. `run.sh <out-dir> <name> <variant>`
runs one build under `qemu-aarch64` when that is installed.
