/**
 * @file gpu3d_fifo.c
 * @brief The 3D engine's registers and its command queue: GXFIFO with
 * packed commands, the command ports, the pipe in front of the 256-entry
 * FIFO, the time each command takes, the FIFO full stall of the store that
 * meets it, SWAP_BUFFERS waiting for VBlank, GXSTAT, RAM_COUNT, the test and
 * matrix results, the GXFIFO interrupt (a level: IF bit 21 stays set while
 * its condition holds) and the GXFIFO DMA start, and the rendering
 * registers.
 *
 * The engine is lazy: it runs its queue up to a tick when something may see
 * it (a register access, the rendering engine's frame start) or when its
 * event fires, which is queued only while an interrupt or a DMA waits for
 * the FIFO to drain. The event is the display's, so the display draws its
 * lines up to the event's tick before a swap changes the lists the 3D lines
 * come from. A command runs once all its parameters are in the queue
 * and the one before it is done; its effect lands at its start and it holds
 * the engine for its cycle count (GBATEK's table, two ticks a cycle).
 *
 * Fresh C from GBATEK ("DS 3D I/O Map", "DS 3D Geometry Commands", "DS 3D
 * Status", "DS 3D Display Control").
 */
#include "nds_int.h"

#include <string.h>

/** @brief Parameters (bits 0-5) and cycles (bits 8-17) of each command; 0 for an invalid one. */
static const uint32_t cmd_info[0x80] = {
    [0x10] = 1 | 1u << 8,   [0x11] = 0 | 17u << 8,  [0x12] = 1 | 36u << 8,  [0x13] = 1 | 17u << 8,
    [0x14] = 1 | 36u << 8,  [0x15] = 0 | 19u << 8,  [0x16] = 16 | 34u << 8, [0x17] = 12 | 30u << 8,
    [0x18] = 16 | 35u << 8, [0x19] = 12 | 31u << 8, [0x1A] = 9 | 28u << 8,  [0x1B] = 3 | 22u << 8,
    [0x1C] = 3 | 22u << 8,  [0x20] = 1 | 1u << 8,   [0x21] = 1 | 9u << 8,   [0x22] = 1 | 1u << 8,
    [0x23] = 2 | 9u << 8,   [0x24] = 1 | 8u << 8,   [0x25] = 1 | 8u << 8,   [0x26] = 1 | 8u << 8,
    [0x27] = 1 | 8u << 8,   [0x28] = 1 | 8u << 8,   [0x29] = 1 | 1u << 8,   [0x2A] = 1 | 1u << 8,
    [0x2B] = 1 | 1u << 8,   [0x30] = 1 | 4u << 8,   [0x31] = 1 | 4u << 8,   [0x32] = 1 | 6u << 8,
    [0x33] = 1 | 1u << 8,   [0x34] = 32 | 32u << 8, [0x40] = 1 | 1u << 8,   [0x41] = 0 | 1u << 8,
    [0x50] = 1 | 392u << 8, [0x60] = 1 | 1u << 8,   [0x70] = 3 | 103u << 8, [0x71] = 2 | 9u << 8,
    [0x72] = 1 | 5u << 8,
};

enum {
    CMD_SWAP = 0x50,
    /** @brief Cycles from VBlank to the end of SWAP_BUFFERS. */
    SWAP_CYCLES = 392,
    /** @brief Words one GXFIFO DMA start moves. */
    DMA_BURST = 112,
};

static bool valid(uint8_t cmd) { return cmd < 0x80 && cmd_info[cmd] != 0; }
static unsigned params(uint8_t cmd) { return cmd_info[cmd & 0x7F] & 0x3F; }
/** @brief Queue entries a command takes: one per parameter, one for a command without any. */
static unsigned entries(uint8_t cmd) { return params(cmd) ? params(cmd) : 1; }

unsigned nds_gx_cycles(uint8_t cmd) { return (cmd_info[cmd & 0x7F] >> 8) & 0x3FF; }

static unsigned queued(const nds_gx *g) { return (unsigned)g->pipe_n + g->fifo_n; }
static nds_gx_entry *at_q(nds_gx *g, unsigned i) { return &g->q[(g->head + i) % (NDS_GX_FIFO + NDS_GX_PIPE)]; }

/** @brief True when the GXFIFO interrupt's condition holds. */
static bool irq_cond(const nds_gx *g)
{
    unsigned mode = g->stat >> 30;
    return (mode == 1 && g->fifo_n < NDS_GX_FIFO / 2) || (mode == 2 && g->fifo_n == 0);
}

