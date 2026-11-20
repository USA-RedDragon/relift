#include <stdio.h>

__attribute__((noinline)) static int classify(int x)
{
    switch (x) {
    case 0: return 10;
    case 1: return 21;
    case 2: return 32;
    case 3: return 43;
    case 4: return 54;
    case 5: return 65;
    case 6: return 76;
    case 7: return 87;
    case 8: return 98;
    case 9: return 109;
    case 10: return 120;
    case 12: return 142;
    case 13: return 153;
    case 15: return 175;
    default: return -1;
    }
}

__attribute__((noinline)) static const char *name(unsigned x)
{
    switch (x) {
    case 0: return "zero";
    case 1: return "one";
    case 2: return "two";
    case 3: return "three";
    case 4: return "four";
    case 5: return "five";
    case 6: return "six";
    case 7: return "seven";
    default: return "many";
    }
}

__attribute__((noinline)) static int fall(int x)
{
    int r = 0;
    switch (x) {
    case 0: r += 1;
    case 1: r += 2;
    case 2: r += 4;
    case 3: r += 8; break;
    case 4: r += 16;
    case 5: r += 32; break;
    case 6: r = 64;
    }
    return r;
}

int main(void)
{
    int sum = 0;
    for (int i = -2; i < 20; i++)
        sum = sum * 3 + classify(i);
    printf("sum=%d\n", sum);
    for (unsigned i = 0; i < 10; i++)
        printf("%s%c", name(i), i == 9 ? '\n' : ',');
    for (int i = 0; i < 8; i++)
        printf("%d ", fall(i));
    putchar('\n');
    return 0;
}
