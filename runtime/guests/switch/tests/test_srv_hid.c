/**
 * @file test_srv_hid.c
 * @brief The hid, hid:sys and irs services through IPC the way libnx uses
 * them: hidInitialize, the shared memory read back from guest memory with
 * the libnx ring reader, the npad, touch, keyboard and mouse updates from
 * published host input, and the control commands.
 *
 * The ring reader follows libnx (ISC, libnx Authors) services/hid.c.
 */
#include "t.h"
#include "tc.h"

#include "hid.h"

#include <relift/ports.h>

#include <unistd.h>

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
    uint64_t at;
    tc_obj hid;
    tc_obj ar;
    uint32_t shm;
    const hid_shared_memory *mem;
} env;

static tc_req hreq(uint32_t cmd)
{
    tc_req q = tc_new(cmd);
    q.send_pid = true;
    return q;
}

static tc_req req_aruid(uint32_t cmd)
{
    tc_req q = hreq(cmd);
    tc_put64(&q, 0x1234);
    return q;
}

static tc_res call_u32(env *e, uint32_t cmd, uint32_t v)
{
    tc_req q = hreq(cmd);
    tc_put32(&q, v);
    tc_put32(&q, 0);
    tc_put64(&q, 0x1234);
    return tc_send(e->c, e->hid, q);
}

static tc_res call_aruid_u64(env *e, uint32_t cmd, uint64_t v)
{
    tc_req q = hreq(cmd);
    tc_put64(&q, 0x1234);
    tc_put64(&q, v);
    return tc_send(e->c, e->hid, q);
}

static uint32_t rc_of(tc_res r) { return r.svc_rc ? r.svc_rc : r.result; }

static uint32_t cmd_aruid(env *e, uint32_t cmd)
{
    tc_req q = hreq(cmd);
    tc_put64(&q, 0x1234);
    return rc_of(tc_send(e->c, e->hid, q));
}

static bool start_at(env *e, bool manual, uint64_t shift)
{
    memset(e, 0, sizeof *e);
    e->c = t_boot(NULL);
    if (manual)
        rl_hid_set_manual(true);
    e->heap = t_heap(e->c, 0x200000);
    if (!e->heap)
        return false;
    e->hid = tc_open_service(e->c, e->heap, "hid");
    if (!e->hid.handle)
        return false;
    tc_req q = hreq(0);
    tc_put64(&q, 0x1234);
    if (tc_sub(e->c, e->hid, q, &e->ar) != 0)
        return false;
    tc_res r = tc_send(e->c, e->ar, tc_new(0));
    if (rc_of(r) != 0 || r.ncopy != 1)
        return false;
    e->shm = r.copy[0];
    e->at = t_free_map_addr() + shift;
    if (t_svc(e->c, SVC_MAP_SHARED_MEMORY, e->shm, e->at, HID_SHMEM_SIZE, HOS_PERM_R, 0, 0) != 0)
        return false;
    e->mem = (const hid_shared_memory *)(const void *)(t_mem() + e->at);
    return true;
}

static bool start_with(env *e, bool manual) { return start_at(e, manual, 0); }

static bool start(env *e) { return start_with(e, true); }

static void publish(const rl_input_snapshot *s) { rl_input_publish(s); }

static rl_input_snapshot blank(void)
{
    rl_input_snapshot s;
    memset(&s, 0, sizeof s);
    s.pads[0].connected = true;
    return s;
}

static size_t get_states(const hid_lifo_header *h, const void *in_states, size_t max_states, size_t state_offset,
                         size_t sampling_number_offset, void *states, size_t entrysize, size_t count)
{
    int64_t total_entries = (int64_t)__atomic_load_n(&h->count, __ATOMIC_ACQUIRE);
    if (total_entries < 0)
        total_entries = 0;
    if (total_entries > (int64_t)count)
        total_entries = (int64_t)count;
    int64_t tail = (int64_t)__atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
    for (int64_t i = 0; i < total_entries; i++) {
        int64_t entrypos = (((tail + (int64_t)(max_states + 1)) - total_entries) + i) % (int64_t)max_states;
        const uint8_t *state_entry = (const uint8_t *)in_states + (size_t)entrypos * (state_offset + entrysize);
        uint8_t *out_state = (uint8_t *)states + (size_t)(total_entries - i - 1) * entrysize;
        const uint8_t *out_state_prev = (const uint8_t *)states + (size_t)(total_entries - i) * entrysize;
        uint64_t s0, s1;
        memcpy(&s0, state_entry, 8);
        memcpy(out_state, state_entry + state_offset, entrysize);
        memcpy(&s1, state_entry, 8);
        uint64_t cur, prev = 0;
        memcpy(&cur, out_state + sampling_number_offset, 8);
        if (i > 0)
            memcpy(&prev, out_state_prev + sampling_number_offset, 8);
        if (s0 != s1 || (s0 & 1) || (i > 0 && cur - prev != 1))
            return 0;
    }
    return (size_t)total_entries;
}

#define GET_STATES(lifo, out, count)                                                                          \
    get_states(&(lifo).header, (lifo).entries, HID_LIFO_ENTRIES, 8, offsetof(__typeof__((out)[0]), sampling_number), \
               (out), sizeof((out)[0]), (count))

static size_t get_six(const hid_six_axis_lifo *l, hid_six_axis_state *out, size_t count)
{
    return get_states(&l->header, l->entries, HID_LIFO_ENTRIES, 8, 8, out, sizeof out[0], count);
}

static uint32_t npad_style(const env *e, uint32_t slot)
{
    return __atomic_load_n(&e->mem->npads[slot].state.style_set, __ATOMIC_ACQUIRE);
}

static void set_styles(env *e, uint32_t styles)
{
    tc_res r = call_u32(e, 100, styles);
    CHECK_EQ(rc_of(r), 0);
}

