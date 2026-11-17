/**
 * @file disp.c
 * @brief The display as a scheduler device: both 2D engines drawn line by
 * line at each line's render tick (the HBlank edge), so register, palette,
 * OAM and video RAM writes land on the line the console shows them on;
 * engine A's display modes (graphics, VRAM display, main memory display
 * FIFO), display capture, master brightness, POWCNT1's screen swap and
 * engine power, and the finished frame of both screens at the first VBlank
 * line. Engine A's 3D layer comes from the 3D engine's line hook
 * (nds_disp_set_3d); without one it is transparent.
 *
 * Fresh C from GBATEK ("DS Video Capture and Main Memory Display Mode",
 * "DS Video Display System Block Diagram", "DS Power Control"). The render
 * tick and the forced blank echo are the oracle's (relift's mid-line probe
 * ROMs): a line is drawn at its HBlank edge, and a line also shows white
 * when forced blank was on at the render tick two lines before. A capture
 * reads source B from video RAM only when the bank DISPCNT selects is given
 * to LCDC, and the VRAM and FIFO displays leave bit 15 out of green. With
 * POWCNT1 bit 0 clear both screens are black; an engine whose power bit is
 * clear shows black (A) or white (B) through its master brightness.
 */
#include "nds_int.h"

#include <string.h>

/** @brief White, in 18 bits. */
#define WHITE 0x3FFFFu

/** @brief The tick of line's render edge for the line that starts at t0. */
static uint64_t due_of(const nds_disp *d) { return d->t0 + NDS_HBLANK_TICKS; }

/** @brief The 15-bit pixel of bank b (A to D) at byte offset off, wrapping in the bank. */
static uint16_t bank16(rl_nds *m, unsigned b, uint32_t off)
{
    const uint8_t *p = nds_vram_bank(m, b) + (off & 0x1FFFEu);
    return (uint16_t)(p[0] | p[1] << 8);
}

/** @brief True when bank b (A to D) is enabled and given to LCDC (A and B ignore MST bit 2). */
static bool lcdc(const rl_nds *m, unsigned b) { return (m->vramcnt[b] & (b <= NDS_BANK_B ? 0x83 : 0x87)) == 0x80; }

/** @brief Pops one pixel of the main memory display FIFO (0 when it is empty). */
static uint16_t fifo_pop(nds_disp *d)
{
    if (!d->fifo_n)
        return 0;
    uint16_t v = d->fifo[d->fifo_rd];
    d->fifo_rd = (uint16_t)((d->fifo_rd + 1) % NDS_FIFO_PX);
    d->fifo_n--;
    return v;
}

/** @brief One line of capture: sources A and B of line y mixed into the VRAM write block. */
static void capture(rl_nds *m, nds_disp *d, unsigned y, const uint32_t *a2d, const uint16_t *fifo_line)
{
    static const uint16_t widths[4] = {128, 256, 256, 256}, heights[4] = {128, 64, 128, 192};
    uint32_t c = d->capcnt;
    unsigned size = (c >> 20) & 3, w = widths[size];
    if (y >= heights[size])
        return;
    unsigned wb = (c >> 16) & 3, rb = (d->eng[0].dispcnt >> 18) & 3, src = (c >> 29) & 3;
    unsigned eva = (c & 31u) > 16 ? 16 : c & 31u, evb = ((c >> 8) & 31u) > 16 ? 16 : (c >> 8) & 31u;
    uint32_t wofs = ((c >> 18) & 3) * 0x8000u + y * w * 2u, rofs = ((c >> 26) & 3) * 0x8000u + y * w * 2u;
    for (unsigned x = 0; x < w; x++) {
        uint32_t ca, aa;
        if (c & 0x01000000u) {
            uint32_t v = d->l3d_on ? d->l3d[x] : 0;
            aa = ((v >> 24) & 31u) != 0;
            ca = v & 0x3FFFFu;
        } else {
            aa = 1;
            ca = a2d[x];
        }
        uint32_t a15 = (ca >> 1 & 31u) | (ca >> 7 & 31u) << 5 | (ca >> 13 & 31u) << 10;
        uint16_t b15 = (c & 0x02000000u) ? fifo_line[x] : lcdc(m, rb) ? bank16(m, rb, rofs + x * 2u) : 0;
        uint32_t ab = b15 >> 15, out;
        if (src == 0) {
            out = a15 | aa << 15;
        } else if (src == 1) {
            out = b15;
        } else {
            out = 0;
            for (unsigned s = 0; s < 15; s += 5) {
                unsigned v = ((a15 >> s & 31u) * aa * eva + (b15 >> s & 31u) * ab * evb + 8) >> 4;
                out |= (v > 31 ? 31u : v) << s;
            }
            out |= (uint32_t)((aa && eva) || (ab && evb)) << 15;
        }
        if (lcdc(m, wb)) {
            uint8_t *p = nds_vram_bank(m, wb) + ((wofs + x * 2u) & 0x1FFFEu);
            p[0] = (uint8_t)out;
            p[1] = (uint8_t)(out >> 8);
            nds_watch_store(m, p);
        }
    }
}

