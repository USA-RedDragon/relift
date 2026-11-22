# Recompile your first program

This step recompiles a small Linux arm64 program into a native program for this computer and runs it. The program is a C file of a few lines, so there is no game data to find and nothing to wait for. A game follows in [the next step](03-first-game.md).

## Make the program

relift reads a statically linked arm64 Linux executable. This file is `hello.c`, the same program the repository's Linux test corpus (`guests/linux/guest/corpus/hello`) builds:

```c
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argv;
    printf("hello from a static arm64 program, argc=%d\n", argc);
    fputs("to stderr\n", stderr);
    return 3;
}
```

Build it for arm64 Linux with `zig cc`, which cross compiles from any host:

```sh
zig cc -target aarch64-linux-musl -static -O2 -o hello hello.c
```

## Import it

Run `relift` in an empty project folder. `import` reads the executable and writes a `game.toml` for the project:

```sh
mkdir hello-demo && cd hello-demo
relift import ../hello
```

```text
imported hello (1411592 bytes, aarch64 Linux), into ~/.cache/relift/projects/hello-demo/import; wrote game.toml, 0.0 s
```

The program that `relift build` makes is named after the project: the `name` in the `[game]` table of `game.toml` when it has one, otherwise the name of the project folder, cut down to lowercase letters, digits and dashes. Here that is `hello-demo`. Add `name = "Hello Demo"` under `[game]` to choose another; the file becomes `hello-demo`, and the title people see (an app label, a window title) stays what the program says it is.

## Build and run

```sh
relift build
```

```text
built 64 functions, 0 instructions not lifted, 30.9 s in all
  output: ~/.cache/relift/projects/hello-demo/build/host/hello-demo
```

The output is a native program. It does not need relift, and it prints what the arm64 program would print and returns its exit status:

```sh
~/.cache/relift/projects/hello-demo/build/host/hello-demo
```

```text
hello from a static arm64 program, argc=1
to stderr
```

Its exit status is 3, which is what `hello.c` returns. `relift run` builds a development build when it needs one and runs it, so it is the short way to run the project:

```sh
relift run
```

`relift build` also packages the program for this host ([Package a game for each host](../guides/packaging.md)). Both commands keep their artifacts in the cache directory, which can be copied to another path; the next build reuses it ([Use relift](../guides/usage.md#projects-and-the-cache)).

Next: [Recompile your first game](03-first-game.md).
