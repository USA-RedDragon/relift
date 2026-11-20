#ifndef THROW_LIB_H
#define THROW_LIB_H

struct LibError {
    int code;
};

void lib_throw(int depth);
void lib_throw_std(const char *what);
int lib_catch_inside(int depth);

#endif
