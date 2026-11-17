/**
 * @file sms.c
 * @brief The Master System and Game Gear machine: memory map, mappers, I/O
 * ports, controllers and the CPU-to-VDP timeline.
 *
 * Ported from ares (ms/cpu/memory.cpp, ms/cartridge/board/sega.cpp,
 * codemasters.cpp, korea.cpp, ms/controller/gamepad/gamepad.cpp,
 * ms/system/controls.cpp and the scheduler's clock steps). It carries this
 * notice:
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
#include "sms.h"

#include <stdlib.h>
#include <string.h>

#define SECOND 0x7FFFFFFFFFFFFFFFull
#define REBASE (1ull << 20)

static bool is_gg(const sms *m) { return m->system == SMS_SYSTEM_GG || m->system == SMS_SYSTEM_GG_MS; }

static bool is_sg(const sms *m) { return m->system == SMS_SYSTEM_SG; }

static bool gg_mode(const sms *m) { return m->system == SMS_SYSTEM_GG; }

static uint32_t mirror(uint32_t addr, uint32_t size)
{
    uint32_t base = 0, mask = 1u << 31;
    while (addr >= size) {
        while (!(addr & mask))
            mask >>= 1;
        addr -= mask;
        if (size > mask) {
            size -= mask;
            base += mask;
        }
        mask >>= 1;
    }
    return base + addr;
}

/* Time. */

static void clock_init(sms_clock *k, bool pal)
{
    uint64_t cpu = pal ? 3546895u : 3579545u;
    uint64_t vdp = pal ? 10640685u : 10738636u;
    *k = (sms_clock){.cpu_step = SECOND / cpu, .vdp_step = SECOND / vdp};
}

/** @brief Moves the base point forward in steps while it is more than two steps behind c, so times up to one step before c stay above it. */
static void clock_rebase(sms_clock *k, uint64_t c)
{
    while (c >= k->c0 && c - k->c0 >= 2 * REBASE) {
        uint64_t t = k->r0 + (REBASE - 1u) * k->cpu_step;
        k->q0 += t / k->vdp_step;
        k->r0 = t % k->vdp_step;
        k->c0 += REBASE - 1u;
    }
}

uint64_t sms_vdp_count(sms *m, uint64_t now)
{
    sms_clock *k = &m->clock;
    clock_rebase(k, now);
    uint64_t t = k->r0 + (now - k->c0) * k->cpu_step;
    return k->q0 + t / k->vdp_step;
}

uint64_t sms_cycle_of(sms *m, uint64_t p)
{
    sms_clock *k = &m->clock;
    if (p <= k->q0)
        return k->c0;
    uint64_t need = (p - k->q0) * k->vdp_step;
    if (need <= k->r0)
        return k->c0;
    need -= k->r0;
    return k->c0 + (need + k->cpu_step - 1u) / k->cpu_step;
}

/* Inputs. */

static uint32_t input_of(const sms *m, uint64_t frame)
{
    if (!m->input_count)
        return m->input ? m->input(m->input_user) : 0;
    return m->inputs[frame < m->input_count ? frame : m->input_count - 1];
}

static void pad_latch(sms_pad *p, uint32_t b)
{
    bool up = b & SMS_BUTTON_UP, down = b & SMS_BUTTON_DOWN;
    bool left = b & SMS_BUTTON_LEFT, right = b & SMS_BUTTON_RIGHT;
    if (!(up && down)) {
        p->y_hold = false;
        p->up = up;
        p->down = down;
    } else if (!p->y_hold) {
        bool t = p->up;
        p->y_hold = true;
        p->up = p->down;
        p->down = t;
    }
    if (!(left && right)) {
        p->x_hold = false;
        p->left = left;
        p->right = right;
    } else if (!p->x_hold) {
        bool t = p->left;
        p->x_hold = true;
        p->left = p->right;
        p->right = t;
    }
}

/** @brief The Game Gear's controls.poll: the start edge, then the direction latches. */
static void gg_poll(sms *m, uint32_t b)
{
    bool was = m->start_value;
    m->start_value = b & SMS_BUTTON_START;
    if (!was && m->start_value && m->system == SMS_SYSTEM_GG_MS)
        m->nmi = true;
    pad_latch(&m->pad, b);
}

