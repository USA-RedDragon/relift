/**
 * @file bus.c
 * @brief How long a CPU read outside RDRAM keeps the CPU waiting: the RCP's
 * read latency for its registers and SP memory, plus the PI bus cycle in
 * the cartridge's address space, or (as ares counts it) twice the rest of a
 * bus write that is still in flight there. Writes are posted and keep the
 * CPU waiting for nothing. The device answers at the cycle the read
 * starts, before the wait.
 *
 * The latencies are ported from ares (n64/memory/io.hpp, n64/memory/bus.hpp
 * and n64/pi/bus.hpp), which carries this notice:
 *
 * Copyright (c) 2004-2025 ares team, Near et al
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "n64_int.h"

enum {
    RDRAM_REGS = 0x03F00000u,
    PI_BUS = 0x05000000u,
    PIF = 0x1FC00000u,
    PIF_END = 0x1FD00000u,
    RCP_READ = 20,
    PI_BUS_READ = 250,
};

uint32_t rl_n64_read_stall(rl_n64 *m, uint32_t paddr)
{
    paddr &= 0x1FFFFFFFu;
    if (paddr < RDRAM_REGS)
        return 0;
    if (paddr < PI_BUS || (paddr >= PIF && paddr < PIF_END))
        return RCP_READ;
    if (!m->pi.io_busy)
        return RCP_READ + PI_BUS_READ;
    uint64_t now = rl_n64_now(m);
    uint64_t end = m->pi.bus_write.when;
    return RCP_READ + (uint32_t)(end > now ? 2u * (end - now) : 0);
}
