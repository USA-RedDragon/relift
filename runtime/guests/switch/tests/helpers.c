/**
 * @file helpers.c
 * @brief Shared helpers of the Switch guest tests: boot, svc calls, the heap
 * and guest function registration.
 */
#include "t.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int t_svc_rc;

static char g_sdmc[512];

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void sdmc_remove(void)
{
    if (g_sdmc[0])
        nftw(g_sdmc, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

__attribute__((constructor)) static void sdmc_default(void)
{
    if (g_sdmc[0] || getenv("RELIFT_SDMC"))
        return;
    const char *tmp = getenv("TMPDIR");
    snprintf(g_sdmc, sizeof g_sdmc, "%s/relift-sdmc-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(g_sdmc)) {
        fprintf(stderr, "  mkdtemp %s failed\n", g_sdmc);
        exit(2);
    }
    setenv("RELIFT_SDMC", g_sdmc, 1);
    atexit(sdmc_remove);
}

rl_ctx *t_boot(const rl_hos_config *cfg)
{
    if (rl_hos_init(cfg) != 0) {
        fprintf(stderr, "  rl_hos_init failed\n");
        exit(2);
    }
    rl_ctx *c = rl_hos_attach_thread(44, 0);
    if (!c) {
        fprintf(stderr, "  attach failed\n");
        exit(2);
    }
    return c;
}

uint32_t t_svc(rl_ctx *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    c->x[4] = a4;
    c->x[5] = a5;
    t_svc_rc = rl_svc(c, t_mem(), imm);
    return (uint32_t)c->x[0];
}

void t_sleep(uint64_t ns)
{
    rl_ctx *c = rl_thread_current()->ctx;
    uint64_t x[6];
    memcpy(x, c->x, sizeof x);
    int rc = t_svc_rc;
    t_svc(c, SVC_SLEEP_THREAD, ns, 0, 0, 0, 0, 0);
    memcpy(c->x, x, sizeof x);
    t_svc_rc = rc;
}

uint64_t t_heap(rl_ctx *c, uint64_t size)
{
    if (t_svc(c, SVC_SET_HEAP_SIZE, 0, size, 0, 0, 0, 0) != 0)
        return 0;
    return c->x[1];
}

void t_guest_fns(const uint64_t *addrs, rl_fn const *fns, uint32_t n)
{
    rl_fn_entry *e = calloc(n, sizeof *e);
    rl_unit *u = calloc(1, sizeof *u);
    const rl_unit **list = calloc(1, sizeof *list);
    for (uint32_t i = 0; i < n; i++) {
        e[i].guest_addr = addrs[i];
        e[i].guest_size = 4;
        e[i].fn = fns[i];
    }
    u->abi_version = RL_ABI_VERSION;
    u->ctx_size = sizeof(rl_ctx);
    u->fn_count = n;
    u->fns = e;
    u->name = "tests";
    list[0] = u;
    rl_units_register((const rl_unit *const *)list, 1);
    if (rl_thread_current())
        rl_dispatch_view(rl_thread_current()->ctx);
}

uint64_t t_free_map_addr(void)
{
    const hos_layout *l = &hos.lay;
    uint64_t ends[4] = { l->heap_end, l->alias_end, l->stack_end, l->tlsio_end };
    uint64_t a = l->code_end;
    for (int i = 0; i < 4; i++)
        if (ends[i] > a)
            a = ends[i];
    return a + 0x100000;
}