static void frame_edge(void *user)
{
    sms *m = user;
    uint32_t lines;
    if (is_sg(m)) {
        lines = SMS_TMS_H;
        for (size_t i = 0; i < (size_t)SMS_TMS_W * SMS_TMS_H; i++)
            m->picture[i] = m->tms.pixels[i];
    } else {
        lines = sms_vdp_vlines(&m->vdp);
        memcpy(m->picture, m->vdp.pixels, sizeof m->picture);
    }
    m->picture_lines = lines;
    uint32_t done = m->input_count ? input_of(m, m->frames) : m->buttons;
    if (is_gg(m)) {
        gg_poll(m, done);
    } else if (!is_sg(m)) {
        bool was = m->start_value;
        m->reset_value = done & SMS_BUTTON_RESET;
        m->start_value = done & SMS_BUTTON_START;
        if (!was && m->start_value)
            m->nmi = true;
    }
    uint64_t frame = m->frames++;
    m->buttons = input_of(m, m->frames);
    if (m->on_frame)
        m->on_frame(m->user, frame);
}

static void plan(sms *m)
{
    uint64_t p = is_sg(m) ? sms_tms_next_irq(&m->tms) : sms_vdp_next_irq(&m->vdp);
    m->frame_pos = is_sg(m) ? sms_tms_frame_edge(&m->tms) : sms_vdp_frame_edge(&m->vdp);
    if (m->frame_pos < p)
        p = m->frame_pos;
    m->event_cycle = sms_cycle_of(m, p);
}

void sms_catch_up(sms *m, uint64_t now)
{
    uint64_t last = sms_vdp_count(m, now);
    if (is_sg(m)) {
        if (last >= m->tms.pos) {
            sms_tms_run(&m->tms, last);
            plan(m);
        }
    } else if (last >= m->vdp.pos) {
        sms_vdp_run(&m->vdp, last);
        plan(m);
    }
}

bool sms_int_line(sms *m, uint64_t now)
{
    if (now >= m->event_cycle)
        sms_catch_up(m, now);
    return is_sg(m) ? m->tms.irq_level : m->vdp.irq_level;
}


/* Memory. */

int sms_init(sms *m, const sms_config *cfg, const uint8_t *rom, size_t rom_len)
{
    memset(m, 0, sizeof *m);
    m->system = cfg->system;
    m->region = cfg->region;
    m->mapper = cfg->mapper;
    uint32_t size = 1;
    while (size < rom_len)
        size <<= 1;
    m->rom = malloc(size);
    if (!m->rom)
        return -1;
    if (rom_len)
        memcpy(m->rom, rom, rom_len);
    for (uint32_t a = (uint32_t)rom_len; a < size; a++)
        m->rom[a] = rom_len ? m->rom[mirror(a, (uint32_t)rom_len)] : 0xFF;
    m->rom_mask = size - 1u;
    m->rom_size = (uint32_t)rom_len;
    m->cart_ram_mask = m->mapper == SMS_MAPPER_CODEMASTERS ? 0x1FFFu : 0x7FFFu;
    m->bank[0] = 0;
    m->bank[1] = 1;
    m->bank[2] = m->mapper == SMS_MAPPER_CODEMASTERS ? 0 : 2;
    if (cfg->system == SMS_SYSTEM_SG) {
        memset(m->ram, 0xFF, sizeof m->ram);
    } else {
        m->ram[0] = 0xAB;
        m->ram[0x0700] = 0x9B;
    }
    m->io_enable = m->ram_enable = m->card_enable = m->cart_enable = m->exp_enable = true;
    m->gg_parallel = 0x7F;
    m->gg_direction = 0x7F;
    m->gg_nmi_enable = true;
    m->gg_receive = 0xFF;
    m->pullup = 0xFF;
    for (unsigned i = 0; i < 2; i++) {
        m->tr_dir[i] = m->th_dir[i] = true;
        m->tr_level[i] = m->th_level[i] = true;
    }
    sms_vdp_mode mode = m->system == SMS_SYSTEM_GG ? SMS_VDP_GG : m->system == SMS_SYSTEM_GG_MS ? SMS_VDP_GG_MS : SMS_VDP_MS;
    bool pal = m->region == SMS_REGION_PAL;
    m->vdp.on_frame = frame_edge;
    m->vdp.user = m;
    sms_vdp_reset(&m->vdp, mode, 2, pal);
    m->tms.on_frame = frame_edge;
    m->tms.user = m;
    sms_tms_reset(&m->tms);
    sms_psg_reset(&m->psg, is_gg(m));
    m->fm = m->system == SMS_SYSTEM_MS && m->region == SMS_REGION_NTSC_J;
    m->fm_mute = true;
    sms_ym2413_reset(&m->fm_chip);
    clock_init(&m->clock, pal);
    plan(m);
    return 0;
}