uint32_t nds_gx_irq_hold(rl_nds *m, uint64_t now)
{
    nds_gx_run(m, now);
    return irq_cond(&m->gx) ? RL_NDS_IRQ_GXFIFO : 0;
}

/** @brief Raises the interrupt and starts GXFIFO DMA when their conditions hold at tick t. */
static void levels(rl_nds *m, uint64_t t)
{
    nds_gx *g = &m->gx;
    if (irq_cond(g))
        nds_irq_set(m, RL_NDS_ARM9, RL_NDS_IRQ_GXFIFO, t);
    if (g->fifo_n < NDS_GX_FIFO / 2)
        nds_dma_start(m, RL_NDS_ARM9, NDS_DMA9_GXFIFO, t);
}

/** @brief The tick of the next VBlank start at or after t. */
static uint64_t vblank_from(const rl_nds *m, uint64_t t)
{
    uint64_t p = nds_lcd_pos(m, t), v = (uint64_t)NDS_VISIBLE * RL_NDS_LINE_TICKS;
    return t + (p <= v ? v - p : RL_NDS_FRAME_TICKS - p + v);
}

/** @brief Removes one entry from the head: the pipe gives it, and takes two from the FIFO when it runs below three. */
static void pop(nds_gx *g)
{
    g->head = (uint16_t)((g->head + 1) % (NDS_GX_FIFO + NDS_GX_PIPE));
    g->pipe_n--;
    if (g->pipe_n < 3) {
        unsigned k = g->fifo_n < 2 ? g->fifo_n : 2;
        g->pipe_n = (uint16_t)(g->pipe_n + k);
        g->fifo_n = (uint16_t)(g->fifo_n - k);
    }
}

/** @brief The tick the head command can start, or NDS_NEVER when its parameters are not all in yet. */
static uint64_t head_start(nds_gx *g)
{
    if (g->swap_wait)
        return g->swap_at;
    if (!queued(g))
        return NDS_NEVER;
    unsigned need = entries(at_q(g, 0)->cmd);
    if (queued(g) < need)
        return NDS_NEVER;
    uint64_t at = at_q(g, need - 1)->at;
    return at > g->free_at ? at : g->free_at;
}

static void swap(rl_nds *m)
{
    nds_gx *g = &m->gx;
    g->render_mode = g->swap_mode;
    g->swap_mode = g->swap_param & 3;
    g->geo ^= 1;
    g->ram[g->geo].polys = 0;
    g->ram[g->geo].verts = 0;
    g->share[0] = g->share[1] = -1;
}

void nds_gx_run(rl_nds *m, uint64_t to)
{
    nds_gx *g = &m->gx;
    for (;;) {
        uint64_t start = head_start(g);
        if (start == NDS_NEVER || start > to)
            break;
        if (g->swap_wait) {
            g->swap_wait = false;
            swap(m);
            g->free_at = start + 2u * SWAP_CYCLES;
            continue;
        }
        uint8_t cmd = at_q(g, 0)->cmd;
        uint32_t p[32];
        unsigned n = entries(cmd);
        for (unsigned i = 0; i < n; i++) {
            p[i] = at_q(g, 0)->param;
            pop(g);
        }
        levels(m, start);
        if (cmd == CMD_SWAP) {
            g->swap_wait = true;
            g->swap_param = p[0];
            g->swap_at = vblank_from(m, start);
            g->free_at = g->swap_at;
            continue;
        }
        unsigned cycles = nds_gx_exec(m, cmd, p, start);
        g->free_at = start + 2u * cycles;
    }
    if (to > g->t)
        g->t = to;
}

/** @brief Queues the engine's event at the next tick the FIFO drains while an interrupt or a DMA waits for it, or cancels it. */
static void repost(rl_nds *m)
{
    nds_gx *g = &m->gx;
    if (!m->sched)
        return;
    bool dma = false;
    for (unsigned i = 0; i < 4; i++) {
        const nds_dma_ch *c = &m->dma[RL_NDS_ARM9].ch[i];
        dma |= (c->cnt & 0x80000000u) && ((c->cnt >> 27) & 7u) == NDS_DMA9_GXFIFO;
    }
    uint64_t next = (dma || (g->stat >> 30)) ? head_start(g) : NDS_NEVER;
    if (next != NDS_NEVER && next <= g->t)
        next = g->t + 1;
    if (next == NDS_NEVER)
        rl_sched_cancel(m->sched, &g->ev);
    else if (!rl_event_queued(&g->ev) || g->ev.when != next)
        rl_sched_post(m->sched, &g->ev, next);
}

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_nds *m = e->opaque;
    nds_gx_run(m, when);
    repost(m);
}

