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
With `AARCH64_GCC=aarch64-linux-gnu-gcc` set, `build.sh` also builds every program
with that compiler, statically against glibc, as `<name>.gnu`; they use the
same expected answers.
A program with a `dynamic_only` file is built dynamically only (it needs a
shared library, `dlopen` or the program interpreter); with a `lib.c` or
`lib.cpp` its shared library `lib<name>.so` is built beside it and the
program finds it through `$ORIGIN`; a `dlopen` file says the program loads
the library by name instead of linking it. Every `<name>.dyn` is the same
program linked against glibc's shared libraries, built with
`aarch64-linux-gnu-gcc`; `ONLY_DYNAMIC=1` skips the musl builds. `check.sh`
imports a `.dyn` with the sysroot of that compiler (`SYSROOT` overrides it).
`verify_dyn.sh <out-dir>` runs each `.dyn` build on the device, through the
sysroot's loader pushed with it, and compares with the recorded output.
`record.sh <out-dir> [name...]` pushes each named (default: every) `static` build to the shared arm64 test
device in its own folder under `/data/local/tmp/relift-elf`, runs it and
writes the two expected files; it takes the lock `RELIFT_ADB_LOCK` names (default: a file under
`$XDG_RUNTIME_DIR` or the output directory) around every use of the
device, picks the device with `ANDROID_SERIAL`, and removes what it pushed. `run.sh <out-dir> <name> <variant>`
runs one build under `qemu-aarch64` when that is installed.

An `etc` directory holds the system configuration the program reads
(`nsswitch.conf`, `passwd`, `group`, `hosts`): `check.sh` imports the
program with a sysroot that has it, and `record.sh` runs the program on the
device in a root directory (`chroot`) that holds the same files and the
libraries, since glibc reads these at fixed paths; `verify_dyn.sh` skips
such programs.

A `gconv` file marks a program that loads glibc's iconv plugins: the device
runs it with `GCONV_PATH` pointing at the sysroot's `gconv` directory pushed
beside it, and relift imports the plugin directory glibc spells with the
program's libraries.