void sms_free(sms *m)
{
    free(m->rom);
    m->rom = NULL;
}

static uint8_t rom_at(const sms *m, uint32_t offset) { return m->rom[offset & m->rom_mask]; }

static uint32_t sega_offset(const sms *m, unsigned slot, uint16_t a)
{
    return (uint32_t)m->bank_upper << 17 | (uint32_t)m->bank[slot] << 14 | (a & 0x3FFFu);
}

static uint8_t cart_read(const sms *m, uint16_t a, uint8_t data)
{
    switch (m->mapper) {
    case SMS_MAPPER_SEGA:
        if (a <= 0x03FF)
            return rom_at(m, a);
        if (a <= 0xBFFF) {
            if (a >= 0x8000 && m->ram_bank2)
                return m->cart_ram[((uint32_t)m->ram_bank2_select << 14 | (a & 0x3FFFu)) & m->cart_ram_mask];
            return rom_at(m, sega_offset(m, a >> 14, a));
        }
        return m->ram_bank3 ? m->cart_ram[a & 0x3FFFu & m->cart_ram_mask] : data;
    case SMS_MAPPER_CODEMASTERS:
        if (a <= 0x9FFF)
            return rom_at(m, (uint32_t)m->bank[a >> 14] << 14 | (a & 0x3FFFu));
        if (a <= 0xBFFF)
            return m->cm_ram ? m->cart_ram[a & 0x1FFFu] : rom_at(m, (uint32_t)m->bank[2] << 14 | (a & 0x3FFFu));
        return data;
    case SMS_MAPPER_KOREA:
        if (a <= 0x7FFF)
            return rom_at(m, a);
        if (a <= 0xBFFF)
            return rom_at(m, (uint32_t)m->bank[2] << 14 | (a & 0x3FFFu));
        return data;
    }
    return data;
}

static void cart_write(sms *m, uint16_t a, uint8_t data)
{
    switch (m->mapper) {
    case SMS_MAPPER_SEGA:
        switch (a) {
        case 0xFFFC: {
            static const uint8_t shift[4] = {0, 3, 2, 1};
            m->bank_shift = shift[data & 3u];
            m->ram_bank2_select = (data >> 2) & 1u;
            m->ram_bank2 = (data >> 3) & 1u;
            m->ram_bank3 = (data >> 4) & 1u;
            break;
        }
        case 0xFFFD:
        case 0xFFFE:
        case 0xFFFF:
            m->bank_upper = m->bank_shift;
            m->bank[a - 0xFFFDu] = data;
            break;
        default:
            break;
        }
        if (a >= 0x8000 && a <= 0xBFFF) {
            if (m->ram_bank2)
                m->cart_ram[((uint32_t)m->ram_bank2_select << 14 | (a & 0x3FFFu)) & m->cart_ram_mask] = data;
        } else if (a >= 0xC000 && m->ram_bank3) {
            m->cart_ram[a & 0x3FFFu & m->cart_ram_mask] = data;
        }
        return;
    case SMS_MAPPER_CODEMASTERS:
        if (a <= 0x3FFF) {
            m->bank[0] = data;
        } else if (a <= 0x7FFF) {
            m->bank[1] = data;
            m->cm_ram = data >> 7;
        } else if (a <= 0x9FFF) {
            m->bank[2] = data;
        } else if (a <= 0xBFFF) {
            if (m->cm_ram)
                m->cart_ram[a & 0x1FFFu] = data;
            else
                m->bank[2] = data;
        }
        return;
    case SMS_MAPPER_KOREA:
        if (a >= 0xA000 && a <= 0xBFFF)
            m->bank[2] = data;
        return;
    }
}

uint8_t sms_peek(const sms *m, uint16_t a)
{
    if (is_sg(m)) {
        if (a <= 0x7FFF || a < m->rom_size)
            return m->rom[a & m->rom_mask];
        return a >= 0xC000 ? m->ram[a & 0x3FFu] : 0xFF;
    }
    uint8_t data = m->pullup;
    if (a >= 0xC000 && m->ram_enable)
        data = m->ram[a & 0x1FFFu];
    if (!is_gg(m) ? m->cart_enable : (a >= 0x400 || !m->bios_enable))
        data = cart_read(m, a, data);
    return data;
}