TEST(hid_services_registered)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(tc_open_service_result(c, heap, "hid"), 0);
    CHECK_EQ(tc_open_service_result(c, heap, "hid:sys"), 0);
    CHECK_EQ(tc_open_service_result(c, heap, "hid:dbg"), 0);
    CHECK_EQ(tc_open_service_result(c, heap, "irs"), 0);
    CHECK_EQ(tc_open_service_result(c, heap, "irs:sys"), 0);
    CHECK(tc_open_service_result(c, heap, "hid:xx") != 0);
}

TEST(hid_libnx_initialize_sequence)
{
    env e;
    CHECK(start(&e));
    const hid_shared_memory *m = e.mem;
    CHECK_EQ(m->debug_pad.header.buffer_count, 17);
    CHECK_EQ(m->touch_screen.header.buffer_count, 17);
    CHECK_EQ(m->mouse.header.buffer_count, 17);
    CHECK_EQ(m->keyboard.header.buffer_count, 17);
    CHECK_EQ(m->debug_mouse.header.buffer_count, 17);
    for (uint32_t i = 0; i < HID_NPADS; i++) {
        const hid_npad_internal *n = &m->npads[i].state;
        CHECK_EQ(n->full_key.header.buffer_count, 17);
        CHECK_EQ(n->system_ext.header.buffer_count, 17);
        CHECK_EQ(n->handheld_six.header.buffer_count, 17);
        CHECK_EQ(n->gc_trigger.header.buffer_count, 17);
    }
    CHECK_EQ(m->condition.b, 1);
    CHECK_EQ(m->condition.hold_type, 1);
    CHECK_EQ((const uint8_t *)&m->npads[1] - (const uint8_t *)m, 0x9A00 + 0x5000);
    CHECK(t_write(e.at, 1).faulted);
    CHECK(!t_read(e.at + HID_SHMEM_SIZE - 1).faulted);
    CHECK(t_read(e.at + HID_SHMEM_SIZE).faulted);
    CHECK(rl_hid_is_active());
    rl_hid_stop();
    CHECK(!rl_hid_is_active());
}

TEST(hid_applet_resource_handle_twice)
{
    env e;
    CHECK(start(&e));
    tc_res r = tc_send(e.c, e.ar, tc_new(0));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    uint64_t at2 = e.at + 0x400000;
    CHECK_EQ(t_svc(e.c, SVC_MAP_SHARED_MEMORY, r.copy[0], at2, HID_SHMEM_SIZE, HOS_PERM_R, 0, 0), 0);
    e.mem = (const hid_shared_memory *)(const void *)(t_mem() + at2);
    CHECK_EQ(rc_of(tc_send(e.c, e.ar, tc_new(1))), SF_RESULT_UNKNOWN_COMMAND);
    rl_hid_stop();
}

TEST(hid_shared_memory_inside_a_host_page)
{
    env e;
    CHECK(start_at(&e, true, 0x1000));
    CHECK_EQ(e.mem->debug_pad.header.buffer_count, 17);
    CHECK_EQ(e.mem->npads[0].state.full_key.header.buffer_count, 17);
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    rl_input_snapshot s = blank();
    s.pads[0].buttons = 1u << RL_BTN_EAST;
    publish(&s);
    rl_hid_update_now();
    hid_npad_state st[17];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 17), 16);
    CHECK_EQ(st[0].buttons, HID_BTN_A);
    CHECK(!t_read(e.at + HID_SHMEM_SIZE - 1).faulted);
    CHECK_EQ(t_svc(e.c, SVC_MAP_SHARED_MEMORY, e.shm, e.at + 0x400000 - 0x1000, HID_SHMEM_SIZE, HOS_PERM_R, 0, 0),
             hos.vm.host_page > 0x1000 ? KR_INVALID_MEM_STATE : 0u);
    rl_hid_stop();
}

TEST(hid_update_without_resource)
{
    rl_ctx *c = t_boot(NULL);
    (void)c;
    rl_hid_update_now();
    CHECK(!rl_hid_is_active());
    rl_hid_stop();
}

TEST(hid_npad_buttons_and_sticks)
{
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    CHECK_EQ(npad_style(&e, 0), HID_STYLE_FULL_KEY);
    rl_input_snapshot s = blank();
    s.pads[0].buttons = (1u << RL_BTN_SOUTH) | (1u << RL_BTN_EAST) | (1u << RL_BTN_START) | (1u << RL_BTN_DPAD_UP) |
                        (1u << RL_BTN_LEFT_SHOULDER);
    s.pads[0].lx = 30000;
    s.pads[0].ly = -32768;
    s.pads[0].rx = -1000;
    s.pads[0].ry = 500;
    publish(&s);
    rl_hid_update_now();
    hid_npad_state st[17];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 17), 16);
    uint64_t want = HID_BTN_B | HID_BTN_A | HID_BTN_PLUS | HID_BTN_UP | HID_BTN_L | HID_BTN_STICK_L_RIGHT | HID_BTN_STICK_L_DOWN;
    CHECK_EQ(st[0].buttons, want);
    CHECK_EQ(st[0].l.x, 30000);
    CHECK_EQ(st[0].l.y, -32767);
    CHECK_EQ(st[0].r.x, -1000);
    CHECK_EQ(st[0].r.y, 500);
    CHECK_EQ(st[0].attributes, HID_ATTR_CONNECTED | HID_ATTR_WIRED);
    CHECK_EQ(st[1].buttons, 0);
    hid_npad_state ext[17];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.system_ext, ext, 17), 16);
    CHECK_EQ(ext[0].buttons, want);
    hid_npad_state hh[17];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.handheld, hh, 17), 16);
    CHECK_EQ(hh[0].buttons, 0);
    CHECK_EQ(hh[0].attributes, 0);
    const hid_npad_internal *n = &e.mem->npads[0].state;
    CHECK_EQ(n->device_type, 1);
    CHECK_EQ(n->applet_footer_ui_type, 12);
    CHECK_EQ(n->full_key_color.body, 0x828282);
    CHECK_EQ(n->battery_level[0], 4);
    CHECK(n->system_properties & (1u << 11));
    CHECK(n->full_key.header.timestamp != 0);
    hid_six_axis_state six[17];
    CHECK_EQ(get_six(&n->full_key_six, six, 17), 16);
    CHECK_EQ(six[0].attributes, 1);
    CHECK_EQ(six[0].direction[0] == 1.0f && six[0].direction[4] == 1.0f && six[0].direction[8] == 1.0f, 1);
    rl_hid_stop();
}

