/**
 * @file soload.c
 * @brief Runs an android-arm64 build of a Linux project from a shell on the device, for tests: relift builds such a
 * project into an app library and packages no app, and its output in an app would be lost. soload loads the library
 * and calls its exported main with the arguments after the library's path, with the shell's stdin, stdout and stderr.
 *
 *     $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang -O2 -o soload soload.c -ldl
 *     adb push soload lib<name>.so libSDL3.so libadrenotools.so /data/local/tmp/t/
 *     adb shell 'cd /data/local/tmp/t && LD_LIBRARY_PATH=. ./soload ./lib<name>.so args...'
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: soload <lib.so> [args...]\n");
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "soload: %s\n", dlerror());
        return 3;
    }
    void *sym = dlsym(h, "main");
    if (!sym) {
        fprintf(stderr, "soload: %s has no main\n", argv[1]);
        return 4;
    }
    int (*run)(int, char **);
    *(void **)&run = sym;
    return run(argc - 1, argv + 1);
}
