#include <setjmp.h>
#include <stdio.h>

int _setjmp(jmp_buf);

static jmp_buf env;
static int (*volatile save)(jmp_buf) = _setjmp;
static void (*volatile jump)(jmp_buf, int) = longjmp;

typedef void (*thrower)(int);

static void raise_it(int code) { jump(env, code); }

static void deep(thrower t, int depth, int code)
{
    if (depth == 0)
        t(code);
    else
        deep(t, depth - 1, code);
    puts("not reached");
}

static int attempt(int depth, int code)
{
    volatile int local = 5;
    int r = save(env);
    if (r == 0) {
        local += 2;
        deep(raise_it, depth, code);
        return -1;
    }
    return r * 100 + local;
}

int main(void)
{
    printf("a=%d\n", attempt(0, 4));
    printf("b=%d\n", attempt(12, 8));
    int total = 0;
    for (int i = 1; i <= 5; i++)
        total += attempt(i, i);
    printf("total=%d\n", total);
    return 0;
}
