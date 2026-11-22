/**
 * @file test_spirvval.c
 * @brief rl_spirv_check passes a valid module and stops the process at an
 * invalid one, when the runner found a validator.
 */
#include "runner.h"

#include "spirvval.h"

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief The smallest valid compute module: an empty main with a local size of 1. */
static const uint32_t empty_compute[] = {
    0x07230203u, 0x00010600u, 0x00070000u, 0x00000005u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0005000fu, 0x00000005u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00060010u,
    0x00000001u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u,
    0x00000003u, 0x00000002u, 0x00050036u, 0x00000002u, 0x00000001u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000004u, 0x000100fdu, 0x00010038u,
};

/** @brief Whether a child that checks the n words at w exits normally. */
static bool child_passes(const uint32_t *w, size_t n)
{
    pid_t pid = fork();
    if (pid == 0) {
        rl_spirv_check(w, n, "the test module");
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

TEST(spirv_check_stops_at_an_invalid_module)
{
    if (!getenv(RL_SPIRV_VAL_ENV))
        return;
    CHECK(child_passes(empty_compute, sizeof empty_compute / 4));
    uint32_t bad[sizeof empty_compute / 4];
    memcpy(bad, empty_compute, sizeof bad);
    bad[sizeof bad / 4 - 2] = 0x00010000u;
    CHECK(!child_passes(bad, sizeof bad / 4));
}