/** @brief Adds one entry at tick now; a full FIFO first runs the engine until an entry leaves, and the store waits until then. */
static void push(rl_nds *m, uint8_t cmd, uint32_t param, uint64_t now)
{
    nds_gx *g = &m->gx;
    if (!(m->disp.powcnt & 8))
        return;
    nds_gx_run(m, now);
    while (g->fifo_n >= NDS_GX_FIFO) {
        uint64_t t = head_start(g);
        if (t == NDS_NEVER)
            return;
        nds_gx_run(m, t);
        if (t > now) {
            now = t;
            g->stall_to = t;
        }
    }
    if (g->fifo_n == 0 && g->pipe_n < NDS_GX_PIPE)
        g->pipe_n++;
    else
        g->fifo_n++;
    *at_q(g, queued(g) - 1) = (nds_gx_entry){.cmd = cmd, .param = param, .at = now};
    nds_gx_run(m, now);
}

/** @brief Moves past commands of the packed word that take no parameters, queueing the valid ones. */
static void packed_advance(rl_nds *m, uint64_t now)
{
    nds_gx *g = &m->gx;
    while (g->pk_i < g->pk_n) {
        uint8_t c = g->pk[g->pk_i];
        if (valid(c) && params(c)) {
            g->pk_left = (uint8_t)params(c);
            return;
        }
        if (valid(c))
            push(m, c, 0, now);
        g->pk_i++;
    }
    g->pk_n = g->pk_i = 0;
}

static void fifo_write(rl_nds *m, uint32_t v, uint64_t now)
{
    nds_gx *g = &m->gx;
    if (g->pk_n) {
        push(m, g->pk[g->pk_i], v, now);
        if (--g->pk_left == 0) {
            g->pk_i++;
            packed_advance(m, now);
        }
        return;
    }
    g->pk_n = 0;
    for (unsigned i = 0; i < 4; i++)
        if ((v >> (8 * i)) & 0xFF)
            g->pk_n = (uint8_t)(i + 1);
    for (unsigned i = 0; i < 4; i++)
        g->pk[i] = (uint8_t)(v >> (8 * i));
    g->pk_i = 0;
    packed_advance(m, now);
}

bool nds_gx_reg(unsigned cpu, uint32_t off) { return cpu == RL_NDS_ARM9 && (off == 0x060 || (off >= 0x320 && off < 0x6A4)); }

static uint32_t gxstat(rl_nds *m, uint64_t now)
{
    nds_gx *g = &m->gx;
    uint32_t v = g->stat & 0xC0000002u;
    v |= (uint32_t)(g->test_busy > now);
    v |= (uint32_t)(g->pos_sp & 31u) << 8 | (uint32_t)(g->proj_sp & 1u) << 13;
    v |= (uint32_t)(g->stack_busy > now) << 14;
    v |= g->stat & 0x8000u;
    v |= (uint32_t)g->fifo_n << 16;
    v |= (uint32_t)(g->fifo_n < NDS_GX_FIFO / 2) << 25 | (uint32_t)(g->fifo_n == 0) << 26;
    v |= (uint32_t)(queued(g) || g->free_at > now || g->swap_wait) << 27;
    return v;
}

uint32_t nds_gx_read(rl_nds *m, uint32_t off, uint64_t now)
{
    nds_gx *g = &m->gx;
    nds_gx_run(m, now);
    if (off == 0x060)
        return g->disp3dcnt | (g->ram_over ? 0x2000u : 0);
    if (off == 0x320)
        return 46;
    if (off == 0x600)
        return gxstat(m, now);
    if (off == 0x604) {
        const nds_gx_ram *r = &g->ram[g->geo];
        return r->polys | r->verts << 16;
    }
    if (off >= 0x620 && off < 0x630)
        return (uint32_t)g->pos_result[(off - 0x620) >> 2];
    if (off == 0x630)
        return (uint16_t)g->vec_result[0] | (uint32_t)(uint16_t)g->vec_result[1] << 16;
    if (off == 0x634)
        return (uint16_t)g->vec_result[2];
    if (off >= 0x640 && off < 0x680)
        return (uint32_t)g->clip[(off - 0x640) >> 2];
    if (off >= 0x680 && off < 0x6A4) {
        unsigned i = (off - 0x680) >> 2;
        return (uint32_t)g->vec[(i / 3) * 4 + i % 3];
    }
    return 0;
}