TEST(hid_npad_button_map)
{
    static const struct {
        uint32_t src;
        uint64_t dst;
    } map[] = {
        { RL_BTN_SOUTH, HID_BTN_B },
        { RL_BTN_EAST, HID_BTN_A },
        { RL_BTN_WEST, HID_BTN_Y },
        { RL_BTN_NORTH, HID_BTN_X },
        { RL_BTN_LEFT_STICK, HID_BTN_STICK_L },
        { RL_BTN_RIGHT_STICK, HID_BTN_STICK_R },
        { RL_BTN_LEFT_SHOULDER, HID_BTN_L },
        { RL_BTN_RIGHT_SHOULDER, HID_BTN_R },
        { RL_BTN_LEFT_TRIGGER, HID_BTN_ZL },
        { RL_BTN_RIGHT_TRIGGER, HID_BTN_ZR },
        { RL_BTN_START, HID_BTN_PLUS },
        { RL_BTN_BACK, HID_BTN_MINUS },
        { RL_BTN_DPAD_UP, HID_BTN_UP },
        { RL_BTN_DPAD_RIGHT, HID_BTN_RIGHT },
        { RL_BTN_DPAD_DOWN, HID_BTN_DOWN },
        { RL_BTN_DPAD_LEFT, HID_BTN_LEFT },
        { RL_BTN_GUIDE, 0 },
        { RL_BTN_MISC, 0 },
    };
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        rl_input_snapshot s = blank();
        s.pads[0].buttons = 1u << map[i].src;
        publish(&s);
        rl_hid_update_now();
        hid_npad_state st[17];
        CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 1), 1);
        CHECK_EQ(st[0].buttons, map[i].dst);
    }
    rl_input_snapshot s = blank();
    s.pads[0].lx = -20000;
    s.pads[0].ly = 20000;
    s.pads[0].rx = 20000;
    s.pads[0].ry = -20000;
    publish(&s);
    rl_hid_update_now();
    hid_npad_state st[1];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 1), 1);
    CHECK_EQ(st[0].buttons, HID_BTN_STICK_L_LEFT | HID_BTN_STICK_L_UP | HID_BTN_STICK_R_RIGHT | HID_BTN_STICK_R_DOWN);
    rl_hid_stop();
}

TEST(hid_ring_advances_and_wraps)
{
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    const hid_lifo_header *h = &e.mem->npads[0].state.full_key.header;
    uint64_t start_n = h->count == 0 ? 0 : e.mem->npads[0].state.full_key.entries[h->tail].state.sampling_number;
    CHECK_EQ(h->count, 16);
    uint64_t last = 0;
    for (int i = 1; i <= 40; i++) {
        rl_input_snapshot s = blank();
        s.pads[0].lx = (int16_t)i;
        publish(&s);
        rl_hid_update_now();
        last = e.mem->npads[0].state.full_key.entries[h->tail].state.sampling_number;
        CHECK_EQ(last, start_n + (uint64_t)i);
        CHECK_EQ(e.mem->npads[0].state.full_key.entries[h->tail].seq, 2 * last);
    }
    for (uint32_t i = 0; i < HID_LIFO_ENTRIES; i++) {
        const hid_npad_lifo_entry *en = &e.mem->npads[0].state.full_key.entries[i];
        CHECK_EQ(en->seq, 2 * en->state.sampling_number);
    }
    CHECK_EQ(h->count, 16);
    hid_npad_state st[17];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 17), 16);
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(st[i].sampling_number, last - (uint64_t)i);
        CHECK_EQ(st[i].l.x, 40 - i);
    }
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 3), 3);
    uint64_t t0 = h->timestamp;
    rl_hid_update_now();
    CHECK(h->timestamp >= t0);
    rl_hid_stop();
}

TEST(hid_disconnected_pad)
{
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    CHECK_EQ(npad_style(&e, 1), 0);
    CHECK_EQ(npad_style(&e, 7), 0);
    CHECK_EQ(npad_style(&e, 9), 0);
    hid_npad_state st[17];
    const hid_npad_internal *n = &e.mem->npads[1].state;
    CHECK_EQ(GET_STATES(n->full_key, st, 17), 16);
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(st[i].buttons, 0);
        CHECK_EQ(st[i].attributes, 0);
        CHECK_EQ(st[i].l.x, 0);
    }
    uint64_t before = n->full_key.entries[n->full_key.header.tail].state.sampling_number;
    rl_hid_update_now();
    CHECK_EQ(n->full_key.entries[n->full_key.header.tail].state.sampling_number, before + 1);
    CHECK_EQ(n->device_type, 0);
    CHECK_EQ(n->system_properties, 0);
    rl_hid_stop();
}

