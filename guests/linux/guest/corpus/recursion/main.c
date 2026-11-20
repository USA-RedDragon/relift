#include <stdio.h>

static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static unsigned ack(unsigned m, unsigned n)
{
    if (m == 0)
        return n + 1;
    if (n == 0)
        return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}

static int is_odd(int n);
static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
static int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }

static void hanoi(int n, int from, int to, int via, long *moves)
{
    if (n == 0)
        return;
    hanoi(n - 1, from, via, to, moves);
    (*moves)++;
    hanoi(n - 1, via, to, from, moves);
}

__attribute__((noinline)) static long depth(long n)
{
    volatile char pad[64];
    pad[0] = (char)n;
    return n == 0 ? 0 : 1 + depth(n - 1) + (pad[0] & 0);
}

static long gcd(long a, long b) { return b ? gcd(b, a % b) : a; }

int main(void)
{
    long moves = 0;
    hanoi(16, 0, 2, 1, &moves);
    printf("fib(27)=%ld ack(2,3)=%u ack(3,5)=%u\n", fib(27), ack(2, 3), ack(3, 5));
    printf("even(1001)=%d odd(1001)=%d hanoi=%ld\n", is_even(1001), is_odd(1001), moves);
    printf("depth=%ld gcd=%ld\n", depth(20000), gcd(1071, 462));
    return 0;
}