/** @brief Merges the mask bytes of v into a run of bytes at p (a little-endian register array). */
static void put_bytes(uint8_t *p, uint32_t v, uint32_t mask)
{
    for (unsigned i = 0; i < 4; i++)
        if (mask >> (8 * i) & 0xFF)
            p[i] = (uint8_t)(v >> (8 * i));
}

static void put16(uint16_t *p, uint32_t v, uint32_t mask)
{
    for (unsigned i = 0; i < 2; i++)
        if (mask >> (16 * i) & 0xFFFF) {
            uint16_t keep = (uint16_t)~(mask >> (16 * i));
            p[i] = (uint16_t)((p[i] & keep) | ((v >> (16 * i)) & (mask >> (16 * i))));
        }
}

void nds_gx_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_gx *g = &m->gx;
    if (off >= 0x400 && off < 0x600) {
        if (off < 0x440)
            fifo_write(m, v & mask, now);
        else if (valid((uint8_t)((off - 0x400) >> 2)))
            push(m, (uint8_t)((off - 0x400) >> 2), v & mask, now);
        repost(m);
        return;
    }
    nds_gx_run(m, now);
    if (off == 0x060) {
        uint16_t w = (uint16_t)(mask & 0x4FFFu);
        g->disp3dcnt = (uint16_t)((g->disp3dcnt & ~w) | (v & w));
        if (v & mask & 0x2000u)
            g->ram_over = false;
        if (v & mask & 0x1000u)
            g->disp3dcnt &= (uint16_t)~0x1000u;
        return;
    }
    if (off >= 0x330 && off < 0x340) {
        put16(&g->edge[(off - 0x330) >> 1], v, mask);
        for (unsigned i = 0; i < 8; i++)
            g->edge[i] &= 0x7FFF;
        return;
    }
    switch (off) {
    case 0x340:
        if (mask & 0xFF)
            g->alpha_ref = v & 31u;
        return;
    case 0x350:
        g->clear_color = (g->clear_color & ~mask) | (v & mask & 0x3F1FFFFFu);
        return;
    case 0x354: {
        uint16_t h[2] = {g->clear_depth, g->clear_ofs};
        put16(h, v, mask);
        g->clear_depth = h[0] & 0x7FFF;
        g->clear_ofs = h[1];
        return;
    }
    case 0x358:
        g->fog_color = (g->fog_color & ~mask) | (v & mask & 0x001F7FFFu);
        return;
    case 0x35C:
        if (mask & 0xFFFF)
            g->fog_ofs = (uint16_t)((g->fog_ofs & ~mask) | (v & mask & 0x7FFF));
        return;
    case 0x600:
        if (mask & v & 0x8000u) {
            g->stat &= ~0x8000u;
            g->proj_sp = 0;
            g->tex_sp = 0;
        }
        if (mask & 0xC0000000u) {
            g->stat = (g->stat & ~0xC0000000u) | (v & 0xC0000000u);
            levels(m, now);
            repost(m);
        }
        return;
    case 0x610:
        if (mask & 0xFFFF)
            g->dot_depth = (uint16_t)(v & 0x7FFF);
        return;
    default:
        break;
    }
    if (off >= 0x360 && off < 0x380) {
        put_bytes(&g->fog[off - 0x360], v, mask);
        for (unsigned i = 0; i < 4; i++)
            g->fog[off - 0x360 + i] &= 0x7F;
    } else if (off >= 0x380 && off < 0x3C0) {
        put16(&g->toon[(off - 0x380) >> 1], v, mask);
    }
}

void nds_gx_dma_check(rl_nds *m, uint64_t now)
{
    nds_gx_run(m, now);
    if (m->gx.fifo_n < NDS_GX_FIFO / 2)
        nds_dma_start(m, RL_NDS_ARM9, NDS_DMA9_GXFIFO, now);
    repost(m);
}

uint64_t nds_gx_stall(rl_nds *m)
{
    uint64_t t = m->gx.stall_to;
    m->gx.stall_to = 0;
    return t;
}

void nds_gx_reset(rl_nds *m)
{
    nds_gx *g = &m->gx;
    memset(g, 0, sizeof *g);
    g->ev = (rl_event){.fire = fire, .dev = &m->d_disp.dev, .opaque = m};
    nds_gx_geo_reset(m);
}

void nds_gx_attach(rl_nds *m)
{
    uint64_t now = nds_now(m, RL_NDS_ARM9);
    m->gx.t = now;
    m->gx.free_at = now;
}
