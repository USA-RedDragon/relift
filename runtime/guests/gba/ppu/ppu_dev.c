/**
 * @file ppu_dev.c
 * @brief The picture unit as the machine's PPU slot device: the unit is
 * advanced lazily line by line (start of line at dot 0, drawing at the
 * render dot) whenever something can observe it, a register access, a
 * write to palette, video RAM or OAM, or the frame event at the start of
 * VBlank, which hands the finished frame to the frame hook.
 */
#include "gba_int.h"

#include <string.h>

enum { DEFAULT_RENDER_DOT = 24 };

static void schedule(rl_gba *g)
{
    gba_ppu_sched *s = &g->ps;
    s->due = s->t0 + (s->stage ? s->dot : 0);
}

static void finish_frame(rl_gba *g, uint64_t when)
{
    gba_ppu_sched *s = &g->ps;
    memcpy(s->last, g->ppu.pixels, sizeof s->last);
    s->hash = gba_ppu_hash(&g->ppu);
    s->frames++;
    if (s->frame_fn)
        s->frame_fn(s->frame_user, s->last, when);
}

void gba_ppu_dev_catch_up(rl_gba *g, uint64_t to)
{
    gba_ppu_sched *s = &g->ps;
    while (s->due <= to) {
        if (s->stage == 0) {
            gba_ppu_line_start(&g->ppu, s->line);
            if (s->line == GBA_PPU_H)
                finish_frame(g, s->t0);
            s->stage = 1;
        } else {
            gba_ppu_render(&g->ppu, s->line);
            s->stage = 0;
            s->t0 += RL_GBA_LINE_CYCLES;
            if (++s->line == GBA_PPU_LINES)
                s->line = 0;
        }
        schedule(g);
    }
}

static void dev_catch_up(rl_device *d, uint64_t from, uint64_t to)
{
    (void)from;
    gba_ppu_dev_catch_up(gba_of(d), to);
}

static void frame_fire(rl_sched *sch, rl_event *e, uint64_t when)
{
    rl_sched_post(sch, e, when + RL_GBA_FRAME_CYCLES);
}

static void post_frame(rl_gba *g)
{
    gba_ppu_sched *s = &g->ps;
    if (!s->frame_fn || !g->sched) {
        if (g->sched)
            rl_sched_cancel(g->sched, &s->ev);
        return;
    }
    unsigned ahead = (GBA_PPU_H + GBA_PPU_LINES - s->line) % GBA_PPU_LINES;
    if (ahead == 0 && s->stage != 0)
        ahead = GBA_PPU_LINES;
    rl_sched_post(g->sched, &s->ev, s->t0 + (uint64_t)ahead * RL_GBA_LINE_CYCLES);
}

void gba_ppu_dev_resync(rl_gba *g)
{
    gba_ppu_sched *s = &g->ps;
    if (!g->sched)
        return;
    uint64_t now = gba_now(g);
    uint64_t p = gba_lcd_pos(g, now);
    unsigned line = (unsigned)(p / RL_GBA_LINE_CYCLES), dot = (unsigned)(p % RL_GBA_LINE_CYCLES);
    s->t0 = now - dot;
    s->line = line;
    s->stage = dot == 0 ? 0 : 1;
    if (dot > s->dot) {
        s->stage = 0;
        s->t0 += RL_GBA_LINE_CYCLES;
        s->line = (line + 1) % GBA_PPU_LINES;
    }
    schedule(g);
    post_frame(g);
}

void gba_ppu_dev_reset(rl_gba *g)
{
    gba_ppu_sched *s = &g->ps;
    gba_ppu_reset(&g->ppu, g->pal, g->vram, g->oam);
    memset(s, 0, sizeof *s);
    s->dot = DEFAULT_RENDER_DOT;
    s->due = GBA_NONE;
}

void gba_ppu_dev_attach(rl_gba *g)
{
    gba_ppu_sched *s = &g->ps;
    g->d_ppu.dev.catch_up = dev_catch_up;
    s->ev = (rl_event){.fire = frame_fire, .dev = &g->d_ppu.dev, .opaque = g};
    s->t0 = g->lcd.origin;
    s->line = 0;
    s->stage = 0;
    schedule(g);
    post_frame(g);
}

uint8_t gba_ppu_dev_read(rl_gba *g, uint32_t addr)
{
    int v = gba_ppu_read(&g->ppu, addr);
    return v < 0 ? gba_open_byte(g, addr) : (uint8_t)v;
}

void gba_ppu_dev_write(rl_gba *g, uint32_t addr, uint8_t v) { gba_ppu_write(&g->ppu, addr, v); }

void rl_gba_set_frame_hook(rl_gba *g, rl_gba_frame_fn *fn, void *user)
{
    g->ps.frame_fn = fn;
    g->ps.frame_user = user;
    post_frame(g);
}

static const gba_ppu_sched *caught_up(rl_gba *g)
{
    if (g->sched && g->slot[RL_GBA_SLOT_PPU] == &g->d_ppu.dev)
        gba_ppu_sync(g, gba_now(g));
    return &g->ps;
}

uint64_t rl_gba_frames(rl_gba *g) { return caught_up(g)->frames; }
uint64_t rl_gba_frame_hash(rl_gba *g) { return caught_up(g)->hash; }

const uint16_t *rl_gba_frame(rl_gba *g)
{
    const gba_ppu_sched *s = caught_up(g);
    return s->frames ? s->last : NULL;
}

void rl_gba_set_render_dot(rl_gba *g, unsigned dot)
{
    g->ps.dot = dot < RL_GBA_LINE_CYCLES ? dot : RL_GBA_LINE_CYCLES - 1;
    schedule(g);
}

void rl_gba_set_stopped(rl_gba *g, bool stopped) { g->ppu.stopped = stopped; }
