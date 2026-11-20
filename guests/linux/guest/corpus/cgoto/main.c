#include <stdio.h>

enum { PUSH, ADD, MUL, DUP, JNZ, DEC, SWAP, POP, PRINT, HALT };

static long run(const int *code)
{
    static const void *labels[] = { &&l_push, &&l_add, &&l_mul, &&l_dup, &&l_jnz, &&l_dec, &&l_swap, &&l_pop, &&l_print, &&l_halt };
    long stack[64];
    int sp = 0, pc = 0;
    long out = 0;
#define NEXT goto *labels[code[pc++]]
    NEXT;
l_push:
    stack[sp++] = code[pc++];
    NEXT;
l_add:
    sp--;
    stack[sp - 1] += stack[sp];
    NEXT;
l_mul:
    sp--;
    stack[sp - 1] *= stack[sp];
    NEXT;
l_dup:
    stack[sp] = stack[sp - 1];
    sp++;
    NEXT;
l_jnz:
    if (stack[sp - 1] != 0)
        pc = code[pc];
    else
        pc++;
    NEXT;
l_dec:
    stack[sp - 1]--;
    NEXT;
l_swap: {
    long t = stack[sp - 1];
    stack[sp - 1] = stack[sp - 2];
    stack[sp - 2] = t;
    NEXT;
}
l_pop:
    sp--;
    NEXT;
l_print:
    out = out * 7 + stack[sp - 1];
    NEXT;
l_halt:
    return out;
}

int main(void)
{
    const int prog[] = {
        PUSH, 1, PUSH, 12,
        SWAP, PUSH, 3, MUL, PUSH, 1, ADD, PRINT, SWAP,
        DEC, JNZ, 4,
        POP, PRINT, HALT,
    };
    printf("vm result %ld\n", run(prog));
    return 0;
}
