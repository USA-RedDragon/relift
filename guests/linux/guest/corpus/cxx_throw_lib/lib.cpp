#include "lib.h"

#include <cstdio>
#include <stdexcept>
#include <string>

struct Guard {
    int depth;
    explicit Guard(int d) : depth(d) { std::printf("lib: enter %d\n", depth); }
    ~Guard() { std::printf("lib: leave %d\n", depth); }
};

void lib_throw(int depth)
{
    Guard g(depth);
    if (depth == 0)
        throw LibError{99};
    lib_throw(depth - 1);
}

void lib_throw_std(const char *what) { throw std::runtime_error(std::string("lib says ") + what); }

int lib_catch_inside(int depth)
{
    try {
        lib_throw(depth);
    } catch (const LibError &e) {
        std::printf("lib: caught %d inside\n", e.code);
        return e.code + 1;
    }
    return 0;
}
