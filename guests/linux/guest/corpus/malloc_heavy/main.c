#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t seed = 0x9e3779b97f4a7c15ull;

static uint32_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (uint32_t)(seed >> 11);
}

struct node {
    struct node *next;
    uint32_t size;
    uint32_t tag;
};

int main(void)
{
    enum { N = 2000 };
    static void *slots[N];
    static uint32_t sizes[N];
    uint64_t sum = 0;
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < N; i++) {
            if (slots[i]) {
                unsigned char *p = slots[i];
                for (uint32_t k = 0; k < sizes[i]; k += 7)
                    if (p[k] != (unsigned char)(i + k))
                        return 100 + round;
                if (rnd() % 3 == 0) {
                    uint32_t ns = rnd() % 3000 + 1;
                    p = realloc(p, ns);
                    if (!p)
                        return 90;
                    uint32_t keep = ns < sizes[i] ? ns : sizes[i];
                    for (uint32_t k = 0; k < keep; k += 7)
                        if (p[k] != (unsigned char)(i + k))
                            return 91;
                    for (uint32_t k = 0; k < ns; k++)
                        p[k] = (unsigned char)(i + k);
                    slots[i] = p;
                    sizes[i] = ns;
                } else {
                    free(p);
                    slots[i] = NULL;
                }
            } else {
                uint32_t sz = rnd() % 4000 + 1;
                unsigned char *p = (rnd() & 1) ? malloc(sz) : calloc(1, sz);
                if (!p)
                    return 92;
                for (uint32_t k = 0; k < sz; k++)
                    p[k] = (unsigned char)(i + k);
                slots[i] = p;
                sizes[i] = sz;
            }
            sum += sizes[i];
        }
    }
    size_t live = 0;
    for (int i = 0; i < N; i++)
        if (slots[i]) {
            live++;
            free(slots[i]);
        }
    size_t big = 8u << 20;
    unsigned char *b = malloc(big);
    memset(b, 0xab, big);
    uint64_t chk = 0;
    for (size_t i = 0; i < big; i += 4096)
        chk += b[i];
    b = realloc(b, big * 2);
    for (size_t i = 0; i < big; i += 4096)
        chk += b[i];
    free(b);
    struct node *head = NULL;
    for (uint32_t i = 0; i < 50000; i++) {
        struct node *n = malloc(sizeof *n);
        n->next = head;
        n->size = i;
        n->tag = i * 2654435761u;
        head = n;
    }
    uint64_t lsum = 0;
    while (head) {
        struct node *n = head;
        head = n->next;
        lsum += n->tag ^ n->size;
        free(n);
    }
    printf("live=%zu sum=%llu chk=%llu list=%llu\n", live, (unsigned long long)sum, (unsigned long long)chk, (unsigned long long)lsum);
    return 0;
}
