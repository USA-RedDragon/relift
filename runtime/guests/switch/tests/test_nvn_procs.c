/**
 * @file test_nvn_procs.c
 * @brief nvnDeviceGetProcAddress for names relift does not implement: the game's driver loader decides between NULL
 * (a name no NVN driver has) and a slot that stops when called (a name the driver has and relift does not serve).
 */
#include "nvn/nvn.h"
#include "nvn/nvn_priv.h"
#include "t.h"
#include "tc.h"

#include <string.h>

#define RET 0x7000000ull

/** @brief A stand-in for the driver's nvnBootstrapLoaderInternal: knows nvnDeviceFakeKnownNVX only. */
static uint64_t fake_loader(rl_ctx *c, uint8_t *mem)
{
    const char *name = (const char *)(mem + c->x[0]);
    c->x[0] = !strcmp(name, "nvnDeviceFakeKnownNVX") ? 0x9990000ull : 0;
    return c->x[30];
}

static uint64_t proc(rl_ctx *c, uint64_t heap, const char *name)
{
    strcpy((char *)(t_mem() + heap), name);
    c->x[0] = 0;
    c->x[1] = heap;
    c->x[30] = RET;
    return nvnh_nvnDeviceGetProcAddress(c, t_mem()) == RET ? c->x[0] : 0xdead;
}

TEST(nvn_a_proc_the_games_driver_does_not_have_is_null)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    c->sp = heap + 0x100000;
    static rl_fn const fns[1] = { fake_loader };
    uint64_t addrs[1] = { t_code(0) };
    t_guest_fns(addrs, fns, 1);
    const rl_switch_hook *h = rl_nvn_switch_hook();
    uint64_t code_free = 0x9000000ull;
    CHECK_EQ(h->start(h->user, &code_free), 0);
    rl_dispatch_view(c);
    h->defined(h->user, "nvnBootstrapLoaderInternal", t_code(0));
    CHECK_EQ(proc(c, heap, "nvnCUDAInteropFunction"), 0);
    uint64_t known = proc(c, heap, "nvnDeviceFakeKnownNVX");
    CHECK(known != 0 && known != 0x9990000ull);
    CHECK(proc(c, heap, "nvnQueueSubmitCommands") != 0);
    h->defined(h->user, "nvnBootstrapLoaderInternal", 0);
    CHECK(proc(c, heap, "nvnCUDAInteropFunction") != 0);
}