TEST(hid_connect_disconnect_event)
{
    env e;
    CHECK(start(&e));
    tc_req q = hreq(106);
    tc_put32(&q, 1);
    tc_put32(&q, 0);
    tc_put64(&q, 0x1234);
    tc_put64(&q, 0);
    tc_res r = tc_send(e.c, e.hid, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    uint32_t ev = r.copy[0];
    CHECK_EQ(t_svc(e.c, SVC_RESET_SIGNAL, ev, 0, 0, 0, 0, 0), 0);
    CHECK(t_svc(e.c, SVC_RESET_SIGNAL, ev, 0, 0, 0, 0, 0) != 0);
    rl_input_snapshot s = blank();
    s.pads[1].connected = true;
    s.pads[1].buttons = 1u << RL_BTN_NORTH;
    publish(&s);
    rl_hid_update_now();
    CHECK_EQ(npad_style(&e, 1), HID_STYLE_FULL_KEY);
    CHECK_EQ(t_svc(e.c, SVC_RESET_SIGNAL, ev, 0, 0, 0, 0, 0), 0);
    hid_npad_state st[1];
    CHECK_EQ(GET_STATES(e.mem->npads[1].state.full_key, st, 1), 1);
    CHECK_EQ(st[0].buttons, HID_BTN_X);
    rl_hid_update_now();
    CHECK(t_svc(e.c, SVC_RESET_SIGNAL, ev, 0, 0, 0, 0, 0) != 0);
    s.pads[1].connected = false;
    publish(&s);
    rl_hid_update_now();
    CHECK_EQ(npad_style(&e, 1), 0);
    CHECK_EQ(t_svc(e.c, SVC_RESET_SIGNAL, ev, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(GET_STATES(e.mem->npads[1].state.full_key, st, 1), 1);
    CHECK_EQ(st[0].attributes, 0);
    tc_req bad = hreq(106);
    tc_put32(&bad, 77);
    tc_put32(&bad, 0);
    tc_put64(&bad, 0x1234);
    tc_put64(&bad, 0);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, bad)), RL_MAKE_RESULT(202, 123));
    rl_hid_stop();
}

TEST(hid_style_set_remaps_pad)
{
    env e;
    CHECK(start(&e));
    tc_res r = tc_send(e.c, e.hid, req_aruid(101));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), HID_STYLE_FULL_KEY | HID_STYLE_HANDHELD | HID_STYLE_JOY_DUAL | HID_STYLE_JOY_LEFT | HID_STYLE_JOY_RIGHT);
    set_styles(&e, HID_STYLE_JOY_DUAL | HID_STYLE_HANDHELD);
    r = tc_send(e.c, e.hid, req_aruid(101));
    CHECK_EQ(tc_u32(&r, 0), HID_STYLE_JOY_DUAL | HID_STYLE_HANDHELD);
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    CHECK_EQ(npad_style(&e, 0), HID_STYLE_JOY_DUAL);
    CHECK_EQ(npad_style(&e, 8), 0);
    rl_input_snapshot s = blank();
    s.pads[0].buttons = 1u << RL_BTN_BACK;
    publish(&s);
    rl_hid_update_now();
    hid_npad_state st[1];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.joy_dual, st, 1), 1);
    CHECK_EQ(st[0].buttons, HID_BTN_MINUS);
    CHECK_EQ(st[0].attributes, HID_ATTR_CONNECTED | HID_ATTR_LEFT_CONNECTED | HID_ATTR_RIGHT_CONNECTED);
    CHECK_EQ(e.mem->npads[0].state.device_type, (1u << 4) | (1u << 5));
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 1), 1);
    CHECK_EQ(st[0].buttons, 0);

    set_styles(&e, HID_STYLE_HANDHELD);
    rl_hid_update_now();
    CHECK_EQ(npad_style(&e, 0), 0);
    CHECK_EQ(npad_style(&e, 8), HID_STYLE_HANDHELD);
    CHECK_EQ(e.mem->npads[8].state.device_type, (1u << 2) | (1u << 3));
    CHECK_EQ(GET_STATES(e.mem->npads[8].state.handheld, st, 1), 1);
    CHECK_EQ(st[0].buttons, HID_BTN_MINUS);
    CHECK_EQ(st[0].attributes, HID_ATTR_CONNECTED | HID_ATTR_WIRED);

    set_styles(&e, HID_STYLE_JOY_LEFT);
    rl_hid_update_now();
    CHECK_EQ(npad_style(&e, 0), 0);
    CHECK_EQ(npad_style(&e, 8), 0);
    rl_hid_stop();
}

TEST(hid_supported_npad_ids)
{
    env e;
    CHECK(start(&e));
    rl_input_snapshot s = blank();
    s.pads[1].connected = true;
    publish(&s);
    uint32_t ids[2] = { 1, 5 };
    memcpy(t_mem() + e.heap + 0x1000, ids, sizeof ids);
    tc_req q = hreq(102);
    tc_put64(&q, 0x1234);
    q.na = 1;
    q.a_addr[0] = e.heap + 0x1000;
    q.a_size[0] = sizeof ids;
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), 0);
    rl_hid_update_now();
    CHECK_EQ(npad_style(&e, 0), 0);
    CHECK_EQ(npad_style(&e, 1), HID_STYLE_FULL_KEY);
    rl_hid_stop();
}

TEST(hid_touch_screen)
{
    env e;
    CHECK(start(&e));
    rl_input_snapshot s = blank();
    s.touch_count = 1;
    s.touches[0].id = 7;
    s.touches[0].x = 32768;
    s.touches[0].y = 16384;
    publish(&s);
    rl_hid_update_now();
    const hid_touch_lifo *l = &e.mem->touch_screen;
    hid_touch_screen_state st[17];
    CHECK_EQ(GET_STATES(*l, st, 17), 1);
    CHECK_EQ(st[0].count, 0);
    CHECK_EQ(cmd_aruid(&e, 11), 0);
    CHECK_EQ(GET_STATES(*l, st, 17), 16);
    CHECK_EQ(st[0].count, 0);
    CHECK_EQ(st[15].count, 0);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 17), 16);
    CHECK_EQ(st[0].count, 1);
    CHECK_EQ(st[0].touches[0].attributes, HID_TOUCH_START);
    CHECK_EQ(st[0].touches[0].finger_id, 7);
    CHECK_EQ(st[0].touches[0].x, 640);
    CHECK_EQ(st[0].touches[0].y, 180);
    CHECK_EQ(st[0].touches[0].diameter_x, 15);
    CHECK_EQ(st[1].count, 0);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].count, 1);
    CHECK_EQ(st[0].touches[0].attributes, 0);
    s.touch_count = 2;
    s.touches[1].id = 9;
    s.touches[1].x = 65535;
    s.touches[1].y = 65535;
    publish(&s);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].count, 2);
    CHECK_EQ(st[0].touches[0].attributes, 0);
    CHECK_EQ(st[0].touches[1].attributes, HID_TOUCH_START);
    CHECK_EQ(st[0].touches[1].x, 1279);
    CHECK_EQ(st[0].touches[1].y, 719);
    s.touch_count = 0;
    publish(&s);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].count, 2);
    CHECK_EQ(st[0].touches[0].attributes, HID_TOUCH_END);
    CHECK_EQ(st[0].touches[1].attributes, HID_TOUCH_END);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].count, 0);
    rl_hid_stop();
}