uint8_t sms_read(sms *m, uint16_t a, uint64_t now)
{
    (void)now;
    return sms_peek(m, a);
}

void sms_write(sms *m, uint16_t a, uint8_t data, uint64_t now)
{
    (void)now;
    if (is_sg(m)) {
        if (a >= 0xC000 && !(a <= 0x7FFF || a < m->rom_size))
            m->ram[a & 0x3FFu] = data;
        return;
    }
    if (a >= 0xC000 && m->ram_enable)
        m->ram[a & 0x1FFFu] = data;
    if (!is_gg(m) ? m->cart_enable : (a >= 0x400 || !m->bios_enable))
        cart_write(m, a, data);
}

static int32_t padded_slot_bank(const sms *m, unsigned s)
{
    if (is_sg(m))
        return s < 2 || s * 0x4000u < m->rom_size ? (int32_t)(((s * 0x4000u) & m->rom_mask) >> 14) : -1;
    switch (m->mapper) {
    case SMS_MAPPER_SEGA:
        if (s == 2 && m->ram_bank2)
            return -1;
        return (int32_t)((sega_offset(m, s, 0) & m->rom_mask) >> 14);
    case SMS_MAPPER_CODEMASTERS:
        return (int32_t)((((uint32_t)m->bank[s] << 14) & m->rom_mask) >> 14);
    case SMS_MAPPER_KOREA:
        return s < 2 ? (int32_t)s : (int32_t)((((uint32_t)m->bank[2] << 14) & m->rom_mask) >> 14);
    }
    return -1;
}

int32_t sms_slot_bank(const sms *m, unsigned s)
{
    int32_t b = padded_slot_bank(m, s);
    if (b < 0 || (uint32_t)b << 14 < m->rom_size || !m->rom_size)
        return b;
    return (int32_t)(mirror((uint32_t)b << 14, m->rom_size) >> 14);
}

/* Ports. */

static uint8_t pad_port1(sms *m)
{
    pad_latch(&m->pad, m->buttons);
    uint8_t d = 0;
    d |= !m->pad.up ? 0x01u : 0;
    d |= !m->pad.down ? 0x02u : 0;
    d |= !m->pad.left ? 0x04u : 0;
    d |= !m->pad.right ? 0x08u : 0;
    d |= !(m->buttons & SMS_BUTTON_1) ? 0x10u : 0;
    d |= !(m->buttons & SMS_BUTTON_2) ? 0x20u : 0;
    return (uint8_t)(d | 0x40u);
}

static uint8_t gg_line(const sms *m, unsigned bit)
{
    return (m->gg_direction >> bit) & 1u ? 1u : (m->gg_parallel >> bit) & 1u;
}

static uint8_t sg_in(sms *m, uint8_t p, uint64_t now)
{
    switch (p >> 6) {
    case 2: {
        sms_catch_up(m, now);
        uint8_t data = p & 1u ? sms_tms_status(&m->tms) : sms_tms_data_read(&m->tms);
        plan(m);
        return data;
    }
    case 3:
        if (p & 1u)
            return 0xFF;
        sms_catch_up(m, now);
        return (uint8_t)(pad_port1(m) | 0xC0u);
    default:
        return 0xFF;
    }
}

static void sg_out(sms *m, uint8_t p, uint8_t data, uint64_t now)
{
    switch (p >> 6) {
    case 1:
        sms_psg_write(&m->psg, now, data);
        return;
    case 2:
        sms_catch_up(m, now);
        if (p & 1u)
            sms_tms_control(&m->tms, data);
        else
            sms_tms_data_write(&m->tms, data);
        plan(m);
        return;
    default:
        return;
    }
}