/** @brief Draws visible line y of both engines into the frame. */
static void draw(rl_nds *m, nds_disp *d, unsigned y)
{
    uint32_t *top = d->frame + y * NDS_W, *bot = d->frame + (NDS_H + y) * NDS_W;
    bool a_top = d->powcnt & 0x8000;
    uint32_t *dst[2] = {a_top ? top : bot, a_top ? bot : top};
    d->l3d_on = d->line3d_fn && (d->eng[0].dispcnt & 8) && d->line3d_fn(d->line3d_user, y, d->l3d);
    uint16_t fifo_line[NDS_W];
    bool fifo_used = ((d->eng[0].dispcnt >> 16) & 3) == 3 || ((d->capcnt & 0x82000000u) == 0x82000000u && d->capturing);
    for (unsigned x = 0; x < NDS_W; x++)
        fifo_line[x] = fifo_used ? fifo_pop(d) : 0;
    for (unsigned n = 0; n < 2; n++) {
        nds_eng *e = &d->eng[n];
        uint32_t *o = dst[n];
        if (!(d->powcnt & 1)) {
            for (unsigned x = 0; x < NDS_W; x++)
                o[x] = 0;
            continue;
        }
        if (!(d->powcnt & (n ? 0x200u : 0x2u))) {
            for (unsigned x = 0; x < NDS_W; x++)
                o[x] = nds_eng_bright(e, n ? WHITE : 0);
            continue;
        }
        nds_eng_line(m, e, y, d->l3d_on && n == 0 ? d->l3d : NULL);
        unsigned mode = (e->dispcnt >> 16) & 3;
        if (n == 1)
            mode &= 1;
        bool blank = d->fb[n][y] || d->fb[n][(y + RL_NDS_LINES - 2) % RL_NDS_LINES];
        if (n == 0 && d->capturing)
            capture(m, d, y, e->out, fifo_line);
        for (unsigned x = 0; x < NDS_W; x++) {
            uint32_t c;
            if (mode == 0)
                c = WHITE;
            else if (mode == 1)
                c = blank ? WHITE : e->out[x];
            else if (mode == 2)
                c = nds_c18(bank16(m, (e->dispcnt >> 18) & 3, (y * NDS_W + x) * 2u) & 0x7FFFu);
            else
                c = nds_c18(fifo_line[x] & 0x7FFFu);
            o[x] = nds_eng_bright(e, c);
        }
        nds_eng_line_end(e, y);
    }
}

/** @brief Hands over the finished frame at the render tick when of the first VBlank line. */
static void finish(nds_disp *d, uint64_t when)
{
    if (d->capturing) {
        d->capcnt &= 0x7FFFFFFFu;
        d->capturing = false;
    }
    memcpy(d->last, d->frame, sizeof d->last);
    d->frames++;
    d->have = true;
    if (d->frame_fn)
        d->frame_fn(d->frame_user, d->last, when);
}

/** @brief The work of line l at its render tick when. */
static void line(rl_nds *m, nds_disp *d, unsigned l, uint64_t when)
{
    if (l == 0) {
        nds_eng_frame(&d->eng[0]);
        nds_eng_frame(&d->eng[1]);
        d->capturing = d->capcnt & 0x80000000u;
    }
    for (unsigned n = 0; n < 2; n++)
        d->fb[n][l] = d->eng[n].dispcnt & 0x80;
    nds_gx_line(m, l, when);
    if (l < NDS_VISIBLE)
        draw(m, d, l);
    else if (l == NDS_VISIBLE)
        finish(d, when);
}

