/**
 * @file spirvval.c
 * @brief See spirvval.h.
 */
#include "spirvval.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#if defined(RL_CHECKS) && !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
void rl_spirv_check(const uint32_t *w, size_t n, const char *what)
{
    const char *cmd = getenv(RL_SPIRV_VAL_ENV);
    if (!cmd || !*cmd || !w || !n)
        return;
    fflush(stdout);
    fflush(stderr);
    FILE *p = popen(cmd, "w");
    if (!p) {
        fprintf(stderr, "[relift error spirv] cannot start the SPIR-V validator %s\n", cmd);
        abort();
    }
    size_t put = fwrite(w, sizeof *w, n, p);
    int st = pclose(p);
    if (put != n || st != 0) {
        fprintf(stderr, "[relift error spirv] %s is not valid SPIR-V (%zu words; %s rejected it, status %d)\n",
                what ? what : "a module", n, cmd, st);
        abort();
    }
}
#else
void rl_spirv_check(const uint32_t *w, size_t n, const char *what)
{
    (void)w;
    (void)n;
    (void)what;
}
#endif
