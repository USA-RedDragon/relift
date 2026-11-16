/**
 * @file test_switch2.c
 * @brief The Switch 2's kernel: its cores, memory and regions, the remote
 * region svcGetInfo ids, and the Switch's kernel refusing those ids.
 */
#include "t.h"
#include "services.h"

#include <relift/settings.h>

static rl_ctx *boot_console(bool switch2)
{
    rl_switch_declare_settings();
    if (rl_settings_set("kernel.layout", "compact") != RL_SETTING_OK)
        return NULL;
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    if (switch2)
        rl_hos_switch2_config(&cfg);
    return t_boot(&cfg);
}

static uint64_t info(rl_ctx *c, uint32_t id)
{
    return t_svc(c, SVC_GET_INFO, 0, id, HOS_CUR_PROCESS, 0, 0, 0) == 0 ? c->x[1] : UINT64_MAX;
}

TEST(switch2_gives_six_cores_and_its_memory)
{
    rl_ctx *c = boot_console(true);
    CHECK(c && hos_switch2());
    CHECK_EQ(hos_cores(), 8);
    CHECK_EQ(info(c, 0), 0x3full);
    CHECK_EQ(info(c, 6), 9809ull << 20);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, HOS_CUR_THREAD, 5, 0x20, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, HOS_CUR_THREAD, 6, 0x40, 0, 0, 0), KR_INVALID_CPU_CORE);
}

TEST(switch2_heap_region_holds_its_memory)
{
    if (rl_cfg()->address_bits == 36)
        return;
    rl_ctx *c = boot_console(true);
    CHECK(c);
    CHECK(info(c, 5) >= 9809ull << 20);
    CHECK(info(c, 3) >= info(c, 5));
    CHECK(t_heap(c, 0x1ffe00000ull));
}

TEST(switch2_remote_region_follows_the_others)
{
    if (rl_cfg()->address_bits == 36)
        return;
    rl_ctx *c = boot_console(true);
    CHECK(c);
    uint64_t start = info(c, 29), size = info(c, 30);
    CHECK(size != 0);
    CHECK(start >= hos.lay.tlsio_end);
    CHECK(start + size <= info(c, 12) + info(c, 13));
    CHECK_EQ(info(c, 31), 0);
    CHECK_EQ(info(c, 32), 0);
    CHECK_EQ(info(c, 33), 0x1000);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 29, 0x1234, 0, 0, 0), KR_INVALID_HANDLE);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 29, HOS_CUR_PROCESS, 1, 0, 0), KR_INVALID_COMBINATION);
    uint64_t at = 0;
    while (rl_hos_reserve(1ull << 30, 0x200000, &at) == KR_SUCCESS)
        CHECK(at >= start + size);
}

TEST(switch_refuses_the_switch2_info_ids)
{
    rl_ctx *c = boot_console(false);
    CHECK(c && !hos_switch2());
    CHECK_EQ(hos_cores(), 4);
    for (uint32_t id = 29; id <= 33; id++)
        CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, id, HOS_CUR_PROCESS, 0, 0, 0), KR_INVALID_ENUM_VALUE);
}
