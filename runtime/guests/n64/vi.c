/**
 * @file vi.c
 * @brief VI: the registers, the line counter with its field and leap
 * pattern, the interrupt at the line VI_V_INTR names, the frame edge and the
 * scan-out of the framebuffer (RGBA5551 and RGBA8888 read from RDRAM
 * through the scale registers into the video output, as ares' software path
 * does, no filters) and the raw framebuffer.
 *
 * The line counter runs in closed form: a read, a write or the line event
 * catch it up to the CPU's tick, running the lines that started before it
 * (ares' VI runs up to the CPU's clock, not including it). The first line
 * starts at the CPU's first cycle. The line rules (the duration of each line,
 * the field toggle, the interrupt in progressive and interlaced modes, the
 * frame at the start line, a VI that is off counting 0x800-clock lines, the
 * scan-out's window, guard band, scaling and field lines) are ported from
 * ares (n64/vi/vi.cpp and io.cpp), which carries this notice:
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

/** @brief The NTSC and PAL video clocks, in Hz. */
#define VIDEO_HZ_NTSC 48681818u
#define VIDEO_HZ_PAL 49656530u

static uint32_t video_hz(const rl_n64 *m) { return m->pal ? VIDEO_HZ_PAL : VIDEO_HZ_NTSC; }

static bool active(const rl_n64 *m) { return (m->vi.control & 3u) != 0; }

/** @brief CPU cycles of a line of n video clocks, carrying the remainder. */
static uint64_t cycles_of(rl_n64 *m, uint32_t n)
{
    uint64_t scaled = (uint64_t)n * RL_N64_HZ + m->vi.frac;
    m->vi.frac = scaled % video_hz(m);
    return scaled / video_hz(m);
}

