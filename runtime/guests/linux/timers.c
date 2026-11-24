/**
 * @file timers.c
 * @brief The interval timers of setitimer, run by the helper thread of
 * process.c.
 */
#include "linux_priv.h"

int64_t lx_itimer_run(void) { return -1; }

void lx_itimer_fork_child(void) {}
