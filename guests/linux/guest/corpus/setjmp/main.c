#include <setjmp.h>
#include <stdio.h>

static jmp_buf env;
static sigjmp_buf senv;

static void thrower(int depth, int code)
{
    if (depth == 0)
        longjmp(env, code);
    thrower(depth - 1, code);
    puts("not reached");
}

static int try_once(int depth, int code)
{
    volatile int local = 7;
    int r = setjmp(env);
    if (r == 0) {
        local += 1;
        thrower(depth, code);
        return -1;
    }
    return r * 100 + local;
}

int main(void)
{
    printf("a=%d\n", try_once(0, 3));
    printf("b=%d\n", try_once(25, 9));
    printf("zero maps to one: %d\n", try_once(2, 0));
    jmp_buf inner;
    volatile int count = 0;
    int r = setjmp(inner);
    if (r < 5) {
        count++;
        longjmp(inner, r + 1);
    }
    printf("loop r=%d count=%d\n", r, count);
    int sr = sigsetjmp(senv, 1);
    if (sr == 0) {
        siglongjmp(senv, 42);
    }
    printf("sigsetjmp r=%d\n", sr);
    return 0;
}