TEST(hid_keyboard)
{
    env e;
    CHECK(start(&e));
    rl_input_snapshot s = blank();
    s.keys[0] = 1ull << 4;
    s.keys[3] = 1ull << (0xE1 - 192);
    s.modifiers = 1 | 2 | 16;
    publish(&s);
    rl_hid_update_now();
    const hid_keyboard_lifo *l = &e.mem->keyboard;
    CHECK_EQ(l->header.count, 0);
    CHECK_EQ(cmd_aruid(&e, 31), 0);
    hid_keyboard_state st[17];
    CHECK_EQ(GET_STATES(*l, st, 17), 16);
    CHECK_EQ(st[0].keys[0], 0);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].keys[0], 1ull << 4);
    CHECK_EQ(st[0].keys[3], 1ull << (0xE1 - 192));
    CHECK_EQ(st[0].modifiers, 1 | 2 | (1u << 8));
    s.modifiers = 4 | 8 | 32 | 64;
    s.keys[0] = 0;
    publish(&s);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].keys[0], 0);
    CHECK_EQ(st[0].modifiers, (1u << 2) | (1u << 4) | (1u << 10) | (1u << 9));
    rl_hid_stop();
}

TEST(hid_mouse)
{
    env e;
    CHECK(start(&e));
    rl_input_snapshot s = blank();
    s.pointer.x = 100;
    s.pointer.y = 50;
    s.pointer.dx = 3;
    s.pointer.dy = -2;
    s.pointer.wheel = 1;
    s.pointer.buttons = (1u << RL_PTR_LEFT) | (1u << RL_PTR_FORWARD) | (1u << RL_PTR_BACK) | (1u << RL_PTR_MIDDLE);
    publish(&s);
    rl_hid_update_now();
    const hid_mouse_lifo *l = &e.mem->mouse;
    hid_mouse_state st[17];
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].buttons, 0);
    CHECK_EQ(st[0].attributes, 0);
    CHECK_EQ(cmd_aruid(&e, 21), 0);
    rl_hid_update_now();
    CHECK_EQ(GET_STATES(*l, st, 1), 1);
    CHECK_EQ(st[0].x, 100);
    CHECK_EQ(st[0].y, 50);
    CHECK_EQ(st[0].delta_x, 3);
    CHECK_EQ(st[0].delta_y, -2);
    CHECK_EQ(st[0].wheel_delta_y, 1);
    CHECK_EQ(st[0].buttons, HID_MOUSE_LEFT | HID_MOUSE_FORWARD | HID_MOUSE_BACK | HID_MOUSE_MIDDLE);
    CHECK_EQ(st[0].attributes, HID_MOUSE_CONNECTED);
    CHECK_EQ(cmd_aruid(&e, 26), 0);
    CHECK_EQ(cmd_aruid(&e, 3150), 0);
    CHECK_EQ(cmd_aruid(&e, 1), 0);
    CHECK_EQ(e.mem->debug_mouse.header.count, 16);
    CHECK_EQ(e.mem->debug_pad.header.count, 16);
    rl_hid_stop();
}

static uint32_t vib_handle(uint8_t type, uint8_t player, uint8_t pos)
{
    return (uint32_t)type | ((uint32_t)player << 8) | ((uint32_t)pos << 16);
}

TEST(hid_has_a_pointer_buffer_for_its_x_buffers)
{
    env e;
    CHECK(start(&e));
    tc_req q = tc_new(3);
    q.type = HIPC_CONTROL;
    tc_res r = tc_call(e.c, e.hid.handle, &q, false);
    CHECK_EQ(r.result, 0);
    uint16_t pbs;
    memcpy(&pbs, r.data, 2);
    CHECK(pbs >= 10 * 4);
    rl_hid_stop();
}

