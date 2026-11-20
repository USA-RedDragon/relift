#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argv;
    printf("hello from a static arm64 program, argc=%d\n", argc);
    fputs("to stderr\n", stderr);
    return 3;
}