/** @brief The RGB of the RGBA5551 pixel at RDRAM address a, as 0xRRGGBB. */
static uint32_t pixel16(const rl_n64 *m, uint32_t a)
{
    a &= RL_N64_RDRAM_SIZE - 2u;
    uint32_t v = (uint32_t)rl_n64_rd8(m, a) << 8 | rl_n64_rd8(m, a + 1u);
    uint32_t r = v >> 11 & 31u, g = v >> 6 & 31u, b = v >> 1 & 31u;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/** @brief The RGB of the RGBA8888 pixel at RDRAM address a, as 0xRRGGBB. */
static uint32_t pixel32(const rl_n64 *m, uint32_t a)
{
    a &= RL_N64_RDRAM_SIZE - 4u;
    return rl_n64_rd32(m, a) >> 8;
}

/**
 * @brief The data lines in the RDRAM VI scans out (with two lines of the
 * framebuffer on each side) whose bytes the CPU holds and RDRAM does not:
 * their number, and with swap, RDRAM and the machine's kept bytes of each
 * exchanged.
 */
static unsigned held_lines(rl_n64 *m, bool swap)
{
    const uint32_t *tags = m->dc.tags;
    uint32_t depth = m->vi.control & 3u, stride = (m->vi.width & 0xFFFu) * (depth == 3 ? 4u : 2u);
    if (!tags || depth < 2)
        return 0;
    uint32_t vstart = (m->vi.v_video >> 16) & 0x3FFu, vend = m->vi.v_video & 0x3FFu;
    uint32_t lines = vend > vstart ? ((vend - vstart) / 2u) * (m->vi.y_scale & 0xFFFu) / 1024u : 0;
    if (!lines || lines > 576u)
        lines = 576u;
    uint32_t origin = m->vi.origin & 0xFFFFFFu;
    uint32_t lo = origin > 2u * stride ? origin - 2u * stride : 0, hi = origin + (lines + 2u) * stride;
    unsigned n = 0;
    for (uint32_t i = 0; i < RL_N64_DCACHE_LINES; i++) {
        uint32_t t = tags[i];
        if (!(t & RL_N64_DCACHE_VALID) || !((t & RL_N64_DCACHE_DIRTY) || m->dc.stale[i]))
            continue;
        uint32_t a = (t & RL_N64_DCACHE_TAG) | (i << 4 & 0xFF0u);
        if (a < lo || a >= hi || a >= RL_N64_RDRAM_SIZE)
            continue;
        n++;
        if (swap) {
            uint8_t tmp[16];
            memcpy(tmp, m->rdram + a, 16);
            memcpy(m->rdram + a, m->dc.ram[i], 16);
            memcpy(m->dc.ram[i], tmp, 16);
        }
    }
    return n;
}

/**
 * @brief The renderer's scan-out at the frame edge. It reads RDRAM itself,
 * where the CPU's lines are; when a line the CPU holds lies in the picture,
 * RDRAM's bytes go in its place until the scan-out is done.
 */
static void scan_out(rl_n64 *m)
{
    bool held = m->dp.ops->vi_picture && held_lines(m, false) != 0;
    if (held) {
        m->dp.ops->sync_full(m->dp.user);
        held_lines(m, true);
    }
    m->dp.ops->vi_frame(m->dp.user, m->vi.field);
    if (held) {
        uint32_t w, h;
        m->dp.ops->vi_picture(m->dp.user, &w, &h);
        held_lines(m, true);
    }
}

static void frame(rl_n64 *m)
{
    m->stats.frames++;
    if (m->vi.hook)
        m->vi.hook(m->vi.hook_user, m);
}

/** @brief The line that starts at m->vi.next: counter, interrupt, frame; sets the start of the next. */
static void line(rl_n64 *m)
{
    if (!active(m)) {
        m->vi.vcounter = 0;
        if (++m->vi.inactive >= 200) {
            m->vi.inactive = 0;
            frame(m);
        }
        m->vi.next += cycles_of(m, 0x800u);
        return;
    }
    uint32_t half_lines = m->vi.v_total & 0x3FFu;
    uint32_t coincidence = m->vi.coincidence & 0x3FFu;
    m->vi.vcounter++;
    uint32_t halfline = m->vi.vcounter << 1 | m->vi.field;
    if (halfline >= half_lines + 1u) {
        m->vi.vcounter = 0;
        m->vi.field = (m->vi.field + !(half_lines & 1u)) & 1u;
        if (++m->vi.leap_counter == 5)
            m->vi.leap_counter = 0;
    }
    if (m->vi.vcounter == ((m->vi.v_video >> 16) & 0x3FFu) >> 1) {
        if (m->dp.ops && m->dp.ops->vi_frame)
            scan_out(m);
        frame(m);
    }
    if (half_lines & 1u) {
        if (m->vi.vcounter == coincidence >> 1)
            rl_n64_mi_raise(m, RL_N64_IRQ_VI);
    } else {
        if ((coincidence & 1u) && m->vi.vcounter == coincidence >> 1)
            rl_n64_mi_raise(m, RL_N64_IRQ_VI);
        if (!(coincidence & 1u)) {
            uint32_t hl = m->vi.vcounter << 1 | m->vi.field;
            if (!m->vi.field && hl == coincidence)
                rl_n64_mi_raise(m, RL_N64_IRQ_VI);
            if (m->vi.field && hl + 1u == coincidence)
                rl_n64_mi_raise(m, RL_N64_IRQ_VI);
            if (!m->vi.field && hl == half_lines && coincidence == 0)
                rl_n64_mi_raise(m, RL_N64_IRQ_VI);
        }
    }
    uint32_t duration = (m->vi.h_total & 0xFFFu) + 1u;
    if (m->vi.vcounter == 1) {
        unsigned leap = (m->vi.h_total >> 16 >> m->vi.leap_counter) & 1u;
        duration = leap ? (m->vi.h_leap >> 16) & 0xFFFu : m->vi.h_leap & 0xFFFu;
    }
    m->vi.next += cycles_of(m, duration);
}

/** @brief Runs every line that started before tick now. */
static void catch_up(rl_n64 *m, uint64_t now)
{
    while (m->vi.next < now)
        line(m);
}

static void line_event(rl_sched *s, rl_event *e, uint64_t when)
{
    rl_n64 *m = e->opaque;
    catch_up(m, when);
    rl_sched_post(s, e, m->vi.next + 1u);
}

void rl_n64_vi_attach(rl_n64 *m)
{
    m->vi.line.fire = line_event;
    m->vi.line.opaque = m;
    m->vi.next = rl_n64_now(m);
    rl_sched_post(m->s, &m->vi.line, m->vi.next + 1u);
}

void rl_n64_set_frame_hook(rl_n64 *m, rl_n64_frame_hook *hook, void *user)
{
    m->vi.hook = hook;
    m->vi.hook_user = user;
}

static uint32_t reg(const rl_n64 *m, unsigned i)
{
    switch (i) {
    case 0:
        return m->vi.control & 0xFFFFu;
    case 1:
        return m->vi.origin & 0xFFFFFFu;
    case 2:
        return m->vi.width & 0xFFFu;
    case 3:
        return m->vi.coincidence & 0x3FFu;
    case 4:
        return m->vi.field | (m->vi.vcounter & 0x1FFu) << 1;
    case 5:
        return m->vi.timing & 0x3FFFFFFFu;
    case 6:
        return m->vi.v_total & 0x3FFu;
    case 7:
        return m->vi.h_total & 0x1F0FFFu;
    case 8:
        return m->vi.h_leap & 0x0FFF0FFFu;
    case 9:
        return m->vi.h_video & 0x03FF03FFu;
    case 10:
        return m->vi.v_video & 0x03FF03FFu;
    case 11:
        return m->vi.v_burst & 0x03FF03FFu;
    case 12:
        return m->vi.x_scale & 0x0FFF0FFFu;
    case 13:
        return m->vi.y_scale & 0x0FFF0FFFu;
    default:
        return 0;
    }
}

uint32_t rl_n64_vi_reg(rl_n64 *m, unsigned index) { return reg(m, index); }

uint32_t rl_n64_vi_read(rl_n64 *m, unsigned i)
{
    catch_up(m, rl_n64_now(m));
    return reg(m, i);
}

void rl_n64_vi_write(rl_n64 *m, unsigned i, uint32_t v)
{
    catch_up(m, rl_n64_now(m));
    if (m->dp.ops && m->dp.ops->vi_register)
        m->dp.ops->vi_register(m->dp.user, i, v);
    switch (i) {
    case 0:
        m->vi.control = v;
        break;
    case 1:
        m->vi.origin = v;
        break;
    case 2:
        m->vi.width = v;
        break;
    case 3:
        m->vi.coincidence = v;
        break;
    case 4:
        rl_n64_mi_lower(m, RL_N64_IRQ_VI);
        break;
    case 5:
        m->vi.timing = v;
        break;
    case 6:
        m->vi.v_total = v;
        break;
    case 7:
        m->vi.h_total = v;
        break;
    case 8:
        m->vi.h_leap = v;
        break;
    case 9:
        m->vi.h_video = v;
        break;
    case 10:
        m->vi.v_video = v;
        break;
    case 11:
        m->vi.v_burst = v;
        break;
    case 12:
        m->vi.x_scale = v;
        break;
    case 13:
        m->vi.y_scale = v;
        break;
    default:
        break;
    }
}

bool rl_n64_framebuffer(rl_n64 *m, uint8_t *rgb, uint32_t *width, uint32_t *height)
{
    uint32_t depth = m->vi.control & 3u, stride = m->vi.width & 0xFFFu;
    if (depth < 2 || !stride)
        return false;
    uint32_t hstart = (m->vi.h_video >> 16) & 0x3FFu, hend = m->vi.h_video & 0x3FFu;
    uint32_t vstart = (m->vi.v_video >> 16) & 0x3FFu, vend = m->vi.v_video & 0x3FFu;
    uint32_t xs = m->vi.x_scale & 0xFFFu, ys = m->vi.y_scale & 0xFFFu;
    uint32_t w = hend > hstart ? (hend - hstart) * xs / 1024u : 0;
    uint32_t h = vend > vstart ? ((vend - vstart) / 2u) * ys / 1024u : 0;
    if (!w || w > stride)
        w = stride;
    if (w > 640u)
        w = 640u;
    if (!h || h > 576u)
        h = 240u;
    uint32_t base = m->vi.origin & 0xFFFFFFu;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t rgb24 = depth == 2 ? pixel16(m, base + (y * stride + x) * 2u) : pixel32(m, base + (y * stride + x) * 4u);
            uint8_t *o = rgb + ((size_t)y * w + x) * 3u;
            o[0] = (uint8_t)(rgb24 >> 16);
            o[1] = (uint8_t)(rgb24 >> 8);
            o[2] = (uint8_t)rgb24;
        }
    *width = w;
    *height = h;
    return true;
}