TEST(hid_control_commands)
{
    env e;
    CHECK(start(&e));
    tc_res r;

    r = call_aruid_u64(&e, 120, 1);
    CHECK_EQ(rc_of(r), 0);
    r = tc_send(e.c, e.hid, req_aruid(121));
    CHECK_EQ(tc_u64(&r, 0), 1);
    CHECK(rc_of(call_aruid_u64(&e, 120, 5)) != 0);

    CHECK_EQ(rc_of(call_aruid_u64(&e, 128, 2)), 0);
    r = tc_send(e.c, e.hid, req_aruid(129));
    CHECK_EQ(tc_u64(&r, 0), 2);

    CHECK_EQ(rc_of(call_aruid_u64(&e, 1000, 3)), 0);
    r = tc_send(e.c, e.hid, hreq(1001));
    CHECK_EQ(tc_u64(&r, 0), 3);

    CHECK_EQ(rc_of(call_u32(&e, 123, 2)), 0);
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    CHECK_EQ(rc_of(call_u32(&e, 123, 0)), 0);
    CHECK_EQ(e.mem->npads[0].state.joy_assignment_mode, 1);
    CHECK_EQ(rc_of(call_u32(&e, 124, 0)), 0);
    CHECK_EQ(e.mem->npads[0].state.joy_assignment_mode, 0);
    CHECK_EQ(rc_of(call_u32(&e, 122, 32)), 0);
    CHECK_EQ(e.mem->npads[8].state.joy_assignment_mode, 1);

    static const uint64_t led[] = { 1, 3, 7, 15, 9, 5, 13, 6 };
    for (uint32_t i = 0; i < 8; i++) {
        tc_req q = hreq(108);
        tc_put32(&q, i);
        r = tc_send(e.c, e.hid, q);
        CHECK_EQ(tc_u64(&r, 0), led[i]);
    }
    tc_req led_bad = hreq(108);
    tc_put32(&led_bad, 9);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, led_bad)), RL_MAKE_RESULT(202, 123));

    r = call_u32(&e, 68, 0);
    CHECK_EQ(r.data[0], 0);
    tc_req en = hreq(69);
    tc_put32(&en, 1);
    tc_put32(&en, 0x03000100);
    tc_put64(&en, 0x1234);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, en)), 0);
    r = call_u32(&e, 68, 0);
    CHECK_EQ(r.data[0], 1);

    tc_req fp = hreq(70);
    tc_put32(&fp, 0);
    float a = 0.5f, b = 0.25f;
    tc_put(&fp, &a, 4);
    tc_put(&fp, &b, 4);
    tc_put32(&fp, 0);
    tc_put64(&fp, 0x1234);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, fp)), 0);
    r = call_u32(&e, 71, 0);
    float got[2];
    memcpy(got, r.data, 8);
    CHECK(got[0] == 0.5f && got[1] == 0.25f);
    CHECK_EQ(rc_of(call_u32(&e, 72, 0)), 0);
    r = call_u32(&e, 71, 0);
    memcpy(got, r.data, 8);
    CHECK(got[0] == 0.0f && got[1] == 0.0f);

    r = call_u32(&e, 80, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    tc_req gz = hreq(79);
    tc_put32(&gz, 0);
    tc_put32(&gz, 2);
    tc_put64(&gz, 0x1234);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, gz)), 0);
    r = call_u32(&e, 80, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
    CHECK_EQ(rc_of(call_u32(&e, 81, 0)), 0);
    r = call_u32(&e, 80, 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK_EQ(call_u32(&e, 82, 0).data[0], 1);
    CHECK_EQ(call_u32(&e, 83, 0).data[0], 0);
    CHECK_EQ(rc_of(call_u32(&e, 66, 0)), 0);
    CHECK_EQ(rc_of(call_u32(&e, 67, 0)), 0);
    CHECK_EQ(rc_of(call_u32(&e, 91, 1)), 0);

    tc_req hp = hreq(132);
    tc_put32(&hp, 1);
    tc_put32(&hp, 0);
    tc_put64(&hp, 0x1234);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, hp)), 0);
    CHECK_EQ(call_u32(&e, 131, 0).data[0], 1);

    CHECK_EQ(call_u32(&e, 400, 0).svc_rc, 0);
    tc_req uf = hreq(401);
    tc_put32(&uf, 1);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, uf)), 0);
    r = tc_send(e.c, e.hid, hreq(400));
    CHECK_EQ(r.data[0], 1);
    r = call_u32(&e, 404, 0);
    CHECK(r.data[0] == 1 && r.data[1] == 1);
    r = call_u32(&e, 403, 0);
    CHECK_EQ(r.data[0], 1);

    CHECK_EQ(rc_of(tc_send(e.c, e.hid, hreq(9999))), SF_RESULT_UNKNOWN_COMMAND);
    rl_hid_stop();
}