/**
 * @brief Whether a frame from line 0 leaves nothing but black frames: the
 * LCDs are off, no capture is armed, the main memory FIFO is not read and
 * no frame hook watches.
 */
static bool dark(const nds_disp *d)
{
    return !(d->powcnt & 1) && !(d->capcnt & 0x80000000u) && ((d->eng[0].dispcnt >> 16) & 3) != 3 && !d->frame_fn;
}

/**
 * @brief Passes n dark frames at once (what n runs of every line leave,
 * without drawing them). Display-only: the display timing, its interrupts
 * and DMA starts are lcd.c's and are not touched. It runs only while dark()
 * holds, so never with a frame hook (PNG dumps and --frames see every frame).
 */
static void skip_dark(nds_disp *d, uint64_t n)
{
    for (unsigned e = 0; e < 2; e++) {
        nds_eng_frame(&d->eng[e]);
        for (unsigned l = 0; l < RL_NDS_LINES; l++)
            d->fb[e][l] = d->eng[e].dispcnt & 0x80;
    }
    memset(d->frame, 0, sizeof d->frame);
    memset(d->last, 0, sizeof d->last);
    d->frames += n;
    d->have = true;
    d->capturing = false;
    d->t0 += n * RL_NDS_FRAME_TICKS;
}

void nds_disp_catch_up(rl_nds *m, uint64_t to)
{
    nds_disp *d = &m->disp;
    if (d->busy)
        return;
    d->busy = true;
    while (due_of(d) <= to) {
        if (d->line == 0 && dark(d)) {
            uint64_t n = (to - due_of(d)) / RL_NDS_FRAME_TICKS;
            if (n > 1) {
                skip_dark(d, n - 1);
                continue;
            }
        }
        line(m, d, d->line, due_of(d));
        d->t0 += RL_NDS_LINE_TICKS;
        if (++d->line == RL_NDS_LINES)
            d->line = 0;
    }
    d->busy = false;
}

static void dev_catch_up(rl_device *dv, uint64_t from, uint64_t to)
{
    (void)from;
    nds_disp_catch_up(((nds_dev *)dv->opaque)->m, to);
}

void nds_disp_sync(rl_nds *m, uint64_t now)
{
    if (m->sched)
        rl_sched_sync(m->sched, rl_device_bit(&m->d_disp.dev), now);
}

/** @brief The tick of the first VBlank line's render edge after now. */
static uint64_t next_vblank(const rl_nds *m, uint64_t now)
{
    uint64_t p = nds_lcd_pos(m, now), v = (uint64_t)NDS_VISIBLE * RL_NDS_LINE_TICKS + NDS_HBLANK_TICKS;
    return now + (p < v ? v - p : RL_NDS_FRAME_TICKS - p + v);
}

/** @brief The frame event (queued only while a frame hook is set): draws up to the frame's end so the hook runs on time. */
static void frame_fire(rl_sched *s, rl_event *ev, uint64_t when)
{
    rl_nds *m = ev->opaque;
    nds_disp_sync(m, when);
    if (m->disp.frame_fn)
        rl_sched_post(s, ev, next_vblank(m, when));
}

void nds_disp_resync(rl_nds *m, uint64_t now)
{
    nds_disp *d = &m->disp;
    uint64_t p = nds_lcd_pos(m, now);
    unsigned l = (unsigned)(p / RL_NDS_LINE_TICKS);
    d->t0 = now - p % RL_NDS_LINE_TICKS;
    d->line = l;
    if (now >= due_of(d)) {
        d->t0 += RL_NDS_LINE_TICKS;
        d->line = (l + 1) % RL_NDS_LINES;
    }
    if (m->sched && d->frame_fn)
        rl_sched_post(m->sched, &d->ev, next_vblank(m, now));
}

void nds_disp_reset(rl_nds *m)
{
    nds_disp *d = &m->disp;
    memset(d, 0, sizeof *d);
    nds_eng_reset(&d->eng[0], 0);
    nds_eng_reset(&d->eng[1], 1);
}