uint8_t sms_in(sms *m, uint16_t port, uint64_t now)
{
    uint8_t p = (uint8_t)port;
    if (is_sg(m))
        return sg_in(m, p, now);
    uint8_t data = m->pullup;
    bool gg = gg_mode(m);
    if (p <= 0x05 && gg) {
        switch (p) {
        case 0x00:
            sms_catch_up(m, now);
            m->start_value = m->buttons & SMS_BUTTON_START;
            data = (uint8_t)((m->region == SMS_REGION_PAL ? 0x20u : 0) | (m->region != SMS_REGION_NTSC_J ? 0x40u : 0) |
                             (!m->start_value ? 0x80u : 0));
            break;
        case 0x01:
            data = 0;
            for (unsigned b = 0; b < 7; b++)
                data |= (uint8_t)(gg_line(m, b) << b);
            data |= (uint8_t)(m->gg_parallel & 0x80u);
            break;
        case 0x02:
            data = (uint8_t)((m->gg_direction & 0x7Fu) | (m->gg_nmi_enable ? 0x80u : 0));
            break;
        case 0x03:
            data = m->gg_transmit;
            break;
        case 0x04:
            data = m->gg_receive;
            break;
        default:
            data = (uint8_t)(m->gg_serial & 0xF8u);
            break;
        }
        return data;
    }
    if (p == 0x06 && is_gg(m))
        return data;
    switch (p & 0xC1u) {
    case 0x00:
        return data;
    case 0x01:
        if (!is_gg(m) && m->io_enable && m->region != SMS_REGION_NTSC_J)
            data = (uint8_t)(m->tr_dir[0] | m->th_dir[0] << 1 | m->tr_dir[1] << 2 | m->th_dir[1] << 3 | m->tr_level[0] << 4 |
                             m->th_level[0] << 5 | m->tr_level[1] << 6 | m->th_level[1] << 7);
        return data;
    case 0x40:
        sms_catch_up(m, now);
        return sms_vdp_vcounter(&m->vdp);
    case 0x41:
        sms_catch_up(m, now);
        return sms_vdp_hcounter(&m->vdp);
    case 0x80:
        sms_catch_up(m, now);
        data = sms_vdp_data_read(&m->vdp);
        plan(m);
        return data;
    case 0x81:
        sms_catch_up(m, now);
        data = sms_vdp_status(&m->vdp);
        plan(m);
        return data;
    default:
        break;
    }
    if (p == 0xF2 && m->fm) {
        static const uint8_t mode[2][2] = {{3, 0}, {1, 2}};
        sms_catch_up(m, now);
        uint16_t cc = sms_vdp_ccounter(&m->vdp);
        return (uint8_t)(mode[m->psg_mute][m->fm_mute] | ((cc >> 3) & 1u) << 5 | ((cc >> 7) & 1u) << 6 | ((cc >> 11) & 1u) << 7);
    }
    if (!is_gg(m)) {
        sms_catch_up(m, now);
        uint8_t p1 = pad_port1(m), p2 = 0x7F;
        if ((p & 0xC1u) == 0xC0) {
            data = (uint8_t)((p1 & 0x3Fu) | (p2 & 3u) << 6);
            if (!m->tr_dir[0])
                data = (uint8_t)((data & ~0x20u) | m->tr_level[0] << 5);
        } else {
            data = (uint8_t)(((p2 >> 2) & 15u) | (m->region == SMS_REGION_NTSC_J ? 0x10u : (!m->reset_value ? 0x10u : 0)) |
                             0x20u | ((p1 >> 6) & 1u) << 6 | ((p2 >> 6) & 1u) << 7);
            if (!m->tr_dir[1])
                data = (uint8_t)((data & ~0x08u) | m->tr_level[1] << 3);
            if (!m->th_dir[0])
                data = (uint8_t)((data & ~0x40u) | m->th_level[0] << 6);
            if (!m->th_dir[1])
                data = (uint8_t)((data & ~0x80u) | m->th_level[1] << 7);
        }
        return data;
    }
    if (p == 0xC0 || p == 0xDC) {
        sms_catch_up(m, now);
        gg_poll(m, m->buttons);
        data = (uint8_t)((!m->pad.up ? 0x01u : 0) | (!m->pad.down ? 0x02u : 0) | (!m->pad.left ? 0x04u : 0) |
                         (!m->pad.right ? 0x08u : 0) | (!(m->buttons & SMS_BUTTON_1) ? 0x10u : 0) |
                         (!(m->buttons & SMS_BUTTON_2) ? 0x20u : 0) | gg_line(m, 0) << 6 | gg_line(m, 1) << 7);
    } else if (p == 0xC1 || p == 0xDD) {
        data = (uint8_t)(gg_line(m, 2) | gg_line(m, 3) << 1 | gg_line(m, 4) << 2 | gg_line(m, 5) << 3 | 0x70u |
                         gg_line(m, 6) << 7);
    }
    return data;
}