TEST(hid_vibration_commands)
{
    env e;
    CHECK(start(&e));
    tc_res r;
    tc_req q = hreq(200);
    tc_put32(&q, vib_handle(3, 0, 0));
    r = tc_send(e.c, e.hid, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK_EQ(tc_u32(&r, 4), 1);
    q = hreq(200);
    tc_put32(&q, vib_handle(3, 0, 1));
    r = tc_send(e.c, e.hid, q);
    CHECK_EQ(tc_u32(&r, 4), 2);
    q = hreq(200);
    tc_put32(&q, vib_handle(8, 0, 0));
    r = tc_send(e.c, e.hid, q);
    CHECK_EQ(tc_u32(&r, 0), 2);
    CHECK_EQ(tc_u32(&r, 4), 0);
    q = hreq(200);
    tc_put32(&q, vib_handle(0, 0, 0));
    r = tc_send(e.c, e.hid, q);
    CHECK_EQ(tc_u32(&r, 0), 0);
    q = hreq(200);
    tc_put32(&q, vib_handle(3, 50, 0));
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), RL_MAKE_RESULT(202, 123));
    q = hreq(200);
    tc_put32(&q, vib_handle(3, 0, 2));
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), RL_MAKE_RESULT(202, 124));

    q = hreq(202);
    tc_put32(&q, vib_handle(3, 0, 1));
    tc_put32(&q, 0);
    tc_put64(&q, 0x1234);
    r = tc_send(e.c, e.hid, q);
    float v[4];
    memcpy(v, r.data, 16);
    CHECK(v[0] == 0.0f && v[1] == 160.0f && v[2] == 0.0f && v[3] == 320.0f);

    float val[4] = { 0.5f, 100.0f, 0.75f, 200.0f };
    q = hreq(201);
    tc_put32(&q, vib_handle(3, 0, 1));
    tc_put(&q, val, 16);
    tc_put32(&q, 0);
    tc_put64(&q, 0x1234);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), 0);
    q = hreq(202);
    tc_put32(&q, vib_handle(3, 0, 1));
    tc_put32(&q, 0);
    tc_put64(&q, 0x1234);
    r = tc_send(e.c, e.hid, q);
    memcpy(v, r.data, 16);
    CHECK(v[0] == 0.5f && v[1] == 100.0f && v[2] == 0.75f && v[3] == 200.0f);

    float val2[4] = { 0.25f, 120.0f, 0.5f, 240.0f };
    q = hreq(213);
    tc_put32(&q, 2);
    tc_put32(&q, vib_handle(3, 0, 1));
    tc_put(&q, val2, 16);
    tc_put64(&q, 0x1234);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), 0);
    q = hreq(202);
    tc_put32(&q, vib_handle(3, 0, 1));
    tc_put32(&q, 0);
    tc_put64(&q, 0x1234);
    r = tc_send(e.c, e.hid, q);
    memcpy(v, r.data, 16);
    CHECK(v[0] == 0.25f && v[1] == 120.0f && v[2] == 0.5f && v[3] == 240.0f);

    uint32_t hs[2] = { vib_handle(3, 1, 0), vib_handle(3, 1, 1) };
    float vs[2][4] = { { 0.1f, 150.0f, 0.2f, 300.0f }, { 0.3f, 170.0f, 0.4f, 310.0f } };
    memcpy(t_mem() + e.heap + 0x2000, hs, sizeof hs);
    memcpy(t_mem() + e.heap + 0x2100, vs, sizeof vs);
    q = hreq(206);
    tc_put64(&q, 0x1234);
    q.na = 2;
    q.a_addr[0] = e.heap + 0x2000;
    q.a_size[0] = sizeof hs;
    q.a_addr[1] = e.heap + 0x2100;
    q.a_size[1] = sizeof vs;
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), 0);
    q = hreq(202);
    tc_put32(&q, vib_handle(3, 1, 1));
    tc_put32(&q, 0);
    tc_put64(&q, 0x1234);
    r = tc_send(e.c, e.hid, q);
    memcpy(v, r.data, 16);
    CHECK(v[0] == 0.3f && v[1] == 170.0f && v[2] == 0.4f && v[3] == 310.0f);

    r = tc_send(e.c, e.hid, hreq(205));
    CHECK_EQ(r.data[0], 1);
    q = hreq(204);
    tc_put32(&q, 0);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), 0);
    r = tc_send(e.c, e.hid, hreq(205));
    CHECK_EQ(r.data[0], 0);

    tc_obj list;
    CHECK_EQ(tc_sub(e.c, e.hid, hreq(203), &list), 0);
    q = tc_new(0);
    tc_put32(&q, vib_handle(3, 0, 0));
    CHECK_EQ(rc_of(tc_send(e.c, list, q)), 0);
    CHECK_EQ(rc_of(tc_send(e.c, list, tc_new(1))), SF_RESULT_UNKNOWN_COMMAND);
    rl_hid_stop();
}

TEST(hid_sys_service)
{
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    tc_obj sys = tc_open_service(e.c, e.heap, "hid:sys");
    CHECK(sys.handle);
    tc_res r = tc_send(e.c, sys, tc_new(307));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), HID_STYLE_FULL_KEY | HID_STYLE_HANDHELD | HID_STYLE_JOY_DUAL | HID_STYLE_JOY_LEFT | HID_STYLE_JOY_RIGHT);
    tc_req q = tc_new(314);
    tc_put32(&q, 0);
    r = tc_send(e.c, sys, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 12);
    q = tc_new(314);
    tc_put32(&q, 1);
    r = tc_send(e.c, sys, q);
    CHECK_EQ(r.data[0], 0);
    q = tc_new(314);
    tc_put32(&q, 99);
    CHECK_EQ(rc_of(tc_send(e.c, sys, q)), RL_MAKE_RESULT(202, 123));
    q = tc_new(306);
    tc_put32(&q, 0);
    tc_put32(&q, 0);
    r = tc_send(e.c, sys, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.data[0], 12);
    q = tc_new(303);
    tc_put64(&q, 0);
    CHECK_EQ(rc_of(tc_send(e.c, sys, q)), 0);
    rl_hid_stop();
}