int nds_disp_add(rl_nds *m)
{
    nds_disp *d = &m->disp;
    m->d_disp = (nds_dev){.dev = {.name = "nds display", .catch_up = dev_catch_up, .line = RL_LINE_NONE}, .m = m};
    m->d_disp.dev.opaque = &m->d_disp;
    if (rl_sched_add_device(m->sched, &m->d_disp.dev) != 0)
        return -1;
    d->ev = (rl_event){.fire = frame_fire, .dev = &m->d_disp.dev, .opaque = m};
    return 0;
}

bool nds_disp_reg(unsigned cpu, uint32_t off)
{
    if (cpu != RL_NDS_ARM9)
        return false;
    if (off < 0x70)
        return off != 0x004 && off != 0x060;
    return (off >= 0x1000 && off < 0x1070 && off != 0x1004 && off != 0x1060) || off == 0x304;
}

uint32_t nds_disp_read(rl_nds *m, uint32_t off)
{
    nds_disp *d = &m->disp;
    if (off == 0x304)
        return d->powcnt;
    if (off == 0x064)
        return d->capcnt;
    if (off == 0x068)
        return 0;
    return nds_eng_read(&d->eng[off >> 12], off & 0xFFFu);
}

void nds_disp_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask)
{
    nds_disp *d = &m->disp;
    if (off == 0x304) {
        d->powcnt = (uint16_t)((d->powcnt & ~mask) | (v & mask & 0x820Fu));
        return;
    }
    if (off == 0x064) {
        d->capcnt = (d->capcnt & ~mask) | (v & mask & 0xEF3F1F1Fu);
        return;
    }
    if (off == 0x068) {
        for (unsigned i = 0; i < 2; i++) {
            if (!(mask >> (16 * i) & 0xFFFF))
                continue;
            if (d->fifo_n == NDS_FIFO_PX)
                fifo_pop(d);
            d->fifo[(d->fifo_rd + d->fifo_n) % NDS_FIFO_PX] = (uint16_t)(v >> (16 * i));
            d->fifo_n++;
        }
        return;
    }
    nds_eng_write(&d->eng[off >> 12], off & 0xFFFu, v, mask);
}

void nds_disp_set_3d(rl_nds *m, nds_line3d_fn *fn, void *user)
{
    m->disp.line3d_fn = fn;
    m->disp.line3d_user = user;
}

void rl_nds_set_frame_hook(rl_nds *m, rl_nds_frame_fn *fn, void *user)
{
    nds_disp *d = &m->disp;
    d->frame_fn = fn;
    d->frame_user = user;
    if (!m->sched)
        return;
    if (fn)
        rl_sched_post(m->sched, &d->ev, next_vblank(m, nds_now(m, RL_NDS_ARM9)));
    else
        rl_sched_cancel(m->sched, &d->ev);
}

/** @brief The display caught up to the ARM9's tick (as it is, when asked from inside the frame hook). */
static const nds_disp *caught_up(rl_nds *m)
{
    if (m->sched && !m->disp.busy)
        nds_disp_sync(m, nds_now(m, RL_NDS_ARM9));
    return &m->disp;
}

uint64_t rl_nds_frames(rl_nds *m) { return caught_up(m)->frames; }

const uint32_t *rl_nds_frame(rl_nds *m)
{
    const nds_disp *d = caught_up(m);
    return d->have ? d->last : NULL;
}

uint64_t rl_nds_frame_hash(const uint32_t *pixels)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < (size_t)RL_NDS_SCREEN_W * RL_NDS_SCREEN_H * 2; i++)
        for (unsigned k = 0; k < 3; k++)
            h = (h ^ ((pixels[i] >> (6 * k)) & 63u)) * 0x100000001B3ull;
    return h;
}

void rl_nds_frame_rgb(const uint32_t *pixels, uint8_t *rgb)
{
    for (size_t i = 0; i < (size_t)RL_NDS_SCREEN_W * RL_NDS_SCREEN_H * 2; i++)
        for (unsigned k = 0; k < 3; k++) {
            unsigned c = (pixels[i] >> (6 * k)) & 63u;
            rgb[i * 3 + k] = (uint8_t)(c << 2 | c >> 4);
        }
}
