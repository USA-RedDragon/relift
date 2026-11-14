/**
 * @file desc.c
 * @brief The guest and CPU descriptors the layers above give the core, and
 * the guest byte order helpers.
 */
#include "core.h"

static const rl_guest_desc *g_guest;
static const rl_cpu_desc *g_cpu;

static bool pow2(uint64_t v) { return v && !(v & (v - 1)); }

int rl_core_init(const rl_guest_desc *guest, const rl_cpu_desc *cpu)
{
    if (!guest || !cpu || !pow2(guest->page_size) || guest->address_bits < 12 || guest->address_bits > 56 ||
        guest->tick_hz == 0 || !pow2(cpu->ctx_align) || cpu->ctx_align < sizeof(void *) ||
        (cpu->common_offset & 7) || cpu->common_offset + sizeof(rl_ctx_common) > cpu->ctx_size || cpu->code_shift > 6 ||
        (cpu->time_offset & 7) || (cpu->time_offset && cpu->time_offset + sizeof(rl_time) > cpu->ctx_size)) {
        rl_log(RL_LOG_ERROR, "core", "malformed guest or CPU descriptor");
        return -1;
    }
    g_guest = guest;
    g_cpu = cpu;
    return 0;
}

const rl_guest_desc *rl_core_guest(void) { return g_guest; }
const rl_cpu_desc *rl_core_cpu(void) { return g_cpu; }
rl_ctx_common *rl_common(rl_ctx *c) { return (rl_ctx_common *)(void *)((char *)c + g_cpu->common_offset); }

rl_time *rl_ctx_time(rl_ctx *c)
{
    return g_cpu->time_offset ? (rl_time *)(void *)((char *)c + g_cpu->time_offset) : NULL;
}

uint16_t rl_guest_order16(uint16_t v) { return g_cpu && g_cpu->big_endian ? __builtin_bswap16(v) : v; }
uint32_t rl_guest_order32(uint32_t v) { return g_cpu && g_cpu->big_endian ? __builtin_bswap32(v) : v; }
uint64_t rl_guest_order64(uint64_t v) { return g_cpu && g_cpu->big_endian ? __builtin_bswap64(v) : v; }