TEST(hid_irs_service)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj irs = tc_open_service(c, heap, "irs");
    CHECK(irs.handle);
    tc_req q = tc_new(302);
    q.send_pid = true;
    tc_put64(&q, 1);
    CHECK_EQ(rc_of(tc_send(c, irs, q)), 0);
    q = tc_new(304);
    q.send_pid = true;
    tc_put64(&q, 1);
    tc_res r = tc_send(c, irs, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    uint64_t at = t_free_map_addr();
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, r.copy[0], at, 0x8000, HOS_PERM_R, 0, 0), 0);
    CHECK(!t_read(at + 0x7FFF).faulted);
    CHECK(t_write(at, 1).faulted);
    q = tc_new(311);
    tc_put32(&q, 0);
    r = tc_send(c, irs, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    q = tc_new(311);
    tc_put32(&q, 32);
    r = tc_send(c, irs, q);
    CHECK_EQ(tc_u32(&r, 0), 8);
    q = tc_new(311);
    tc_put32(&q, 9);
    CHECK_EQ(rc_of(tc_send(c, irs, q)), RL_MAKE_RESULT(205, 709));
    q = tc_new(309);
    q.send_pid = true;
    tc_put32(&q, 0);
    tc_put32(&q, 0);
    tc_put64(&q, 1);
    CHECK_EQ(rc_of(tc_send(c, irs, q)), RL_MAKE_RESULT(205, 207));
    q = tc_new(309);
    q.send_pid = true;
    tc_put32(&q, 0);
    tc_put32(&q, 0);
    tc_put64(&q, 1);
    q.nb = 1;
    q.b_addr[0] = heap;
    q.b_size[0] = 0x1000;
    r = tc_send(c, irs, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), 0);
    q = tc_new(305);
    tc_put64(&q, 0);
    CHECK_EQ(rc_of(tc_send(c, irs, q)), 0);
    tc_obj isys = tc_open_service(c, heap, "irs:sys");
    CHECK(isys.handle);
    CHECK_EQ(rc_of(tc_send(c, isys, tc_new(0))), SF_RESULT_UNKNOWN_COMMAND);
    tc_obj dbg = tc_open_service(c, heap, "hid:dbg");
    CHECK(dbg.handle);
    CHECK_EQ(rc_of(tc_send(c, dbg, tc_new(0))), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(hid_palma_and_xpad_events)
{
    env e;
    CHECK(start(&e));
    tc_req q = hreq(40);
    tc_put64(&q, 0);
    tc_res r = tc_send(e.c, e.hid, q);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    CHECK_EQ(t_svc(e.c, SVC_RESET_SIGNAL, r.copy[0], 0, 0, 0, 0, 0), 0);
    q = hreq(502);
    tc_put64(&q, 0);
    r = tc_send(e.c, e.hid, q);
    CHECK_EQ(r.ncopy, 1);
    uint32_t ev = r.copy[0];
    CHECK(t_svc(e.c, SVC_RESET_SIGNAL, ev, 0, 0, 0, 0, 0) != 0);
    q = hreq(501);
    tc_put64(&q, 0);
    CHECK_EQ(rc_of(tc_send(e.c, e.hid, q)), 0);
    CHECK_EQ(t_svc(e.c, SVC_RESET_SIGNAL, ev, 0, 0, 0, 0, 0), 0);
    rl_hid_stop();
}

TEST(hid_update_thread_runs_and_stops)
{
    env e;
    CHECK(start_with(&e, false));
    CHECK(rl_hid_is_active());
    const hid_lifo_header *h = &e.mem->debug_pad.header;
    uint64_t n0 = h->count ? e.mem->debug_pad.entries[h->tail].state.sampling_number : 0;
    while ((h->count ? e.mem->debug_pad.entries[h->tail].state.sampling_number : 0) < n0 + 5)
        usleep(10000);
    uint64_t n1 = h->count ? e.mem->debug_pad.entries[h->tail].state.sampling_number : 0;
    CHECK(n1 >= n0 + 5);
    CHECK(h->timestamp != 0);
    CHECK_EQ(t_svc(e.c, SVC_UNMAP_SHARED_MEMORY, e.shm, e.at, HID_SHMEM_SIZE, 0, 0, 0), 0);
    CHECK_EQ(t_svc(e.c, SVC_CLOSE_HANDLE, e.shm, 0, 0, 0, 0, 0), 0);
    tc_close(e.c, e.ar.handle);
    while (rl_hid_is_active())
        usleep(10000);
    CHECK(!rl_hid_is_active());
    rl_hid_stop();
    CHECK(!rl_hid_is_active());
}

TEST(hid_stop_is_idempotent_and_restartable)
{
    env e;
    CHECK(start_with(&e, false));
    rl_hid_stop();
    rl_hid_stop();
    CHECK(!rl_hid_is_active());
    rl_hid_update_now();
    tc_obj ar2;
    tc_req q = hreq(0);
    tc_put64(&q, 0x1234);
    CHECK_EQ(tc_sub(e.c, e.hid, q, &ar2), 0);
    CHECK(rl_hid_is_active());
    rl_hid_stop();
}

TEST(hid_reports_bound_keyboard_inputs)
{
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    rl_hid_update_now();
    CHECK(rl_ports_apply_setting("bind.p1.a", "key:x"));
    CHECK(rl_ports_apply_setting("bind.p1.lstick_x", "key:left-,key:right+"));
    CHECK(rl_ports_apply_setting("bind.p1.zr", "gamepad:south"));
    CHECK(rl_ports_apply_setting("bind.p1.dpad_right", ""));
    rl_input_snapshot s = blank();
    s.keys[0] |= 1ull << 27;
    s.keys[1] |= 1ull << (79 % 64);
    s.pads[0].buttons = (1u << RL_BTN_EAST) | (1u << RL_BTN_SOUTH);
    publish(&s);
    rl_hid_update_now();
    hid_npad_state st[17];
    CHECK_EQ(GET_STATES(e.mem->npads[0].state.full_key, st, 17), 16);
    CHECK_EQ(st[0].buttons, HID_BTN_ZR | HID_BTN_B | HID_BTN_A | HID_BTN_STICK_L_RIGHT);
    CHECK_EQ(st[0].l.x, 32767);
    rl_hid_stop();
}

TEST(hid_each_player_reads_its_own_port)
{
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    rl_input_snapshot s = blank();
    s.pads[1].connected = true;
    s.pads[1].buttons = 1u << RL_BTN_NORTH;
    publish(&s);
    rl_hid_update_now();
    hid_npad_state p1[17], p2[17];
    CHECK(GET_STATES(e.mem->npads[0].state.full_key, p1, 17) >= 1);
    CHECK(GET_STATES(e.mem->npads[1].state.full_key, p2, 17) >= 1);
    CHECK_EQ(p1[0].buttons, 0);
    CHECK_EQ(p2[0].buttons, HID_BTN_X);
    CHECK(rl_ports_apply_setting("bind.p2.x", "gamepad1:south"));
    s.pads[0].buttons = 1u << RL_BTN_SOUTH;
    publish(&s);
    rl_hid_update_now();
    CHECK(GET_STATES(e.mem->npads[1].state.full_key, p2, 17) >= 1);
    CHECK_EQ(p2[0].buttons, HID_BTN_X);
    rl_hid_stop();
}

TEST(hid_declares_eight_players_and_touch)
{
    env e;
    CHECK(start(&e));
    CHECK_EQ(cmd_aruid(&e, 103), 0);
    rl_hid_update_now();
    CHECK_EQ(rl_ports_count(), 9);
    CHECK(rl_ports_find("p8", "zl") != RL_PORT_NONE);
    CHECK(rl_ports_find("touch", "press") != RL_PORT_NONE);
    rl_hid_stop();
}
