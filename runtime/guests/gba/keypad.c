/**
 * @file keypad.c
 * @brief The keypad: KEYINPUT from the p1 port of the GBA's native port
 * table (or a mask set by the caller), KEYCNT and its interrupt. The ports
 * are sampled when KEYINPUT is read, at most once per scanline, and by a
 * poll event on every scanline while the KEYCNT interrupt is enabled.
 */
#include "gba_int.h"

#include <relift/ports.h>

enum { KEY_COUNT = 10, KEY_MASK = 0x3FF, CNT_MASK = 0xC3FF, CNT_IRQ = 0x4000, CNT_AND = 0x8000 };

static const char *const key_names[KEY_COUNT] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l"};

static bool condition(const gba_keypad *k)
{
    uint16_t mask = k->cnt & KEY_MASK;
    if (!(k->cnt & CNT_IRQ))
        return false;
    if (k->cnt & CNT_AND)
        return (k->keys & mask) == mask;
    return (k->keys & mask) != 0;
}

static void evaluate(rl_gba *g, uint64_t now)
{
    if (condition(&g->pad))
        gba_irq_set(g, RL_GBA_IRQ_KEYPAD, now);
}

static uint16_t read_ports(gba_keypad *k)
{
    if (!k->resolved) {
        k->resolved = true;
        for (unsigned i = 0; i < KEY_COUNT; i++) {
            k->ref[i] = rl_ports_find("p1", key_names[i]);
            if (k->ref[i] == RL_PORT_NONE)
                k->resolved = false;
        }
    }
    uint16_t keys = 0;
    for (unsigned i = 0; i < KEY_COUNT; i++)
        if (k->ref[i] != RL_PORT_NONE && rl_ports_read(k->ref[i]) >= 0.5f)
            keys |= (uint16_t)(1u << i);
    return keys;
}

static void sample(rl_gba *g, uint64_t now)
{
    gba_keypad *k = &g->pad;
    if (k->manual)
        return;
    if (k->sampled_once && now < k->sampled + RL_GBA_LINE_CYCLES)
        return;
    k->sampled_once = true;
    k->sampled = now;
    uint16_t keys = read_ports(k);
    if (keys != k->keys) {
        k->keys = keys;
        evaluate(g, now);
    }
}

static void repost(rl_gba *g, uint64_t from)
{
    gba_keypad *k = &g->pad;
    if (!g->sched)
        return;
    if ((k->cnt & CNT_IRQ) && !k->manual) {
        if (!rl_event_queued(&k->ev))
            rl_sched_post(g->sched, &k->ev, from + RL_GBA_LINE_CYCLES);
    } else {
        rl_sched_cancel(g->sched, &k->ev);
    }
}

static void poll_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_gba *g = e->opaque;
    g->pad.sampled_once = false;
    sample(g, when);
    repost(g, when);
}

void gba_keypad_attach(rl_gba *g)
{
    g->pad.ev = (rl_event){.fire = poll_fire, .opaque = g};
}

uint8_t gba_keypad_read(rl_gba *g, uint32_t addr, uint64_t now)
{
    sample(g, now);
    gba_keypad *k = &g->pad;
    switch (addr & 3u) {
    case 0:
        return (uint8_t)~k->keys;
    case 1:
        return (uint8_t)((~k->keys >> 8) & 3u);
    case 2:
        return (uint8_t)k->cnt;
    default:
        return (uint8_t)((k->cnt >> 8) & 0xC3u);
    }
}

void gba_keypad_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    gba_keypad *k = &g->pad;
    switch (addr & 3u) {
    case 2:
        k->cnt = (uint16_t)((k->cnt & 0xFF00u) | v);
        break;
    case 3:
        k->cnt = (uint16_t)((k->cnt & 0x00FFu) | (v & 0xC3u) << 8);
        break;
    default:
        return;
    }
    k->cnt &= CNT_MASK;
    sample(g, now);
    evaluate(g, now);
    repost(g, now);
}

void rl_gba_keypad_set(rl_gba *g, uint16_t pressed)
{
    gba_keypad *k = &g->pad;
    k->manual = true;
    pressed &= KEY_MASK;
    if (g->sched)
        rl_sched_cancel(g->sched, &k->ev);
    if (pressed != k->keys) {
        k->keys = pressed;
        evaluate(g, gba_now(g));
    }
}

void rl_gba_keypad_use_ports(rl_gba *g)
{
    g->pad.manual = false;
    g->pad.sampled_once = false;
    repost(g, gba_now(g));
}

uint16_t rl_gba_keypad_pressed(const rl_gba *g) { return g->pad.keys; }