/** @brief Draws the lines of the picture the window and scale registers describe into the output, the lines of the other field kept as they were when VI interlaces. */
static void refresh(rl_n64 *m)
{
    uint32_t hscan_start = m->pal ? 128u : 108u, vscan_start = m->pal ? 44u : 34u;
    uint32_t hscan_len = 640u, vscan_len = m->pal ? 576u : 480u;
    uint32_t hscan_stop = hscan_start + hscan_len, vscan_stop = vscan_start + vscan_len;
    uint32_t hstart = (m->vi.h_video >> 16) & 0x3FFu, hend = m->vi.h_video & 0x3FFu;
    uint32_t vstart = (m->vi.v_video >> 16) & 0x3FFu, vend = m->vi.v_video & 0x3FFu;
    uint32_t xscale = m->vi.x_scale & 0xFFFu, xsub = (m->vi.x_scale >> 16) & 0xFFFu;
    uint32_t yscale = m->vi.y_scale & 0xFFFu, ysub = (m->vi.y_scale >> 16) & 0xFFFu;
    uint32_t pitch = m->vi.width & 0xFFFu, origin = m->vi.origin & 0xFFFFFFu;
    bool serrate = (m->vi.control >> 6) & 1u;
    bool bpp16 = (m->vi.control & 3u) == 2u;

    int32_t dy0 = (int32_t)vstart, dy1 = (int32_t)vend;
    if (dy1 < dy0)
        dy1 = (int32_t)vscan_stop;
    int32_t dx0 = (int32_t)hstart, dx1 = (int32_t)hend;
    if (dy0 < (int32_t)vscan_start)
        dy0 = (int32_t)vscan_start;
    if (dy1 > (int32_t)vscan_stop)
        dy1 = (int32_t)vscan_stop;
    if (dx0 < (int32_t)hscan_start)
        dx0 = (int32_t)hscan_start;
    if (dx1 > (int32_t)hscan_stop)
        dx1 = (int32_t)hscan_stop;
    if (dx0 >= (int32_t)hscan_start)
        dx0 += 8;
    if (dx1 < (int32_t)hscan_stop)
        dx1 -= 7;

    uint32_t y0 = ysub + yscale * (uint32_t)(dy0 - (int32_t)vstart);
    for (int32_t dy = dy0; dy < dy1; dy++) {
        if (!serrate || (uint32_t)(dy & 1) == !m->vi.field) {
            uint32_t row = origin + (y0 >> 11) * pitch * (bpp16 ? 2u : 4u);
            uint8_t *line = m->vi.screen + (size_t)(dy - (int32_t)vscan_start) * hscan_len * 3u;
            uint32_t x0 = xsub + xscale * (uint32_t)(dx0 - (int32_t)hstart);
            for (int32_t dx = dx0; dx < dx1; dx++) {
                uint32_t rgb24 = bpp16 ? pixel16(m, row + (x0 >> 10) * 2u) : pixel32(m, row + (x0 >> 10) * 4u);
                uint8_t *o = line + (size_t)(dx - (int32_t)hscan_start) * 3u;
                o[0] = (uint8_t)(rgb24 >> 16);
                o[1] = (uint8_t)(rgb24 >> 8);
                o[2] = (uint8_t)rgb24;
                x0 += xscale;
            }
        }
        y0 += yscale;
    }
    m->vi.screen_w = hscan_len;
    m->vi.screen_h = vscan_len;
    m->vi.screen_ready = true;
}

bool rl_n64_scanout(rl_n64 *m, uint8_t *rgb, size_t cap, uint32_t *width, uint32_t *height)
{
    if (m->dp.ops && m->dp.ops->vi_picture) {
        uint32_t w = 0, h = 0;
        const uint8_t *rgba = m->dp.ops->vi_picture(m->dp.user, &w, &h);
        if (!rgba)
            return false;
        *width = w;
        *height = h;
        if ((size_t)w * h > cap)
            return false;
        for (size_t i = 0; i < (size_t)w * h; i++)
            memcpy(rgb + i * 3u, rgba + i * 4u, 3);
        return true;
    }
    if (!m->vi.screen)
        return false;
    if (active(m))
        refresh(m);
    if (!m->vi.screen_ready)
        return false;
    *width = m->vi.screen_w;
    *height = m->vi.screen_h;
    if ((size_t)m->vi.screen_w * m->vi.screen_h > cap)
        return false;
    memcpy(rgb, m->vi.screen, (size_t)m->vi.screen_w * m->vi.screen_h * 3u);
    return true;
}
