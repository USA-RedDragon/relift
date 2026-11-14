/**
 * @file frames.c
 * @brief A call chain named the way relift names lifted functions, for the
 * gdb frame filter test: main calls guest::main::fn_00123450, which calls
 * guest::main::bar(int), which calls guest::main::foo().
 */
#include <stdio.h>

int _ZN5guest4main3fooEv(void);
int _ZN5guest4main3barEi(int x);
int _ZN5guest4main11fn_00123450E(int x);

/** @brief The innermost frame; the test stops here. */
__attribute__((noinline)) int _ZN5guest4main3fooEv(void) {
    static volatile int calls;
    return ++calls;
}

/** @brief A frame with a parameter. */
__attribute__((noinline)) int _ZN5guest4main3barEi(int x) {
    return _ZN5guest4main3fooEv() + x;
}

/** @brief A frame with an address fallback name. */
__attribute__((noinline)) int _ZN5guest4main11fn_00123450E(int x) {
    return _ZN5guest4main3barEi(x) + 1;
}

int main(void) {
    printf("%d\n", _ZN5guest4main11fn_00123450E(2));
    return 0;
}
