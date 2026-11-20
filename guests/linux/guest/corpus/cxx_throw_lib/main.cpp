#include "lib.h"

#include <cstdio>
#include <exception>
#include <stdexcept>

struct Local {
    ~Local() { std::puts("main: Local destroyed"); }
};

int main()
{
    try {
        Local l;
        lib_throw(2);
    } catch (const LibError &e) {
        std::printf("main: caught LibError %d\n", e.code);
    }
    try {
        lib_throw_std("hello");
    } catch (const std::exception &e) {
        std::printf("main: caught %s\n", e.what());
    }
    std::printf("inside: %d\n", lib_catch_inside(1));
    try {
        throw std::logic_error("from main");
    } catch (const std::logic_error &e) {
        std::printf("main: caught %s\n", e.what());
    }
    return 5;
}
