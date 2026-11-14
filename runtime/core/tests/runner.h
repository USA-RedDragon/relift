/**
 * @file runner.h
 * @brief A tiny test runner shared by every runtime layer: TEST(name)
 * registers a test; each test runs in its own forked process with a fresh
 * runtime and an alarm.
 */
#ifndef RELIFT_RUNNER_H
#define RELIFT_RUNNER_H

#include "core.h"

#include <stdio.h>
#include <string.h>

/** @brief A test body. */
typedef void t_fn(void);

/** @brief Registers a test (called from constructors). */
void t_register(const char *name, t_fn *fn);
/** @brief Records a failure in the running test. */
void t_fail(const char *file, int line, const char *expr, unsigned long long a, unsigned long long b, int has_values);

/** @brief Defines and registers a test. */
#define TEST(name)                                                              \
    static void name(void);                                                     \
    __attribute__((constructor)) static void name##_register(void)              \
    {                                                                           \
        t_register(#name, name);                                                \
    }                                                                           \
    static void name(void)

/** @brief Fails the test and returns when cond is false. */
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            t_fail(__FILE__, __LINE__, #cond, 0, 0, 0);                         \
            return;                                                             \
        }                                                                       \
    } while (0)

/** @brief Fails the test and returns when a != b, printing both. */
#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        unsigned long long t_a_ = (unsigned long long)(a);                      \
        unsigned long long t_b_ = (unsigned long long)(b);                      \
        if (t_a_ != t_b_) {                                                     \
            t_fail(__FILE__, __LINE__, #a " == " #b, t_a_, t_b_, 1);            \
            return;                                                             \
        }                                                                       \
    } while (0)

/** @brief Guest memory base of the running test (rl_mem_base). */
uint8_t *t_mem(void);

/** @brief Result of an access probe. */
typedef struct t_probe {
    /** @brief True when the access faulted. */
    bool faulted;
    /** @brief Guest fault address when faulted. */
    uint64_t addr;
} t_probe;

/** @brief Reads one byte at guest address a, catching a fault. */
t_probe t_read(uint64_t a);
/** @brief Writes one byte at guest address a, catching a fault. */
t_probe t_write(uint64_t a, uint8_t v);

#endif
