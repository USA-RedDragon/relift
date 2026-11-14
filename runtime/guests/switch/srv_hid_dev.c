/**
 * @file srv_hid_dev.c
 * @brief The hid devices: the shared memory rings, the npad configuration
 * and the update that turns host input (rl_input_sample) into ring entries.
 *
 * The ring and device rules follow Ryujinx (MIT, Copyright (c) Ryujinx Team
 * and Contributors) Hid/Types/SharedMemory and Hid/HidDevices.
 */
#include "hid_priv.h"

#include <pthread.h>
#include <string.h>

#define NPAD_SLOTS 9u
#define UPDATE_NS 16666667ull

enum {
    COLOR_BODY_GRAY = 0x828282,
    COLOR_BODY_NEON_RED = 0xFF3C28,
    COLOR_BODY_NEON_BLUE = 0x0AB9E6,
    COLOR_BUTTON_GRAY = 0x0F0F0F
};

enum {
    DEV_FULL_KEY = 1u << 0,
    DEV_HANDHELD_LEFT = 1u << 2,
    DEV_HANDHELD_RIGHT = 1u << 3,
    DEV_JOY_LEFT = 1u << 4,
    DEV_JOY_RIGHT = 1u << 5,
    DEV_PALMA = 1u << 6
};

enum {
    PROP_POWERED_DUAL = 1u << 3,
    PROP_POWERED_LEFT = 1u << 4,
    PROP_POWERED_RIGHT = 1u << 5,
    PROP_ABXY = 1u << 11,
    PROP_SL_SR = 1u << 12,
    PROP_PLUS = 1u << 13,
    PROP_MINUS = 1u << 14
};

enum {
    FOOTER_NONE = 0,
    FOOTER_HANDHELD_LEFT_ONLY = 2,
    FOOTER_HANDHELD_RIGHT_ONLY = 3,
    FOOTER_HANDHELD_BOTH = 4,
    FOOTER_PRO = 12
};

enum {
    DEFAULT_STYLES = HID_STYLE_FULL_KEY | HID_STYLE_HANDHELD | HID_STYLE_JOY_DUAL | HID_STYLE_JOY_LEFT | HID_STYLE_JOY_RIGHT
};

typedef struct touch_prev {
    uint32_t id;
    uint16_t x, y;
    uint64_t start_ns;
} touch_prev;

static struct {
    hos_shmem *shm;
    hid_shared_memory *mem;
    hos_revent *events[NPAD_SLOTS];
    pthread_t thread;
    bool thread_up;
    bool thread_done;
    bool stop;
    bool manual;
    bool active[6];
    uint32_t styles;
    bool players[NPAD_SLOTS];
    uint64_t joy_hold;
    float vibration[NPAD_SLOTS][2][4];
    touch_prev prev[HID_TOUCHES];
    uint32_t prev_count;
    uint64_t last_ns;
    uint64_t ts;
    uint64_t dt_ns;
    uint64_t now_ns;
} g;

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;

bool hid_slot_from_id(uint32_t id, uint32_t *slot)
{
    if (id <= HID_ID_PLAYER8)
        *slot = id;
    else if (id == HID_ID_HANDHELD)
        *slot = HID_NPAD_HANDHELD;
    else if (id == HID_ID_UNKNOWN)
        *slot = 9;
    else
        return false;
    return true;
}

uint32_t hid_id_from_slot(uint32_t slot)
{
    if (slot <= HID_ID_PLAYER8)
        return slot;
    return slot == HID_NPAD_HANDHELD ? HID_ID_HANDHELD : HID_ID_UNKNOWN;
}

static void lifo_init(hid_lifo_header *h)
{
    memset(h, 0, sizeof *h);
    h->buffer_count = HID_LIFO_ENTRIES;
}

static void lifo_clear(hid_lifo_header *h)
{
    __atomic_store_n(&h->count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&h->tail, 0, __ATOMIC_RELEASE);
}

