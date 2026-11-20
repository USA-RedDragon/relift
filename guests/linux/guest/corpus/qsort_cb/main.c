#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item {
    char name[12];
    int key;
    double weight;
};

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_item(const void *a, const void *b)
{
    const struct item *x = a, *y = b;
    if (x->key != y->key)
        return x->key - y->key;
    return strcmp(x->name, y->name);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

typedef int (*binop)(int, int);
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int sub(int a, int b) { return a - b; }
static const binop table[] = { add, mul, sub };

int main(void)
{
    int v[200];
    unsigned s = 12345;
    for (int i = 0; i < 200; i++) {
        s = s * 1103515245u + 12345u;
        v[i] = (int)(s >> 16) % 1000;
    }
    qsort(v, 200, sizeof v[0], cmp_int);
    long chk = 0;
    for (int i = 0; i < 200; i++)
        chk = chk * 31 + v[i];
    printf("sorted: first=%d last=%d chk=%ld\n", v[0], v[199], chk);
    int key = v[77];
    int *hit = bsearch(&key, v, 200, sizeof v[0], cmp_int);
    printf("bsearch found=%d\n", hit != NULL);
    struct item items[40];
    for (int i = 0; i < 40; i++) {
        snprintf(items[i].name, sizeof items[i].name, "item%02d", (i * 17) % 40);
        items[i].key = (i * 7) % 5;
        items[i].weight = i / 4.0;
    }
    qsort(items, 40, sizeof items[0], cmp_item);
    for (int i = 0; i < 6; i++)
        printf("%s %d %.2f\n", items[i].name, items[i].key, items[i].weight);
    const char *words[] = { "pear", "apple", "fig", "banana", "cherry", "date", "elderberry", "grape" };
    qsort(words, 8, sizeof words[0], cmp_str);
    for (int i = 0; i < 8; i++)
        printf("%s%c", words[i], i == 7 ? '\n' : ' ');
    int acc = 1;
    for (int i = 0; i < 9; i++)
        acc = table[i % 3](acc, i + 2);
    printf("table acc=%d\n", acc);
    return 0;
}