void sms_out(sms *m, uint16_t port, uint8_t data, uint64_t now)
{
    uint8_t p = (uint8_t)port;
    if (is_sg(m)) {
        sg_out(m, p, data, now);
        return;
    }
    if (p <= 0x06 && is_gg(m)) {
        switch (p) {
        case 0x01:
            m->gg_parallel = data;
            break;
        case 0x02:
            m->gg_direction = data & 0x7Fu;
            m->gg_nmi_enable = data >> 7;
            break;
        case 0x03:
            m->gg_transmit = data;
            break;
        case 0x05:
            m->gg_serial = data & 0xF8u;
            break;
        case 0x06:
            sms_psg_stereo(&m->psg, now, data);
            break;
        default:
            break;
        }
    }
    if (p == 0x3E || (!is_gg(m) && (p & 0xC1u) == 0x00)) {
        m->io_enable = !((data >> 2) & 1u);
        m->bios_enable = !((data >> 3) & 1u);
        m->ram_enable = !((data >> 4) & 1u);
        m->card_enable = !((data >> 5) & 1u);
        m->cart_enable = !((data >> 6) & 1u);
        m->exp_enable = !((data >> 7) & 1u);
        return;
    }
    if (!is_gg(m) && (p & 0xC1u) == 0x01) {
        bool th1 = m->th_level[0], th2 = m->th_level[1];
        m->tr_dir[0] = data & 1u;
        m->th_dir[0] = (data >> 1) & 1u;
        m->tr_dir[1] = (data >> 2) & 1u;
        m->th_dir[1] = (data >> 3) & 1u;
        if (m->region == SMS_REGION_NTSC_J) {
            m->th_level[0] = m->th_dir[0];
            m->th_level[1] = m->th_dir[1];
        } else {
            m->tr_level[0] = (data >> 4) & 1u;
            m->th_level[0] = (data >> 5) & 1u;
            m->tr_level[1] = (data >> 6) & 1u;
            m->th_level[1] = (data >> 7) & 1u;
        }
        if ((!th1 && m->th_level[0]) || (!th2 && m->th_level[1])) {
            sms_catch_up(m, now);
            sms_vdp_latch_h(&m->vdp);
        }
        return;
    }
    if (m->fm && p >= 0xF0 && p <= 0xF2) {
        if (p == 0xF0) {
            sms_ym2413_address(&m->fm_chip, now, data);
        } else if (p == 0xF1) {
            sms_ym2413_write(&m->fm_chip, now, data);
        } else {
            static const bool psg_mute[4] = {false, true, true, false}, fm_mute[4] = {true, false, true, false};
            m->psg_mute = psg_mute[data & 3u];
            m->fm_mute = fm_mute[data & 3u];
        }
        return;
    }
    switch (p & 0xC0u) {
    case 0x40:
        sms_psg_write(&m->psg, now, data);
        return;
    case 0x80:
        sms_catch_up(m, now);
        if (p & 1u)
            sms_vdp_control(&m->vdp, data);
        else
            sms_vdp_data_write(&m->vdp, data);
        plan(m);
        return;
    default:
        return;
    }
}

/* Picture. */

void sms_screen(const sms *m, uint32_t *w, uint32_t *h)
{
    if (m->system == SMS_SYSTEM_GG) {
        *w = 160;
        *h = 144;
    } else {
        *w = SMS_VDP_W;
        *h = m->picture_lines ? m->picture_lines : 192;
    }
}

void sms_picture(const sms *m, uint16_t *out)
{
    uint32_t w, h;
    sms_screen(m, &w, &h);
    uint32_t x0 = m->system == SMS_SYSTEM_GG ? 48 : 0, y0 = m->system == SMS_SYSTEM_GG ? 24 : 0;
    for (uint32_t y = 0; y < h; y++)
        memcpy(out + (size_t)y * w, m->picture + (size_t)(y0 + y) * SMS_VDP_W + x0, w * sizeof *out);
}

void sms_rgb(const sms *m, uint16_t color, uint8_t rgb[3])
{
    if (is_sg(m)) {
        sms_tms_rgb((uint8_t)color, rgb);
        return;
    }
    if (m->system == SMS_SYSTEM_MS) {
        static const uint8_t blue[4] = {0, 104, 170, 255};
        rgb[0] = (uint8_t)((color & 3u) * 0x55u);
        rgb[1] = (uint8_t)(((color >> 2) & 3u) * 0x55u);
        rgb[2] = blue[(color >> 4) & 3u];
        return;
    }
    rgb[0] = (uint8_t)((color & 15u) * 0x11u);
    rgb[1] = (uint8_t)(((color >> 4) & 15u) * 0x11u);
    rgb[2] = (uint8_t)(((color >> 8) & 15u) * 0x11u);
}
