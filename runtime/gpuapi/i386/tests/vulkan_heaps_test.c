/**
 * @file vulkan_heaps_test.c
 * @brief The heap rule on synthetic devices: lavapipe is capped (heaps and budget), a resizable-BAR style 24 GiB device-local host-visible heap on another driver is not.
 */
#include "../vulkan_heaps.h"

#include <stdio.h>

enum { ID_NVIDIA = 4, ID_MESA_RADV = 3 };

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static uint64_t get64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void device(uint8_t *p, uint64_t heap0, uint64_t heap1)
{
    memset(p, 0, TG_VK_MEMPROPS_SIZE);
    put32(p, 2);
    put32(p + 4, 0x7);
    put32(p + 8, 0);
    put32(p + 12, 0x7);
    put32(p + 16, 1);
    put32(p + TG_VK_MEMPROPS_HEAP_COUNT, 2);
    put64(p + TG_VK_MEMPROPS_HEAPS, heap0);
    put64(p + TG_VK_MEMPROPS_HEAPS + 12, heap1);
}

static int check(const char *what, uint64_t got, uint64_t want)
{
    if (got == want)
        return 0;
    fprintf(stderr, "%s: %llu, want %llu\n", what, (unsigned long long)got, (unsigned long long)want);
    return 1;
}

int main(void)
{
    uint8_t p[TG_VK_MEMPROPS_SIZE], b[TG_VK_BUDGET_SIZE];
    int bad = 0;

    device(p, 24ULL << 30, 16ULL << 30);
    tg_vk_cap_heaps(p, TG_VK_DRIVER_ID_LLVMPIPE);
    bad |= check("lavapipe heap 0", get64(p + TG_VK_MEMPROPS_HEAPS), TG_VK_HEAP_CAP);
    bad |= check("lavapipe heap 1", get64(p + TG_VK_MEMPROPS_HEAPS + 12), TG_VK_HEAP_CAP);
    device(p, 1ULL << 30, 2ULL << 30);
    tg_vk_cap_heaps(p, TG_VK_DRIVER_ID_LLVMPIPE);
    bad |= check("lavapipe small heap", get64(p + TG_VK_MEMPROPS_HEAPS + 12), 2ULL << 30);

    for (uint32_t id = 0; id < 2; id++) {
        uint32_t driver = id ? ID_MESA_RADV : ID_NVIDIA;
        device(p, 24ULL << 30, 16ULL << 30);
        tg_vk_cap_heaps(p, driver);
        bad |= check("resizable BAR heap", get64(p + TG_VK_MEMPROPS_HEAPS), 24ULL << 30);
        bad |= check("second heap", get64(p + TG_VK_MEMPROPS_HEAPS + 12), 16ULL << 30);
    }

    memset(b, 0, sizeof b);
    put64(b + TG_VK_BUDGET_BUDGET, 20ULL << 30);
    put64(b + TG_VK_BUDGET_USAGE, 5ULL << 30);
    tg_vk_cap_budget(b, ID_NVIDIA);
    bad |= check("budget on another driver", get64(b + TG_VK_BUDGET_BUDGET), 20ULL << 30);
    tg_vk_cap_budget(b, TG_VK_DRIVER_ID_LLVMPIPE);
    bad |= check("lavapipe budget", get64(b + TG_VK_BUDGET_BUDGET), TG_VK_HEAP_CAP);
    bad |= check("lavapipe usage", get64(b + TG_VK_BUDGET_USAGE), TG_VK_HEAP_CAP);

    if (!bad)
        puts("vulkan heaps: lavapipe capped, other drivers unchanged");
    return bad;
}