static void lifo_push(hid_lifo_header *h, uint8_t *entries, size_t esz, void *state, size_t ssz, size_t samp_off)
{
    uint64_t count = __atomic_load_n(&h->count, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
    uint64_t prev = 0;
    if (count)
        memcpy(&prev, entries + tail * esz, 8);
    uint64_t n = prev + 1;
    memcpy((uint8_t *)state + samp_off, &n, 8);
    uint64_t target = (tail + 1) % HID_LIFO_ENTRIES;
    uint8_t *e = entries + target * esz;
    __atomic_store_n((uint64_t *)(void *)e, n, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(e + 8, state, ssz);
    h->timestamp = g.ts;
    __atomic_store_n(&h->tail, target, __ATOMIC_RELEASE);
    if (count < HID_LIFO_ENTRIES - 1)
        __atomic_store_n(&h->count, count + 1, __ATOMIC_RELEASE);
}

#define PUSH(lifo, st, off) lifo_push(&(lifo)->header, (uint8_t *)(lifo)->entries, sizeof((lifo)->entries[0]), &(st), sizeof(st), off)

static void npad_lifos_init(hid_npad_internal *n)
{
    hid_npad_lifo *common[] = { &n->full_key, &n->handheld, &n->joy_dual, &n->joy_left, &n->joy_right, &n->palma, &n->system_ext };
    hid_six_axis_lifo *six[] = { &n->full_key_six, &n->handheld_six, &n->joy_dual_six, &n->joy_dual_right_six, &n->joy_left_six, &n->joy_right_six };
    for (size_t i = 0; i < sizeof common / sizeof common[0]; i++)
        lifo_init(&common[i]->header);
    for (size_t i = 0; i < sizeof six / sizeof six[0]; i++)
        lifo_init(&six[i]->header);
    lifo_init(&n->gc_trigger.header);
}

static void shm_init(hid_shared_memory *m)
{
    memset(m, 0, sizeof *m);
    lifo_init(&m->debug_pad.header);
    lifo_init(&m->touch_screen.header);
    lifo_init(&m->mouse.header);
    lifo_init(&m->keyboard.header);
    lifo_init(&m->debug_mouse.header);
    for (uint32_t i = 0; i < HID_NPADS; i++)
        npad_lifos_init(&m->npads[i].state);
    m->condition.a = 0;
    m->condition.b = 1;
    m->condition.hold_type = 1;
    m->condition.c = 1;
}

static void reset_config(void)
{
    g.styles = DEFAULT_STYLES;
    for (uint32_t i = 0; i < NPAD_SLOTS; i++)
        g.players[i] = true;
    g.joy_hold = 0;
}

static hid_npad_lifo *npad_common_lifo(hid_npad_internal *n)
{
    switch (n->style_set) {
    case HID_STYLE_FULL_KEY:
        return &n->full_key;
    case HID_STYLE_HANDHELD:
        return &n->handheld;
    case HID_STYLE_JOY_DUAL:
        return &n->joy_dual;
    case HID_STYLE_JOY_LEFT:
        return &n->joy_left;
    case HID_STYLE_JOY_RIGHT:
        return &n->joy_right;
    case HID_STYLE_PALMA:
        return &n->palma;
    default:
        return &n->system_ext;
    }
}

static bool npad_setup(uint32_t slot, uint32_t style)
{
    hid_npad_internal *n = &g.mem->npads[slot].state;
    if (n->style_set == style)
        return false;
    memset(n, 0, sizeof *n);
    npad_lifos_init(n);
    if (style == 0)
        return true;
    n->joy_assignment_mode = 0;
    n->full_key_color.body = COLOR_BODY_GRAY;
    n->full_key_color.buttons = COLOR_BUTTON_GRAY;
    n->joy_color.left_body = COLOR_BODY_NEON_BLUE;
    n->joy_color.left_buttons = COLOR_BUTTON_GRAY;
    n->joy_color.right_body = COLOR_BODY_NEON_RED;
    n->joy_color.right_buttons = COLOR_BUTTON_GRAY;
    n->system_properties = PROP_POWERED_DUAL | PROP_POWERED_LEFT | PROP_POWERED_RIGHT;
    for (int i = 0; i < 3; i++)
        n->battery_level[i] = 4;
    n->style_set = style;
    switch (style) {
    case HID_STYLE_FULL_KEY:
        n->device_type = DEV_FULL_KEY;
        n->system_properties |= PROP_ABXY | PROP_PLUS | PROP_MINUS;
        n->applet_footer_ui_type = FOOTER_PRO;
        break;
    case HID_STYLE_HANDHELD:
        n->device_type = DEV_HANDHELD_LEFT | DEV_HANDHELD_RIGHT;
        n->system_properties |= PROP_ABXY | PROP_PLUS | PROP_MINUS;
        n->applet_footer_ui_type = FOOTER_HANDHELD_BOTH;
        break;
    case HID_STYLE_JOY_DUAL:
        n->device_type = DEV_JOY_LEFT | DEV_JOY_RIGHT;
        n->system_properties |= PROP_ABXY | PROP_PLUS | PROP_MINUS;
        n->applet_footer_ui_type = FOOTER_HANDHELD_BOTH;
        break;
    case HID_STYLE_JOY_LEFT:
        n->joy_assignment_mode = 1;
        n->device_type = DEV_JOY_LEFT;
        n->system_properties |= PROP_SL_SR | PROP_MINUS;
        n->applet_footer_ui_type = FOOTER_HANDHELD_LEFT_ONLY;
        break;
    case HID_STYLE_JOY_RIGHT:
        n->joy_assignment_mode = 1;
        n->device_type = DEV_JOY_RIGHT;
        n->system_properties |= PROP_SL_SR | PROP_PLUS;
        n->applet_footer_ui_type = FOOTER_HANDHELD_RIGHT_ONLY;
        break;
    case HID_STYLE_PALMA:
        n->device_type = DEV_PALMA;
        n->applet_footer_ui_type = FOOTER_NONE;
        break;
    default:
        break;
    }
    return true;
}

static uint32_t style_for_pad(void)
{
    uint32_t sup = g.styles;
    if (sup & HID_STYLE_FULL_KEY)
        return HID_STYLE_FULL_KEY;
    if (sup & HID_STYLE_JOY_DUAL)
        return HID_STYLE_JOY_DUAL;
    return 0;
}

static uint32_t desired_style(uint32_t slot, const rl_input_snapshot *s)
{
    if (slot >= NPAD_SLOTS || !g.players[slot])
        return 0;
    if (slot < RL_INPUT_PADS) {
        if (!s->pads[slot].connected)
            return 0;
        return style_for_pad();
    }
    if (s->pads[0].connected && (g.styles & HID_STYLE_HANDHELD) && style_for_pad() == 0)
        return HID_STYLE_HANDHELD;
    return 0;
}

enum {
    PORT_A, PORT_B, PORT_X, PORT_Y, PORT_LSTICK, PORT_RSTICK, PORT_L, PORT_R, PORT_ZL, PORT_ZR,
    PORT_PLUS, PORT_MINUS, PORT_UP, PORT_RIGHT, PORT_DOWN, PORT_LEFT,
    PORT_LX, PORT_LY, PORT_RX, PORT_RY, PORT_CONTROLS
};

static const struct {
    const char *id;
    uint64_t button;
} k_port_controls[PORT_CONTROLS] = {
    { "a", HID_BTN_A }, { "b", HID_BTN_B }, { "x", HID_BTN_X }, { "y", HID_BTN_Y },
    { "lstick", HID_BTN_STICK_L }, { "rstick", HID_BTN_STICK_R }, { "l", HID_BTN_L },
    { "r", HID_BTN_R }, { "zl", HID_BTN_ZL }, { "zr", HID_BTN_ZR }, { "plus", HID_BTN_PLUS },
    { "minus", HID_BTN_MINUS }, { "dpad_up", HID_BTN_UP }, { "dpad_right", HID_BTN_RIGHT },
    { "dpad_down", HID_BTN_DOWN }, { "dpad_left", HID_BTN_LEFT }, { "lstick_x", 0 },
    { "lstick_y", 0 }, { "rstick_x", 0 }, { "rstick_y", 0 },
};

static rl_control_ref g_port_ref[RL_INPUT_PADS][PORT_CONTROLS];
static bool g_ports_ready;

static void ports_init(void)
{
    if (g_ports_ready)
        return;
    rl_ports_declare(rl_switch_ports, rl_switch_ports_count);
    for (uint32_t p = 0; p < RL_INPUT_PADS; p++) {
        char id[3] = { 'p', (char)('1' + p), 0 };
        for (uint32_t c = 0; c < PORT_CONTROLS; c++)
            g_port_ref[p][c] = rl_ports_find(id, k_port_controls[c].id);
    }
    g_ports_ready = true;
}

static int16_t axis_value(float v)
{
    float f = v * 32767.0f;
    long r = (long)(f < 0 ? f - 0.5f : f + 0.5f);
    return (int16_t)(r < -32767 ? -32767 : r > 32767 ? 32767 : r);
}

/** The pad of a player as the bound inputs report it. */
static rl_input_pad bound_pad(uint32_t player, const rl_input_snapshot *s, uint64_t *buttons)
{
    rl_input_pad p;
    memset(&p, 0, sizeof p);
    p.connected = s->pads[player].connected;
    uint64_t b = 0;
    for (uint32_t c = 0; c < PORT_LX; c++)
        if (rl_ports_eval(g_port_ref[player][c], s) > 0.5f)
            b |= k_port_controls[c].button;
    p.lx = axis_value(rl_ports_eval(g_port_ref[player][PORT_LX], s));
    p.ly = axis_value(rl_ports_eval(g_port_ref[player][PORT_LY], s));
    p.rx = axis_value(rl_ports_eval(g_port_ref[player][PORT_RX], s));
    p.ry = axis_value(rl_ports_eval(g_port_ref[player][PORT_RY], s));
    const int32_t th = 32767 / 2;
    if (p.lx < -th)
        b |= HID_BTN_STICK_L_LEFT;
    if (p.lx > th)
        b |= HID_BTN_STICK_L_RIGHT;
    if (p.ly < -th)
        b |= HID_BTN_STICK_L_DOWN;
    if (p.ly > th)
        b |= HID_BTN_STICK_L_UP;
    if (p.rx < -th)
        b |= HID_BTN_STICK_R_LEFT;
    if (p.rx > th)
        b |= HID_BTN_STICK_R_RIGHT;
    if (p.ry < -th)
        b |= HID_BTN_STICK_R_DOWN;
    if (p.ry > th)
        b |= HID_BTN_STICK_R_UP;
    *buttons = b;
    return p;
}

uint64_t hid_core_pad_buttons(uint32_t player, const rl_input_snapshot *s)
{
    uint64_t buttons = 0;
    if (player >= RL_INPUT_PADS)
        return 0;
    ports_init();
    (void)bound_pad(player, s, &buttons);
    return buttons;
}

static int32_t clamp_axis(int16_t v) { return v < -32767 ? -32767 : v; }

static hid_six_axis_state six_state(bool connected)
{
    hid_six_axis_state st;
    memset(&st, 0, sizeof st);
    st.delta_time = g.dt_ns;
    if (connected) {
        st.direction[0] = 1.0f;
        st.direction[4] = 1.0f;
        st.direction[8] = 1.0f;
        st.attributes = 1;
    }
    return st;
}

static void six_push(hid_six_axis_lifo *l, hid_six_axis_state st) { PUSH(l, st, 8); }

static void npad_write_six(hid_npad_internal *n)
{
    hid_six_axis_state live = six_state(true);
    hid_six_axis_state none = six_state(false);
    hid_six_axis_lifo *used = NULL;
    switch (n->style_set) {
    case HID_STYLE_FULL_KEY:
        used = &n->full_key_six;
        break;
    case HID_STYLE_HANDHELD:
        used = &n->handheld_six;
        break;
    case HID_STYLE_JOY_DUAL:
        used = &n->joy_dual_six;
        break;
    case HID_STYLE_JOY_LEFT:
        used = &n->joy_left_six;
        break;
    case HID_STYLE_JOY_RIGHT:
        used = &n->joy_right_six;
        break;
    default:
        return;
    }
    six_push(used, live);
    hid_six_axis_lifo *all[] = { &n->full_key_six, &n->handheld_six, &n->joy_dual_six, &n->joy_left_six, &n->joy_right_six };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        if (all[i] != used)
            six_push(all[i], none);
    hid_six_axis_state idle = none;
    idle.attributes = 1;
    six_push(&n->joy_dual_right_six, n->style_set == HID_STYLE_JOY_DUAL ? live : idle);
}

static void npad_write_input(hid_npad_internal *n, uint32_t player, const rl_input_snapshot *s)
{
    uint64_t buttons;
    const rl_input_pad bp = bound_pad(player, s, &buttons);
    const rl_input_pad *p = &bp;
    hid_npad_state st;
    memset(&st, 0, sizeof st);
    st.buttons = buttons;
    st.l.x = clamp_axis(p->lx);
    st.l.y = clamp_axis(p->ly);
    st.r.x = clamp_axis(p->rx);
    st.r.y = clamp_axis(p->ry);
    st.attributes = HID_ATTR_CONNECTED;
    switch (n->style_set) {
    case HID_STYLE_HANDHELD:
    case HID_STYLE_FULL_KEY:
        st.attributes |= HID_ATTR_WIRED;
        break;
    case HID_STYLE_JOY_DUAL:
        st.attributes |= HID_ATTR_LEFT_CONNECTED | HID_ATTR_RIGHT_CONNECTED;
        break;
    case HID_STYLE_JOY_LEFT:
        st.attributes |= HID_ATTR_LEFT_CONNECTED;
        break;
    case HID_STYLE_JOY_RIGHT:
        st.attributes |= HID_ATTR_RIGHT_CONNECTED;
        break;
    default:
        break;
    }
    hid_npad_lifo *used = npad_common_lifo(n);
    PUSH(used, st, 0);
    if (n->style_set != HID_STYLE_SYSTEM_EXT)
        PUSH(&n->system_ext, st, 0);
    hid_npad_state zero;
    memset(&zero, 0, sizeof zero);
    hid_npad_lifo *others[] = { &n->full_key, &n->handheld, &n->joy_dual, &n->joy_left, &n->joy_right, &n->palma };
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
        if (others[i] != used)
            PUSH(others[i], zero, 0);
    npad_write_six(n);
}

static void npad_write_disconnected(hid_npad_internal *n)
{
    hid_npad_state zero;
    memset(&zero, 0, sizeof zero);
    hid_npad_lifo *all[] = { &n->full_key, &n->handheld, &n->joy_dual, &n->joy_left, &n->joy_right, &n->palma };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        PUSH(all[i], zero, 0);
    hid_six_axis_state st = six_state(false);
    st.attributes = 1;
    hid_six_axis_lifo *six[] = { &n->full_key_six, &n->handheld_six, &n->joy_dual_six, &n->joy_dual_right_six, &n->joy_left_six, &n->joy_right_six };
    for (size_t i = 0; i < sizeof six / sizeof six[0]; i++)
        six_push(six[i], st);
}

static uint32_t update_npads(const rl_input_snapshot *s)
{
    ports_init();
    uint32_t changed = 0;
    for (uint32_t slot = 0; slot < NPAD_SLOTS; slot++) {
        if (npad_setup(slot, desired_style(slot, s)))
            changed |= 1u << slot;
        hid_npad_internal *n = &g.mem->npads[slot].state;
        if (n->style_set == 0) {
            npad_write_disconnected(n);
            continue;
        }
        npad_write_input(n, slot == HID_NPAD_HANDHELD ? 0 : slot, s);
    }
    npad_write_disconnected(&g.mem->npads[9].state);
    return changed;
}

static void update_touch(const rl_input_snapshot *s)
{
    hid_touch_screen_state st;
    memset(&st, 0, sizeof st);
    touch_prev cur[HID_TOUCHES];
    uint32_t ncur = 0;
    if (g.active[HID_DEV_TOUCH]) {
        uint32_t n = s->touch_count < HID_TOUCHES ? s->touch_count : HID_TOUCHES;
        uint32_t out = 0;
        for (uint32_t i = 0; i < n; i++) {
            const rl_input_touch *t = &s->touches[i];
            uint64_t start = g.now_ns;
            uint32_t attr = HID_TOUCH_START;
            for (uint32_t k = 0; k < g.prev_count; k++)
                if (g.prev[k].id == t->id) {
                    start = g.prev[k].start_ns;
                    attr = 0;
                }
            hid_touch_state *o = &st.touches[out++];
            o->delta_time = g.now_ns - start;
            o->attributes = attr;
            o->finger_id = t->id;
            o->x = (uint32_t)((uint64_t)t->x * HID_TOUCH_WIDTH / 65536u);
            o->y = (uint32_t)((uint64_t)t->y * HID_TOUCH_HEIGHT / 65536u);
            o->diameter_x = 15;
            o->diameter_y = 15;
            cur[ncur].id = t->id;
            cur[ncur].x = t->x;
            cur[ncur].y = t->y;
            cur[ncur].start_ns = start;
            ncur++;
        }
        for (uint32_t k = 0; k < g.prev_count && out < HID_TOUCHES; k++) {
            bool still = false;
            for (uint32_t i = 0; i < ncur; i++)
                if (cur[i].id == g.prev[k].id)
                    still = true;
            if (still)
                continue;
            hid_touch_state *o = &st.touches[out++];
            o->delta_time = g.now_ns - g.prev[k].start_ns;
            o->attributes = HID_TOUCH_END;
            o->finger_id = g.prev[k].id;
            o->x = (uint32_t)((uint64_t)g.prev[k].x * HID_TOUCH_WIDTH / 65536u);
            o->y = (uint32_t)((uint64_t)g.prev[k].y * HID_TOUCH_HEIGHT / 65536u);
            o->diameter_x = 15;
            o->diameter_y = 15;
        }
        st.count = (int32_t)out;
    }
    memcpy(g.prev, cur, ncur * sizeof cur[0]);
    g.prev_count = ncur;
    PUSH(&g.mem->touch_screen, st, 0);
}

static uint32_t map_pointer_buttons(uint32_t b)
{
    uint32_t r = 0;
    if (b & (1u << RL_PTR_LEFT))
        r |= HID_MOUSE_LEFT;
    if (b & (1u << RL_PTR_RIGHT))
        r |= HID_MOUSE_RIGHT;
    if (b & (1u << RL_PTR_MIDDLE))
        r |= HID_MOUSE_MIDDLE;
    if (b & (1u << RL_PTR_FORWARD))
        r |= HID_MOUSE_FORWARD;
    if (b & (1u << RL_PTR_BACK))
        r |= HID_MOUSE_BACK;
    return r;
}

static uint64_t map_modifiers(uint32_t m)
{
    uint64_t r = 0;
    if (m & 1)
        r |= 1u << 0;
    if (m & 2)
        r |= 1u << 1;
    if (m & 4)
        r |= 1u << 2;
    if (m & 8)
        r |= 1u << 4;
    if (m & 16)
        r |= 1u << 8;
    if (m & 32)
        r |= 1u << 10;
    if (m & 64)
        r |= 1u << 9;
    return r;
}

static void update_all(const rl_input_snapshot *s)
{
    hid_debug_pad_state dp;
    memset(&dp, 0, sizeof dp);
    PUSH(&g.mem->debug_pad, dp, 0);

    update_touch(s);

    hid_mouse_state ms;
    memset(&ms, 0, sizeof ms);
    if (g.active[HID_DEV_MOUSE]) {
        ms.x = s->pointer.x;
        ms.y = s->pointer.y;
        ms.delta_x = s->pointer.dx;
        ms.delta_y = s->pointer.dy;
        ms.wheel_delta_y = s->pointer.wheel;
        ms.buttons = map_pointer_buttons(s->pointer.buttons);
        ms.attributes = HID_MOUSE_CONNECTED;
    }
    PUSH(&g.mem->mouse, ms, 0);

    hid_mouse_state dm;
    memset(&dm, 0, sizeof dm);
    PUSH(&g.mem->debug_mouse, dm, 0);

    if (g.active[HID_DEV_KEYBOARD]) {
        hid_keyboard_state ks;
        memset(&ks, 0, sizeof ks);
        ks.modifiers = map_modifiers(s->modifiers);
        memcpy(ks.keys, s->keys, sizeof ks.keys);
        PUSH(&g.mem->keyboard, ks, 0);
    } else {
        lifo_clear(&g.mem->keyboard.header);
    }
}

static bool mem_enter(void)
{
    if (!g.shm)
        return false;
    g.mem = (hid_shared_memory *)(void *)rl_hos_shmem_lock(g.shm);
    return true;
}

static void mem_leave(void)
{
    if (g.shm)
        rl_hos_shmem_unlock(g.shm);
}

static void signal_events(uint32_t mask)
{
    for (uint32_t i = 0; i < NPAD_SLOTS; i++)
        if ((mask & (1u << i)) && g.events[i])
            rl_hos_event_signal(g.events[i]);
}

static uint32_t update_locked(const rl_input_snapshot *s)
{
    uint64_t ns = rl_clock_ns();
    g.dt_ns = g.last_ns ? ns - g.last_ns : UPDATE_NS;
    g.last_ns = ns;
    g.now_ns = ns;
    g.ts = rl_ns_to_ticks(ns);
    update_all(s);
    return update_npads(s);
}

void rl_hid_update_now(void)
{
    rl_input_snapshot s;
    rl_input_sample(&s);
    pthread_mutex_lock(&g_m);
    if (!mem_enter()) {
        pthread_mutex_unlock(&g_m);
        return;
    }
    uint32_t changed = update_locked(&s);
    mem_leave();
    pthread_mutex_unlock(&g_m);
    signal_events(changed);
}

static void *update_thread(void *arg)
{
    (void)arg;
    uint64_t next = rl_clock_ns();
    for (;;) {
        next += UPDATE_NS;
        uint64_t now = rl_clock_ns();
        if (now < next)
            rl_sleep_ns(next - now);
        else
            next = now;
        if (__atomic_load_n(&g.stop, __ATOMIC_ACQUIRE))
            break;
        if (__atomic_load_n(&g.shm->obj.refs, __ATOMIC_ACQUIRE) <= 1)
            break;
        rl_hid_update_now();
    }
    __atomic_store_n(&g.thread_done, true, __ATOMIC_RELEASE);
    return NULL;
}

static void join_thread_locked_tm(void)
{
    if (g.thread_up) {
        pthread_join(g.thread, NULL);
        g.thread_up = false;
    }
}

hos_shmem *hid_core_open(void)
{
    pthread_mutex_lock(&g_tm);
    pthread_mutex_lock(&g_m);
    if (!g.shm) {
        hos_shmem *s = rl_hos_shmem_create(HID_SHMEM_SIZE, HOS_PERM_R);
        if (!s) {
            pthread_mutex_unlock(&g_m);
            pthread_mutex_unlock(&g_tm);
            return NULL;
        }
        g.shm = s;
        mem_enter();
        shm_init(g.mem);
        mem_leave();
    }
    hos_kobj_ref(&g.shm->obj);
    hos_shmem *ret = g.shm;
    pthread_mutex_unlock(&g_m);
    if (g.thread_up && __atomic_load_n(&g.thread_done, __ATOMIC_ACQUIRE))
        join_thread_locked_tm();
    if (!g.thread_up && !g.manual) {
        __atomic_store_n(&g.stop, false, __ATOMIC_RELEASE);
        __atomic_store_n(&g.thread_done, false, __ATOMIC_RELEASE);
        if (pthread_create(&g.thread, NULL, update_thread, NULL) == 0)
            g.thread_up = true;
    }
    pthread_mutex_unlock(&g_tm);
    return ret;
}

void rl_hid_stop(void)
{
    pthread_mutex_lock(&g_tm);
    __atomic_store_n(&g.stop, true, __ATOMIC_RELEASE);
    join_thread_locked_tm();
    pthread_mutex_lock(&g_m);
    hos_shmem *shm = g.shm;
    hos_revent *ev[NPAD_SLOTS];
    memcpy(ev, g.events, sizeof ev);
    bool manual = g.manual;
    memset(&g, 0, sizeof g);
    g.manual = manual;
    reset_config();
    pthread_mutex_unlock(&g_m);
    pthread_mutex_unlock(&g_tm);
    for (uint32_t i = 0; i < NPAD_SLOTS; i++)
        if (ev[i])
            hos_kobj_unref(&ev[i]->obj);
    if (shm)
        hos_kobj_unref(&shm->obj);
}

void hid_core_reset(void)
{
    rl_hid_stop();
    pthread_mutex_lock(&g_m);
    g.manual = false;
    pthread_mutex_unlock(&g_m);
}

bool rl_hid_is_active(void)
{
    pthread_mutex_lock(&g_m);
    bool a = g.shm != NULL && (g.manual || (g.thread_up && !__atomic_load_n(&g.thread_done, __ATOMIC_ACQUIRE)));
    pthread_mutex_unlock(&g_m);
    return a;
}

void rl_hid_set_manual(bool manual)
{
    pthread_mutex_lock(&g_m);
    g.manual = manual;
    pthread_mutex_unlock(&g_m);
}

void hid_core_activate(enum hid_device d)
{
    rl_input_snapshot s;
    rl_input_sample(&s);
    pthread_mutex_lock(&g_m);
    g.active[d] = true;
    uint32_t changed = 0;
    if (mem_enter()) {
        for (uint32_t i = 0; i < HID_LIFO_ENTRIES; i++) {
            switch (d) {
            case HID_DEV_DEBUG_PAD: {
                hid_debug_pad_state dp;
                memset(&dp, 0, sizeof dp);
                PUSH(&g.mem->debug_pad, dp, 0);
                break;
            }
            case HID_DEV_TOUCH: {
                rl_input_snapshot idle = s;
                idle.touch_count = 0;
                update_touch(&idle);
                break;
            }
            case HID_DEV_MOUSE:
            case HID_DEV_DEBUG_MOUSE: {
                hid_mouse_state ms;
                memset(&ms, 0, sizeof ms);
                hid_mouse_lifo *l = d == HID_DEV_MOUSE ? &g.mem->mouse : &g.mem->debug_mouse;
                PUSH(l, ms, 0);
                break;
            }
            case HID_DEV_KEYBOARD: {
                hid_keyboard_state ks;
                memset(&ks, 0, sizeof ks);
                PUSH(&g.mem->keyboard, ks, 0);
                break;
            }
            case HID_DEV_NPAD:
                changed |= update_npads(&s);
                break;
            }
        }
        mem_leave();
    }
    pthread_mutex_unlock(&g_m);
    signal_events(changed);
}

void hid_core_set_styles(uint32_t styles)
{
    pthread_mutex_lock(&g_m);
    g.styles = styles;
    pthread_mutex_unlock(&g_m);
}

uint32_t hid_core_styles(void)
{
    pthread_mutex_lock(&g_m);
    uint32_t v = g.styles;
    pthread_mutex_unlock(&g_m);
    return v;
}

void hid_core_set_players(const uint32_t *ids, uint32_t n)
{
    pthread_mutex_lock(&g_m);
    memset(g.players, 0, sizeof g.players);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot;
        if (hid_slot_from_id(ids[i], &slot) && slot < NPAD_SLOTS)
            g.players[slot] = true;
    }
    pthread_mutex_unlock(&g_m);
}

bool hid_core_player_supported(uint32_t slot)
{
    pthread_mutex_lock(&g_m);
    bool v = slot < NPAD_SLOTS && g.players[slot];
    pthread_mutex_unlock(&g_m);
    return v;
}

void hid_core_set_joy_hold(uint64_t hold)
{
    pthread_mutex_lock(&g_m);
    g.joy_hold = hold;
    pthread_mutex_unlock(&g_m);
}

uint64_t hid_core_joy_hold(void)
{
    pthread_mutex_lock(&g_m);
    uint64_t v = g.joy_hold;
    pthread_mutex_unlock(&g_m);
    return v;
}

void hid_core_set_assignment(uint32_t slot, uint32_t mode)
{
    pthread_mutex_lock(&g_m);
    if (slot < HID_NPADS && mem_enter()) {
        g.mem->npads[slot].state.joy_assignment_mode = mode;
        mem_leave();
    }
    pthread_mutex_unlock(&g_m);
}

hos_revent *hid_core_style_event(uint32_t slot)
{
    if (slot >= NPAD_SLOTS)
        return NULL;
    pthread_mutex_lock(&g_m);
    if (!g.events[slot])
        g.events[slot] = rl_hos_event_create();
    hos_revent *e = g.events[slot];
    pthread_mutex_unlock(&g_m);
    return e;
}

uint8_t hid_core_footer_type(uint32_t slot)
{
    pthread_mutex_lock(&g_m);
    uint8_t v = 0;
    if (slot < HID_NPADS && mem_enter()) {
        v = g.mem->npads[slot].state.applet_footer_ui_type;
        mem_leave();
    }
    pthread_mutex_unlock(&g_m);
    return v;
}

void hid_core_set_vibration(uint32_t slot, uint32_t position, const float v[4])
{
    if (slot >= NPAD_SLOTS || position > 1)
        return;
    pthread_mutex_lock(&g_m);
    memcpy(g.vibration[slot][position], v, sizeof g.vibration[0][0]);
    pthread_mutex_unlock(&g_m);
}

void hid_core_get_vibration(uint32_t slot, uint32_t position, float v[4])
{
    static const float neutral[4] = { 0.0f, 160.0f, 0.0f, 320.0f };
    pthread_mutex_lock(&g_m);
    bool set = slot < NPAD_SLOTS && position <= 1 && g.vibration[slot][position][1] != 0.0f;
    memcpy(v, set ? g.vibration[slot][position] : neutral, sizeof neutral);
    pthread_mutex_unlock(&g_m);
}
