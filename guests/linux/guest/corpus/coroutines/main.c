#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A coroutine library on a small amount of assembly: co_swap saves the
 * callee-saved registers on the running stack, stores the stack pointer in
 * *save, installs the stack pointer `to` and restores what is saved there.
 * A new coroutine starts at co_boot, which calls fn(arg).
 */
void co_swap(void **save, void *to);
void co_boot(void);

__asm__(".text\n"
        ".globl co_swap\n"
        ".type co_swap,%function\n"
        "co_swap:\n"
        "    sub sp, sp, #160\n"
        "    stp x19, x20, [sp, #0]\n"
        "    stp x21, x22, [sp, #16]\n"
        "    stp x23, x24, [sp, #32]\n"
        "    stp x25, x26, [sp, #48]\n"
        "    stp x27, x28, [sp, #64]\n"
        "    stp x29, x30, [sp, #80]\n"
        "    stp d8, d9, [sp, #96]\n"
        "    stp d10, d11, [sp, #112]\n"
        "    stp d12, d13, [sp, #128]\n"
        "    stp d14, d15, [sp, #144]\n"
        "    mov x9, sp\n"
        "    str x9, [x0]\n"
        "    mov sp, x1\n"
        "    ldp x19, x20, [sp, #0]\n"
        "    ldp x21, x22, [sp, #16]\n"
        "    ldp x23, x24, [sp, #32]\n"
        "    ldp x25, x26, [sp, #48]\n"
        "    ldp x27, x28, [sp, #64]\n"
        "    ldp x29, x30, [sp, #80]\n"
        "    ldp d8, d9, [sp, #96]\n"
        "    ldp d10, d11, [sp, #112]\n"
        "    ldp d12, d13, [sp, #128]\n"
        "    ldp d14, d15, [sp, #144]\n"
        "    add sp, sp, #160\n"
        "    ret\n"
        ".size co_swap, .-co_swap\n"
        ".globl co_boot\n"
        ".type co_boot,%function\n"
        "co_boot:\n"
        "    mov x0, x20\n"
        "    blr x19\n"
        "    brk #1\n"
        ".size co_boot, .-co_boot\n");

enum {
    SAVE_BYTES = 160,
    FP_SLOT = 80,
    STACK_BYTES = 32 * 1024,
    COUNT = 300,
    PASSES = 3,
    MOVED = 7,
};

struct co {
    void *sp;
    unsigned char *stack;
    int id;
    int done;
    uint64_t sum;
    long switches;
};

static struct co cos[COUNT];
static struct co *cur;
static void *sched_sp;

static void yield(void)
{
    struct co *c = cur;
    c->switches++;
    co_swap(&c->sp, sched_sp);
}

__attribute__((noinline)) static uint64_t descend(int depth, int id)
{
    if (depth == 0) {
        yield();
        return (uint64_t)id * 7 + 1;
    }
    uint64_t r = descend(depth - 1, id);
    if (depth % 16 == 0)
        yield();
    return r * 31 + (uint64_t)(depth ^ id);
}

static void body(void *arg)
{
    struct co *c = arg;
    for (int pass = 0; pass < PASSES; pass++)
        c->sum = c->sum * 1000003u + descend(120 + c->id % 60 + pass, c->id);
    c->done = 1;
    for (;;)
        yield();
}

static void start(struct co *c, int id, void (*fn)(void *))
{
    c->id = id;
    c->stack = malloc(STACK_BYTES);
    memset(c->stack, 0xa5, STACK_BYTES);
    uintptr_t top = ((uintptr_t)c->stack + STACK_BYTES) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - SAVE_BYTES);
    memset(frame, 0, SAVE_BYTES);
    frame[0] = (uint64_t)(uintptr_t)fn;
    frame[1] = (uint64_t)(uintptr_t)c;
    frame[11] = (uint64_t)(uintptr_t)co_boot;
    c->sp = frame;
}

static int inside(uintptr_t v, uintptr_t lo, uintptr_t hi) { return v >= lo && v < hi; }

/*
 * Moves a suspended coroutine's stack to a new buffer, as a runtime that
 * grows or shrinks stacks does: copies the bytes, rebases the saved stack
 * pointer and the chain of saved frame pointers, and poisons the old stack.
 */
static void move_stack(struct co *c)
{
    unsigned char *old = c->stack;
    unsigned char *fresh = malloc(STACK_BYTES);
    memcpy(fresh, old, STACK_BYTES);
    intptr_t delta = fresh - old;
    uintptr_t lo = (uintptr_t)old, hi = lo + STACK_BYTES;
    c->sp = (char *)c->sp + delta;
    uintptr_t *fp_slot = (uintptr_t *)((char *)c->sp + FP_SLOT);
    uintptr_t fp = *fp_slot;
    while (inside(fp, lo, hi)) {
        uintptr_t *at = (uintptr_t *)(fp + (uintptr_t)delta);
        *fp_slot = fp + (uintptr_t)delta;
        fp_slot = at;
        fp = *at;
    }
    c->stack = fresh;
    memset(old, 0xdd, STACK_BYTES);
    free(old);
}

static void resume(struct co *c)
{
    cur = c;
    co_swap(&sched_sp, c->sp);
}

static void ping(void *arg)
{
    struct co *c = arg;
    for (;;) {
        c->sum += (uint64_t)c->switches * 3 + 1;
        yield();
    }
}

int main(void)
{
    for (int i = 0; i < COUNT; i++)
        start(&cos[i], i, body);
    long rounds = 0;
    int moved = 0;
    for (;;) {
        int live = 0;
        for (int i = 0; i < COUNT; i++) {
            struct co *c = &cos[i];
            if (c->done)
                continue;
            live++;
            resume(c);
        }
        rounds++;
        if (!live)
            break;
        if (rounds == 5 || rounds == 9 || rounds == 14) {
            move_stack(&cos[MOVED]);
            moved++;
            if (rounds == 14)
                move_stack(&cos[COUNT - 1]);
        }
    }
    uint64_t total = 0;
    long switches = 0;
    for (int i = 0; i < COUNT; i++) {
        total ^= cos[i].sum + (uint64_t)i;
        switches += cos[i].switches;
    }
    printf("coroutines: %d, rounds: %ld, switches: %ld, stack moves: %d\n", COUNT, rounds, switches, moved + 1);
    printf("checksum: %016llx\n", (unsigned long long)total);
    printf("coroutine %d: %016llx\n", MOVED, (unsigned long long)cos[MOVED].sum);
    printf("coroutine %d: %016llx\n", COUNT - 1, (unsigned long long)cos[COUNT - 1].sum);

    struct co a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    start(&a, 1, ping);
    start(&b, 2, ping);
    for (int i = 0; i < 20000; i++) {
        resume(&a);
        resume(&b);
    }
    printf("ping: %llu %llu\n", (unsigned long long)a.sum, (unsigned long long)b.sum);
    return 0;
}
