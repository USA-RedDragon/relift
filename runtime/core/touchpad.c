/**
 * @file touchpad.c
 * @brief The on-screen controller: placement of the guest's layout on the
 * view, the touch rules, the port values, the pause sheet, the settings
 * sheet, the layout editor, toasts and the overlay drawing.
 *
 * Sizes are in dp: a layout is written in the pixels of a reference screen
 * (RL_TOUCHPAD_REF_PX_PER_DP), so on the view a shape is its reference size
 * times px_per_dp / RL_TOUCHPAD_REF_PX_PER_DP times the size setting. A
 * group without a saved place keeps its reference distance in dp from the
 * nearer screen edge on each axis (or its fraction of the screen when it sits
 * in the middle third), so the pad hugs the edges on every screen shape.
 */
#include <relift/input_names.h>
#include <relift/settings.h>
#include <relift/touchpad.h>

#include "core.h"
#include "touchpad_int.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FINGERS 10
#define MAX_ITEMS 32
#define MAX_GROUPS 16
#define FADE_NS 150000000ull
#define TOAST_NS 3000000000ull
#define TOAST_FADE_NS 300000000ull
#define HOLD_NS 500000000ull
#define QUIET_NS 1500000000ull
#define MIN_PRESS_NS 50000000ull
#define PORTRAIT 0
#define LANDSCAPE 1

enum { OWN_NONE, OWN_ITEM, OWN_GAME, OWN_UI };
enum { UI_NONE, UI_PAUSE, UI_SETTINGS, UI_EDIT };
enum { SHOW_AUTO, SHOW_ALWAYS, SHOW_NEVER };

enum {
    T_NONE,
    T_RESUME,
    T_EDIT,
    T_SETTINGS,
    T_LEAVE,
    T_BACK,
    T_SHOW,
    T_OPACITY,
    T_SIZE,
    T_HAPTICS,
    T_EDIT_ROW,
    T_RESET,
    T_DONE,
    T_ATTACH,
    T_GROUP_DOT,
    T_ITEM_DOT,
    T_SPACING,
    T_ITEM_MOVE,
    T_GROUP_MOVE
};

typedef struct finger {
    bool on;
    uint64_t id;
    int own;
    int item;
    float x, y, x0, y0;
    uint64_t t0;
    int target;
    int arg;
    int hold_item;
    float v0, w0;
    bool pressed;
} finger;

typedef struct group_place {
    bool custom;
    float x, y, scale, spacing;
} group_place;

typedef struct item_place {
    bool detached;
    float x, y, scale;
} item_place;

typedef struct layout_state {
    group_place g[MAX_GROUPS];
    item_place d[MAX_ITEMS];
} layout_state;

typedef struct placed {
    float x, y, w, h;
    bool shown;
} placed;

typedef struct rect {
    float x0, y0, x1, y1;
} rect;

typedef struct key_slot {
    char key[128];
    rl_setting_def def;
} key_slot;

static const char *const g_show_choices[] = { "auto", "always", "never" };

static const rl_setting_def g_defs[] = {
    { .key = "input.touch_pad.show",
      .type = RL_SETTING_ENUM,
      .label = "Show",
      .description = "Automatic shows the on-screen controller only while no controller is connected",
      .apply = RL_APPLY_LIVE,
      .choices = g_show_choices,
      .nchoices = 3 },
    { .key = "input.touch_pad.opacity",
      .type = RL_SETTING_INT,
      .label = "Opacity",
      .description = "How see-through the buttons are",
      .apply = RL_APPLY_LIVE,
      .def_int = 60,
      .min_int = 20,
      .max_int = 100,
      .step_int = 1 },
    { .key = "input.touch_pad.size",
      .type = RL_SETTING_INT,
      .label = "Button size",
      .description = "Bigger buttons are easier to hit",
      .apply = RL_APPLY_LIVE,
      .def_int = 100,
      .min_int = 70,
      .max_int = 150,
      .step_int = 1 },
    { .key = "input.touch_pad.haptics",
      .type = RL_SETTING_BOOL,
      .label = "Vibrate on press",
      .description = "A short tick when a button goes down",
      .apply = RL_APPLY_LIVE,
      .def_int = 1 },
};

typedef struct input_name {
    const char *kind;
    const char *name;
    int value;
} input_name;

static const input_name g_names[] = {
#define X(kind, name, value) { #kind, name, value },
    RL_INPUT_NAMES(X)
#undef X
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t resumed;
    const rl_touchpad_guest *guest;
    rl_touchpad_host host;
    key_slot **keys;
    size_t nkeys;
    int8_t index[2][MAX_ITEMS][4];
    uint32_t w, h;
    float ppd;
    int32_t safe[4];
    bool touch_screen;
    bool have_view;
    uint64_t view_ns;
    uint32_t pads;
    float fade;
    uint64_t now, last;
    bool hidden;
    int ui;
    char toast[192];
    uint64_t toast_at;
    layout_state live[2], edit;
    bool loaded[2];
    float values[RL_INPUT_TOUCH_PAD];
    uint64_t held_until[RL_INPUT_TOUCH_PAD];
    uint8_t dirs[MAX_ITEMS];
    finger f[FINGERS];
    bool dirty;
    bool tick;
    bool leave;
    rl_tp_canvas cv;
    int32_t drawn[4];
} g = { .lock = PTHREAD_MUTEX_INITIALIZER,
        .resumed = PTHREAD_COND_INITIALIZER,
        .ppd = 1.0f,
#ifdef __ANDROID__
        .touch_screen = true,
#endif
        .fade = -1.0f };

static int touch_index(const char *name)
{
    for (size_t i = 0; name && i < sizeof g_names / sizeof g_names[0]; i++)
        if (!strcmp(g_names[i].kind, "TOUCH") && !strcmp(g_names[i].name, name))
            return g_names[i].value;
    return -1;
}

static int orient(void) { return g.h > g.w ? PORTRAIT : LANDSCAPE; }

static const rl_touchpad_layout *layout_of(int o)
{
    return !g.guest ? NULL : o == PORTRAIT ? &g.guest->portrait : &g.guest->landscape;
}

static const rl_touchpad_layout *lay(void) { return layout_of(orient()); }

static int show_mode(void)
{
    uint32_t v = rl_setting_enum("input.touch_pad.show");
    return v > SHOW_NEVER ? SHOW_AUTO : (int)v;
}

static bool wanted(void) { return g.guest != NULL && (g.touch_screen || show_mode() == SHOW_ALWAYS); }

static bool active(void)
{
    if (!wanted())
        return false;
    int m = show_mode();
    return m == SHOW_ALWAYS || (m == SHOW_AUTO && g.pads == 0);
}

static float size_factor(void)
{
    int64_t s = rl_setting_int("input.touch_pad.size");
    return (float)(s ? s : 100) / 100.0f;
}

static float opacity(void)
{
    int64_t o = rl_setting_int("input.touch_pad.opacity");
    return (float)(o ? o : 60) / 100.0f;
}

static float dp(float v) { return v * g.ppd; }

static float ref(float v) { return v * g.ppd / RL_TOUCHPAD_REF_PX_PER_DP; }

static bool detachable(const rl_touchpad_item *it)
{
    return (it->shape == RL_TOUCHPAD_BUTTON || it->shape == RL_TOUCHPAD_PILL) && it->controls[0];
}

static void item_size(const rl_touchpad_item *it, float *w, float *h)
{
    *w = it->w;
    *h = it->shape == RL_TOUCHPAD_PILL || it->shape == RL_TOUCHPAD_HIDE || it->shape == RL_TOUCHPAD_MENU ? it->h
                                                                                                         : it->w;
}

static const char *orient_name(int o) { return o == PORTRAIT ? "portrait" : "landscape"; }

static void group_key(char *out, size_t n, int o, const char *group)
{
    snprintf(out, n, "input.touch_pad.layout.%s.%s.%s", g.guest->name, orient_name(o), group);
}

static void control_key(char *out, size_t n, int o, const char *control)
{
    snprintf(out, n, "input.touch_pad.layout.%s.%s.control.%s", g.guest->name, orient_name(o), control);
}

static void load(int o)
{
    const rl_touchpad_layout *L = layout_of(o);
    layout_state *s = &g.live[o];
    memset(s, 0, sizeof *s);
    g.loaded[o] = true;
    if (!L)
        return;
    char key[160];
    for (uint32_t i = 0; i < L->ngroups && i < MAX_GROUPS; i++) {
        group_key(key, sizeof key, o, L->groups[i].id);
        const char *v = rl_setting_str(key);
        float x, y, sc, sp = 1.0f;
        int n = v && *v ? sscanf(v, "%f,%f,%f,%f", &x, &y, &sc, &sp) : 0;
        if (n >= 3 && x >= 0 && x <= 1 && y >= 0 && y <= 1 && sc >= 0.5f && sc <= 2.0f && sp >= 0.6f && sp <= 1.8f)
            s->g[i] = (group_place){ true, x, y, sc, sp };
    }
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++) {
        if (!detachable(&L->items[i]))
            continue;
        control_key(key, sizeof key, o, L->items[i].controls[0]);
        const char *v = rl_setting_str(key);
        float x, y, sc;
        if (v && *v && sscanf(v, "%f,%f,%f", &x, &y, &sc) == 3 && x >= 0 && x <= 1 && y >= 0 && y <= 1 &&
            sc >= 0.5f && sc <= 2.0f)
            s->d[i] = (item_place){ true, x, y, sc };
    }
}

static layout_state *state(void)
{
    int o = orient();
    if (g.ui == UI_EDIT)
        return &g.edit;
    if (!g.loaded[o])
        load(o);
    return &g.live[o];
}

static int group_of(const rl_touchpad_layout *L, uint32_t item)
{
    for (uint32_t i = 0; i < L->ngroups; i++)
        if (item >= L->groups[i].first && item < L->groups[i].first + L->groups[i].count)
            return (int)i;
    return -1;
}

static rect ref_box(const rl_touchpad_layout *L, uint32_t first, uint32_t count)
{
    rect r = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (uint32_t i = first; i < first + count && i < L->nitems; i++) {
        float w, h;
        item_size(&L->items[i], &w, &h);
        r.x0 = fminf(r.x0, L->items[i].x - w / 2);
        r.y0 = fminf(r.y0, L->items[i].y - h / 2);
        r.x1 = fmaxf(r.x1, L->items[i].x + w / 2);
        r.y1 = fmaxf(r.y1, L->items[i].y + h / 2);
    }
    return r;
}

static float anchor(float c0, float c1, float ref_len, float len, float lo_inset, float hi_inset, float half)
{
    float mid = (c0 + c1) / 2;
    if (mid < ref_len * 0.34f)
        return lo_inset + ref(c0) + half;
    if (mid > ref_len * 0.66f)
        return (float)len - hi_inset - ref(ref_len - c1) - half;
    return mid / ref_len * (float)len;
}

static rect limits(void)
{
    rect r = { (float)g.safe[0] + 16.0f, (float)g.safe[1] + 16.0f, (float)g.w - (float)g.safe[2] - 16.0f,
               (float)g.h - (float)g.safe[3] - 16.0f };
    return r;
}

static rect box_of(const placed *p, uint32_t first, uint32_t count, const bool *skip)
{
    rect r = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (uint32_t i = first; i < first + count; i++) {
        if (skip && skip[i])
            continue;
        r.x0 = fminf(r.x0, p[i].x - p[i].w / 2);
        r.y0 = fminf(r.y0, p[i].y - p[i].h / 2);
        r.x1 = fmaxf(r.x1, p[i].x + p[i].w / 2);
        r.y1 = fmaxf(r.y1, p[i].y + p[i].h / 2);
    }
    return r;
}

static void shift_inside(placed *p, uint32_t first, uint32_t count, const bool *skip)
{
    rect b = box_of(p, first, count, skip), l = limits();
    if (b.x1 < b.x0)
        return;
    float dx = 0, dy = 0;
    if (b.x0 < l.x0)
        dx = l.x0 - b.x0;
    else if (b.x1 > l.x1)
        dx = l.x1 - b.x1;
    if (b.y0 < l.y0)
        dy = l.y0 - b.y0;
    else if (b.y1 > l.y1)
        dy = l.y1 - b.y1;
    for (uint32_t i = first; i < first + count; i++)
        if (!skip || !skip[i]) {
            p[i].x += dx;
            p[i].y += dy;
        }
}

/**
 * A built-in group beside a picture the pad must not cover (a guest without game touch) moves toward the screen
 * edge, and shrinks when the side bar is still too narrow, so it ends a few dp short of the picture.
 */
static void keep_off_picture(placed *p, uint32_t first, uint32_t count, const bool *skip)
{
    int32_t pic[4];
    rl_touchpad_picture(pic);
    rect b = box_of(p, first, count, skip), l = limits();
    if (pic[2] <= 0 || b.x1 < b.x0 || b.y1 <= (float)pic[1] || b.y0 >= (float)(pic[1] + pic[3]))
        return;
    float gap = dp(4.0f), px0 = (float)pic[0] - gap, px1 = (float)(pic[0] + pic[2]) + gap;
    bool left = (b.x0 + b.x1) / 2 < (float)g.w / 2;
    if (left ? b.x1 <= px0 : b.x0 >= px1)
        return;
    float room = left ? px0 - l.x0 : l.x1 - px1;
    if (room <= dp(24.0f))
        return;
    float w = b.x1 - b.x0, k = w > room ? room / w : 1.0f;
    float ax = left ? l.x0 : l.x1 - w * k, ox = b.x0, cy = (b.y0 + b.y1) / 2;
    if (k == 1.0f)
        ax = left ? fmaxf(l.x0, px0 - w) : fminf(l.x1 - w, px1);
    for (uint32_t i = first; i < first + count; i++) {
        if (skip && skip[i])
            continue;
        p[i].x = ax + (p[i].x - ox) * k;
        p[i].y = cy + (p[i].y - cy) * k;
        p[i].w *= k;
        p[i].h *= k;
    }
}

static void place_group(const rl_touchpad_layout *L, const layout_state *s, uint32_t first, uint32_t count, int gi,
                        placed *p, const bool *skip)
{
    float k = size_factor();
    rect rb = ref_box(L, first, count);
    float cx = (rb.x0 + rb.x1) / 2, cy = (rb.y0 + rb.y1) / 2;
    const group_place *gp = gi >= 0 ? &s->g[gi] : NULL;
    float sc = gp && gp->custom ? gp->scale : 1.0f, sp = gp && gp->custom ? gp->spacing : 1.0f;
    float X, Y;
    if (gp && gp->custom) {
        X = gp->x * (float)g.w;
        Y = gp->y * (float)g.h;
    } else {
        X = anchor(rb.x0, rb.x1, L->width, (float)g.w, (float)g.safe[0], (float)g.safe[2],
                   ref(rb.x1 - rb.x0) * k / 2);
        Y = anchor(rb.y0, rb.y1, L->height, (float)g.h, (float)g.safe[1], (float)g.safe[3],
                   ref(rb.y1 - rb.y0) * k / 2);
    }
    for (uint32_t i = first; i < first + count; i++) {
        const rl_touchpad_item *it = &L->items[i];
        float w, h;
        item_size(it, &w, &h);
        p[i].x = X + ref(it->x - cx) * k * sc * sp;
        p[i].y = Y + ref(it->y - cy) * k * sc * sp;
        p[i].w = ref(w) * k * sc;
        p[i].h = ref(h) * k * sc;
        p[i].shown = true;
    }
    shift_inside(p, first, count, skip);
    if (!(gp && gp->custom) && !g.guest->game_touch && L->items[first].shape != RL_TOUCHPAD_MENU)
        keep_off_picture(p, first, count, skip);
}

static void place(const layout_state *s, placed *p)
{
    const rl_touchpad_layout *L = lay();
    memset(p, 0, sizeof(placed) * MAX_ITEMS);
    if (!L)
        return;
    bool skip[MAX_ITEMS] = { false };
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
        skip[i] = s->d[i].detached;
    for (uint32_t i = 0; i < L->ngroups && i < MAX_GROUPS; i++)
        place_group(L, s, L->groups[i].first, L->groups[i].count, (int)i, p, skip);
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++) {
        if (group_of(L, i) < 0)
            place_group(L, s, i, 1, -1, p, NULL);
        if (s->d[i].detached) {
            float w, h;
            item_size(&L->items[i], &w, &h);
            float k = size_factor();
            p[i].x = s->d[i].x * (float)g.w;
            p[i].y = s->d[i].y * (float)g.h;
            p[i].w = ref(w) * k * s->d[i].scale;
            p[i].h = ref(h) * k * s->d[i].scale;
            shift_inside(p, i, 1, NULL);
        }
    }
}

static bool item_visible(const rl_touchpad_item *it)
{
    if (g.ui == UI_EDIT)
        return it->shape != RL_TOUCHPAD_MENU;
    if (!active() && g.fade <= 0.0f)
        return false;
    if (g.hidden)
        return it->shape == RL_TOUCHPAD_HIDE;
    return true;
}

static float hit_distance(const rl_touchpad_item *it, const placed *p, float x, float y)
{
    float dx = x - p->x, dy = y - p->y;
    float min = dp(48.0f);
    if (it->shape == RL_TOUCHPAD_BUTTON || it->shape == RL_TOUCHPAD_STICK) {
        float r = fmaxf(p->w * (it->shape == RL_TOUCHPAD_STICK ? 1.2f : 1.15f), min) / 2;
        float d = sqrtf(dx * dx + dy * dy);
        return d <= r ? d : -1.0f;
    }
    float hw = fmaxf(p->w * 1.15f, min) / 2, hh = fmaxf(p->h * 1.15f, min) / 2;
    if (fabsf(dx) > hw || fabsf(dy) > hh)
        return -1.0f;
    return sqrtf(dx * dx + dy * dy);
}

static int hit(const placed *p, float x, float y, bool slide_only, int group)
{
    const rl_touchpad_layout *L = lay();
    int best = -1;
    float bd = 1e9f;
    for (uint32_t i = 0; L && i < L->nitems && i < MAX_ITEMS; i++) {
        const rl_touchpad_item *it = &L->items[i];
        if (!item_visible(it) || (slide_only && (!it->slide || group_of(L, i) != group)))
            continue;
        float d = hit_distance(it, &p[i], x, y);
        if (d >= 0 && d < bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}

static uint8_t dpad_dirs(const placed *p, float x, float y)
{
    float dx = x - p->x, dy = y - p->y;
    if (sqrtf(dx * dx + dy * dy) < p->w * 0.1f)
        return 0;
    float a = atan2f(-dy, dx) * 180.0f / 3.14159265f;
    if (a < 0)
        a += 360.0f;
    int z = (int)((a + 22.5f) / 45.0f) % 8;
    static const uint8_t zones[8] = { 2, 3, 1, 9, 8, 12, 4, 6 };
    return zones[z];
}

static void stick_value(const placed *p, float x, float y, float *vx, float *vy)
{
    float r = p->w / 2;
    float dx = (x - p->x) / r, dy = (p->y - y) / r;
    float m = sqrtf(dx * dx + dy * dy);
    if (m > 1.0f) {
        dx /= m;
        dy /= m;
        m = 1.0f;
    }
    if (m < 0.1f)
        dx = dy = 0;
    *vx = dx;
    *vy = dy;
}

static void set_value(int idx, float v)
{
    if (idx >= 0 && idx < RL_INPUT_TOUCH_PAD && fabsf(v) > fabsf(g.values[idx]))
        g.values[idx] = v;
}

static void press(int idx)
{
    set_value(idx, 1.0f);
    if (idx >= 0 && idx < RL_INPUT_TOUCH_PAD && g.held_until[idx] < g.now)
        g.held_until[idx] = g.now + MIN_PRESS_NS;
}

static void compute_values(void)
{
    const rl_touchpad_layout *L = lay();
    memset(g.values, 0, sizeof g.values);
    memset(g.dirs, 0, sizeof g.dirs);
    if (!L || g.ui != UI_NONE)
        return;
    placed p[MAX_ITEMS];
    place(state(), p);
    for (int i = 0; i < FINGERS; i++) {
        finger *f = &g.f[i];
        if (!f->on || f->own != OWN_ITEM || f->item < 0)
            continue;
        const rl_touchpad_item *it = &L->items[f->item];
        switch (it->shape) {
        case RL_TOUCHPAD_BUTTON:
        case RL_TOUCHPAD_PILL:
            press(g.index[orient()][f->item][0]);
            g.dirs[f->item] = 1;
            break;
        case RL_TOUCHPAD_DPAD: {
            uint8_t d = dpad_dirs(&p[f->item], f->x, f->y);
            g.dirs[f->item] |= d;
            for (int k = 0; k < 4; k++)
                if (d & (1u << k))
                    press(g.index[orient()][f->item][k]);
            break;
        }
        case RL_TOUCHPAD_STICK: {
            float vx, vy;
            stick_value(&p[f->item], f->x, f->y, &vx, &vy);
            set_value(g.index[orient()][f->item][0], vx);
            set_value(g.index[orient()][f->item][1], vy);
            g.dirs[f->item] = 1;
            break;
        }
        default:
            break;
        }
    }
}

static void release_pad(void)
{
    memset(g.held_until, 0, sizeof g.held_until);
    for (int i = 0; i < FINGERS; i++)
        if (g.f[i].on && (g.f[i].own == OWN_ITEM || g.f[i].own == OWN_GAME))
            g.f[i].own = OWN_NONE;
    memset(g.values, 0, sizeof g.values);
    memset(g.dirs, 0, sizeof g.dirs);
}

static void tick_haptic(void)
{
    if (rl_setting_bool("input.touch_pad.haptics"))
        g.tick = true;
}

static void toast(const char *a, const char *b)
{
    if (!g.have_view || g.now - g.view_ns < QUIET_NS)
        return;
    snprintf(g.toast, sizeof g.toast, "%s%s", a, b);
    g.toast_at = g.now;
}

static void open_ui(int ui)
{
    if (ui != UI_NONE && g.ui == UI_NONE)
        release_pad();
    if (ui == UI_EDIT) {
        g.edit = *state();
    }
    for (int i = 0; i < FINGERS; i++)
        if (g.f[i].on)
            g.f[i].own = OWN_NONE;
    g.ui = ui;
    g.dirty = true;
    if (ui == UI_NONE)
        pthread_cond_broadcast(&g.resumed);
}

/* ---- sheets */

typedef struct sheet {
    float k;
    rect card;
    rect rows[6];
    int targets[6];
    int n;
    rect back;
    float title_y;
} sheet;

static float ui_scale(float want_w, float want_h)
{
    float k = ref(1.0f);
    float sx = ((float)g.w * 0.94f) / (want_w * k), sy = ((float)g.h * 0.94f) / (want_h * k);
    float s = fminf(1.0f, fminf(sx, sy));
    return k * s;
}

static void pause_sheet(sheet *s)
{
    float k = ui_scale(820, 700);
    float cx = (float)g.w / 2, cy = (float)g.h / 2;
    s->k = k;
    s->card = (rect){ cx - 410 * k, cy - 350 * k, cx + 410 * k, cy + 350 * k };
    s->title_y = s->card.y0 + 70 * k;
    static const int t[4] = { T_RESUME, T_EDIT, T_SETTINGS, T_LEAVE };
    s->n = 4;
    for (int i = 0; i < 4; i++) {
        float y = s->card.y0 + 190 * k + (float)i * 130 * k;
        s->rows[i] = (rect){ cx - 340 * k, y - 50 * k, cx + 340 * k, y + 50 * k };
        s->targets[i] = t[i];
    }
    s->back = (rect){ 0, 0, 0, 0 };
}

static void settings_sheet(sheet *s)
{
    float rowh = 170, gap = 18, head = 130;
    float total = head + 5 * rowh + 4 * gap + 40;
    float k = ui_scale(1000, total);
    float cx = (float)g.w / 2, cy = (float)g.h / 2;
    s->k = k;
    s->card = (rect){ cx - 520 * k, cy - total / 2 * k, cx + 520 * k, cy + total / 2 * k };
    s->title_y = s->card.y0 + 65 * k;
    static const int t[5] = { T_SHOW, T_OPACITY, T_SIZE, T_HAPTICS, T_EDIT_ROW };
    s->n = 5;
    for (int i = 0; i < 5; i++) {
        float y0 = s->card.y0 + head * k + (float)i * (rowh + gap) * k;
        s->rows[i] = (rect){ cx - 500 * k, y0, cx + 500 * k, y0 + rowh * k };
        s->targets[i] = t[i];
    }
    s->back = (rect){ s->card.x1 - 200 * k, s->card.y0 + 30 * k, s->card.x1 - 30 * k, s->card.y0 + 100 * k };
}

static bool in(rect r, float x, float y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

static void slider_track(const sheet *s, int row, float *x0, float *x1, float *y)
{
    *x0 = s->rows[row].x0 + 40 * s->k;
    *x1 = s->rows[row].x1 - 40 * s->k;
    *y = s->rows[row].y1 - 40 * s->k;
}

static void slider_set(const sheet *s, int row, const char *key, int lo, int hi, float x, bool save)
{
    float x0, x1, y;
    slider_track(s, row, &x0, &x1, &y);
    float t = (x - x0) / (x1 - x0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    char v[16];
    snprintf(v, sizeof v, "%d", lo + (int)lroundf(t * (float)(hi - lo)));
    if (save)
        rl_settings_save(key, v);
    else
        rl_settings_set(key, v);
    g.dirty = true;
}

/* ---- editor */

typedef struct edit_view {
    placed p[MAX_ITEMS];
    rect gbox[MAX_GROUPS];
    bool gshown[MAX_GROUPS];
    rect ibox[MAX_ITEMS];
    rect bar, reset, done;
    float k;
} edit_view;

static void edit_layout(edit_view *v)
{
    const rl_touchpad_layout *L = lay();
    place(&g.edit, v->p);
    float pad = dp(6.0f);
    for (uint32_t i = 0; i < L->ngroups && i < MAX_GROUPS; i++) {
        bool skip[MAX_ITEMS];
        for (uint32_t j = 0; j < MAX_ITEMS; j++)
            skip[j] = j < L->nitems && g.edit.d[j].detached;
        rect b = box_of(v->p, L->groups[i].first, L->groups[i].count, skip);
        v->gshown[i] = b.x1 > b.x0;
        v->gbox[i] = (rect){ b.x0 - pad, b.y0 - pad, b.x1 + pad, b.y1 + pad };
    }
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
        v->ibox[i] = (rect){ v->p[i].x - v->p[i].w / 2 - pad, v->p[i].y - v->p[i].h / 2 - pad,
                             v->p[i].x + v->p[i].w / 2 + pad, v->p[i].y + v->p[i].h / 2 + pad };
    float k = ui_scale(1500, 1080);
    v->k = k;
    float bw = rl_tp_text_width("Drag to move. Drag the dot to resize. Hold a button to detach it.", 30 * k, false);
    float w = bw + 40 * k + 150 * k * 2 + 60 * k;
    float cx = (float)g.w / 2, y0 = (float)g.safe[1] + 20 * k;
    v->bar = (rect){ cx - w / 2, y0, cx + w / 2, y0 + 100 * k };
    v->done = (rect){ v->bar.x1 - 170 * k, y0 + 17 * k, v->bar.x1 - 20 * k, y0 + 83 * k };
    v->reset = (rect){ v->done.x0 - 165 * k, y0 + 17 * k, v->done.x0 - 15 * k, y0 + 83 * k };
}

static bool near(float x, float y, float px, float py) { return hypotf(x - px, y - py) <= dp(24.0f); }

static rect attach_rect(const edit_view *v, uint32_t i)
{
    rect b = v->ibox[i];
    float cx = (b.x0 + b.x1) / 2, k = v->k;
    float y = b.y1 + 12 * k;
    if (y + 48 * k > (float)g.h)
        y = b.y0 - 60 * k;
    return (rect){ cx - 65 * k, y, cx + 65 * k, y + 48 * k };
}

static bool fits(const placed *p, uint32_t first, uint32_t count, const bool *skip)
{
    rect b = box_of(p, first, count, skip), l = limits();
    return b.x1 < b.x0 || (b.x0 >= l.x0 - 0.5f && b.y0 >= l.y0 - 0.5f && b.x1 <= l.x1 + 0.5f && b.y1 <= l.y1 + 0.5f);
}

static void make_custom(int gi, const edit_view *v)
{
    group_place *gp = &g.edit.g[gi];
    if (gp->custom)
        return;
    const rl_touchpad_layout *L = lay();
    rect rb = ref_box(L, L->groups[gi].first, L->groups[gi].count);
    float cx = (rb.x0 + rb.x1) / 2, cy = (rb.y0 + rb.y1) / 2;
    uint32_t i = L->groups[gi].first;
    float k = size_factor();
    float X = v->p[i].x - ref(L->items[i].x - cx) * k, Y = v->p[i].y - ref(L->items[i].y - cy) * k;
    *gp = (group_place){ true, X / (float)g.w, Y / (float)g.h, 1.0f, 1.0f };
}

static bool group_ok(int gi)
{
    const rl_touchpad_layout *L = lay();
    placed p[MAX_ITEMS];
    place(&g.edit, p);
    bool skip[MAX_ITEMS] = { false };
    for (uint32_t j = 0; j < L->nitems && j < MAX_ITEMS; j++)
        skip[j] = g.edit.d[j].detached;
    const group_place *gp = &g.edit.g[gi];
    float k = size_factor();
    rect rb = ref_box(L, L->groups[gi].first, L->groups[gi].count);
    float cx = (rb.x0 + rb.x1) / 2, cy = (rb.y0 + rb.y1) / 2;
    for (uint32_t i = L->groups[gi].first; i < L->groups[gi].first + L->groups[gi].count; i++) {
        float w, h;
        item_size(&L->items[i], &w, &h);
        p[i].x = gp->x * (float)g.w + ref(L->items[i].x - cx) * k * gp->scale * gp->spacing;
        p[i].y = gp->y * (float)g.h + ref(L->items[i].y - cy) * k * gp->scale * gp->spacing;
        p[i].w = ref(w) * k * gp->scale;
        p[i].h = ref(h) * k * gp->scale;
    }
    return fits(p, L->groups[gi].first, L->groups[gi].count, skip);
}

static void clamp_group(int gi)
{
    group_place *gp = &g.edit.g[gi];
    const rl_touchpad_layout *L = lay();
    placed p[MAX_ITEMS];
    place(&g.edit, p);
    bool skip[MAX_ITEMS] = { false };
    for (uint32_t j = 0; j < L->nitems && j < MAX_ITEMS; j++)
        skip[j] = g.edit.d[j].detached;
    uint32_t f = L->groups[gi].first, n = L->groups[gi].count;
    rect before = box_of(p, f, n, skip);
    shift_inside(p, f, n, skip);
    rect after = box_of(p, f, n, skip);
    gp->x += (after.x0 - before.x0) / (float)g.w;
    gp->y += (after.y0 - before.y0) / (float)g.h;
}

static void clamp_item(uint32_t i)
{
    placed p[MAX_ITEMS];
    place(&g.edit, p);
    g.edit.d[i].x = p[i].x / (float)g.w;
    g.edit.d[i].y = p[i].y / (float)g.h;
}

static void edit_save(void)
{
    const rl_touchpad_layout *L = lay();
    int o = orient();
    char key[160], v[64];
    for (uint32_t i = 0; i < L->ngroups && i < MAX_GROUPS; i++) {
        group_key(key, sizeof key, o, L->groups[i].id);
        const group_place *gp = &g.edit.g[i];
        if (!gp->custom) {
            rl_settings_save(key, NULL);
            continue;
        }
        snprintf(v, sizeof v, "%.4f,%.4f,%.2f,%.2f", (double)gp->x, (double)gp->y, (double)gp->scale,
                 (double)gp->spacing);
        rl_settings_save(key, v);
    }
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++) {
        if (!detachable(&L->items[i]))
            continue;
        control_key(key, sizeof key, o, L->items[i].controls[0]);
        const item_place *d = &g.edit.d[i];
        if (!d->detached) {
            rl_settings_save(key, NULL);
            continue;
        }
        snprintf(v, sizeof v, "%.4f,%.4f,%.2f", (double)d->x, (double)d->y, (double)d->scale);
        rl_settings_save(key, v);
    }
    g.live[o] = g.edit;
}

static void edit_down(finger *f)
{
    const rl_touchpad_layout *L = lay();
    edit_view v;
    edit_layout(&v);
    f->target = T_NONE;
    f->hold_item = -1;
    if (in(v.reset, f->x, f->y)) {
        f->target = T_RESET;
        return;
    }
    if (in(v.done, f->x, f->y)) {
        f->target = T_DONE;
        return;
    }
    if (in(v.bar, f->x, f->y))
        return;
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
        if (g.edit.d[i].detached && in(attach_rect(&v, i), f->x, f->y)) {
            f->target = T_ATTACH;
            f->arg = (int)i;
            return;
        }
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
        if (g.edit.d[i].detached && near(f->x, f->y, v.ibox[i].x1 - 4, v.ibox[i].y1 - 4)) {
            f->target = T_ITEM_DOT;
            f->arg = (int)i;
            f->v0 = g.edit.d[i].scale;
            f->w0 = fmaxf(hypotf(f->x - v.p[i].x, f->y - v.p[i].y), 1.0f);
            return;
        }
    for (uint32_t i = 0; i < L->ngroups && i < MAX_GROUPS; i++) {
        if (!v.gshown[i])
            continue;
        rect b = v.gbox[i];
        float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
        if (near(f->x, f->y, b.x1 - 4, b.y1 - 4)) {
            make_custom((int)i, &v);
            f->target = T_GROUP_DOT;
            f->arg = (int)i;
            f->v0 = g.edit.g[i].scale;
            f->w0 = fmaxf(hypotf(f->x - cx, f->y - cy), 1.0f);
            return;
        }
        uint32_t attached = 0;
        for (uint32_t j = L->groups[i].first; j < L->groups[i].first + L->groups[i].count; j++)
            attached += !g.edit.d[j].detached;
        if (attached > 1 && near(f->x, f->y, cx, b.y0 - 30 * v.k)) {
            make_custom((int)i, &v);
            f->target = T_SPACING;
            f->arg = (int)i;
            f->v0 = g.edit.g[i].spacing;
            f->w0 = fmaxf(hypotf(f->x - cx, f->y - cy), 1.0f);
            return;
        }
    }
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
        if (g.edit.d[i].detached && in(v.ibox[i], f->x, f->y)) {
            f->target = T_ITEM_MOVE;
            f->arg = (int)i;
            f->v0 = g.edit.d[i].x;
            f->w0 = g.edit.d[i].y;
            return;
        }
    for (uint32_t i = 0; i < L->ngroups && i < MAX_GROUPS; i++) {
        if (!v.gshown[i] || !in(v.gbox[i], f->x, f->y))
            continue;
        f->target = T_GROUP_MOVE;
        f->arg = (int)i;
        for (uint32_t j = L->groups[i].first; j < L->groups[i].first + L->groups[i].count; j++)
            if (!g.edit.d[j].detached && detachable(&L->items[j]) && in(v.ibox[j], f->x, f->y))
                f->hold_item = (int)j;
        return;
    }
}

static void edit_move(finger *f)
{
    const rl_touchpad_layout *L = lay();
    float dx = f->x - f->x0, dy = f->y - f->y0;
    if (f->hold_item >= 0 && hypotf(dx, dy) > dp(8.0f))
        f->hold_item = -1;
    switch (f->target) {
    case T_GROUP_MOVE: {
        group_place *gp = &g.edit.g[f->arg];
        if (f->hold_item >= 0)
            return;
        if (!f->pressed) {
            edit_view v;
            edit_layout(&v);
            make_custom(f->arg, &v);
            f->v0 = gp->x;
            f->w0 = gp->y;
            f->pressed = true;
        }
        gp->x = f->v0 + dx / (float)g.w;
        gp->y = f->w0 + dy / (float)g.h;
        clamp_group(f->arg);
        break;
    }
    case T_ITEM_MOVE: {
        item_place *d = &g.edit.d[f->arg];
        d->x = f->v0 + dx / (float)g.w;
        d->y = f->w0 + dy / (float)g.h;
        clamp_item((uint32_t)f->arg);
        break;
    }
    case T_GROUP_DOT:
    case T_SPACING: {
        edit_view v;
        edit_layout(&v);
        rect b = v.gbox[f->arg];
        float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
        float r = hypotf(f->x - cx, f->y - cy) / f->w0;
        group_place *gp = &g.edit.g[f->arg];
        group_place keep = *gp;
        if (f->target == T_GROUP_DOT)
            gp->scale = fminf(2.0f, fmaxf(0.5f, f->v0 * r));
        else
            gp->spacing = fminf(1.8f, fmaxf(0.6f, f->v0 * r));
        if (!group_ok(f->arg))
            *gp = keep;
        break;
    }
    case T_ITEM_DOT: {
        placed p[MAX_ITEMS];
        place(&g.edit, p);
        item_place *d = &g.edit.d[f->arg];
        item_place keep = *d;
        d->scale = fminf(2.0f, fmaxf(0.5f, f->v0 * hypotf(f->x - p[f->arg].x, f->y - p[f->arg].y) / f->w0));
        float w, h;
        item_size(&L->items[f->arg], &w, &h);
        p[f->arg].w = ref(w) * size_factor() * d->scale;
        p[f->arg].h = ref(h) * size_factor() * d->scale;
        if (!fits(p, (uint32_t)f->arg, 1, NULL))
            *d = keep;
        break;
    }
    default:
        break;
    }
    g.dirty = true;
}

static void edit_hold(finger *f)
{
    if (f->hold_item < 0 || g.now - f->t0 < HOLD_NS)
        return;
    edit_view v;
    edit_layout(&v);
    uint32_t i = (uint32_t)f->hold_item;
    int gi = group_of(lay(), i);
    float sc = gi >= 0 && g.edit.g[gi].custom ? g.edit.g[gi].scale : 1.0f;
    g.edit.d[i] = (item_place){ true, v.p[i].x / (float)g.w, v.p[i].y / (float)g.h, sc };
    f->target = T_ITEM_MOVE;
    f->arg = (int)i;
    f->x0 = f->x;
    f->y0 = f->y;
    f->v0 = g.edit.d[i].x;
    f->w0 = g.edit.d[i].y;
    f->hold_item = -1;
    tick_haptic();
    g.dirty = true;
}

static void edit_up(finger *f)
{
    edit_view v;
    edit_layout(&v);
    switch (f->target) {
    case T_RESET:
        if (in(v.reset, f->x, f->y)) {
            memset(&g.edit, 0, sizeof g.edit);
            edit_save();
        }
        break;
    case T_DONE:
        if (in(v.done, f->x, f->y)) {
            edit_save();
            open_ui(UI_NONE);
        }
        break;
    case T_ATTACH:
        if (in(attach_rect(&v, (uint32_t)f->arg), f->x, f->y))
            g.edit.d[f->arg].detached = false;
        break;
    default:
        break;
    }
    g.dirty = true;
}

/* ---- sheet fingers */

static void sheet_down(finger *f)
{
    sheet s;
    if (g.ui == UI_PAUSE)
        pause_sheet(&s);
    else
        settings_sheet(&s);
    f->target = T_NONE;
    if (g.ui == UI_SETTINGS && in(s.back, f->x, f->y)) {
        f->target = T_BACK;
        return;
    }
    for (int i = 0; i < s.n; i++)
        if (in(s.rows[i], f->x, f->y)) {
            f->target = s.targets[i];
            f->arg = i;
        }
    if (f->target == T_OPACITY)
        slider_set(&s, f->arg, "input.touch_pad.opacity", 20, 100, f->x, false);
    else if (f->target == T_SIZE)
        slider_set(&s, f->arg, "input.touch_pad.size", 70, 150, f->x, false);
    f->pressed = true;
    g.dirty = true;
}

static void sheet_move(finger *f)
{
    sheet s;
    if (g.ui != UI_SETTINGS)
        return;
    settings_sheet(&s);
    if (f->target == T_OPACITY)
        slider_set(&s, f->arg, "input.touch_pad.opacity", 20, 100, f->x, false);
    else if (f->target == T_SIZE)
        slider_set(&s, f->arg, "input.touch_pad.size", 70, 150, f->x, false);
}

static void sheet_up(finger *f)
{
    sheet s;
    if (g.ui == UI_PAUSE)
        pause_sheet(&s);
    else
        settings_sheet(&s);
    bool inside = f->target == T_BACK ? in(s.back, f->x, f->y)
                                      : f->target != T_NONE && in(s.rows[f->arg], f->x, f->y);
    f->pressed = false;
    g.dirty = true;
    if (f->target == T_OPACITY) {
        slider_set(&s, f->arg, "input.touch_pad.opacity", 20, 100, f->x, true);
        return;
    }
    if (f->target == T_SIZE) {
        slider_set(&s, f->arg, "input.touch_pad.size", 70, 150, f->x, true);
        return;
    }
    if (!inside)
        return;
    switch (f->target) {
    case T_RESUME:
        open_ui(UI_NONE);
        break;
    case T_EDIT:
    case T_EDIT_ROW:
        open_ui(UI_EDIT);
        break;
    case T_SETTINGS:
        open_ui(UI_SETTINGS);
        break;
    case T_LEAVE:
        g.leave = true;
        open_ui(UI_NONE);
        break;
    case T_BACK:
        open_ui(UI_PAUSE);
        break;
    case T_SHOW:
        rl_settings_save("input.touch_pad.show", g_show_choices[(show_mode() + 1) % 3]);
        break;
    case T_HAPTICS:
        rl_settings_save("input.touch_pad.haptics", rl_setting_bool("input.touch_pad.haptics") ? "false" : "true");
        break;
    default:
        break;
    }
}

/* ---- pad fingers */

static bool in_picture(float x, float y, float *px, float *py)
{
    int32_t r[4];
    rl_touchpad_picture(r);
    if (r[2] <= 0 || r[3] <= 0 || x < (float)r[0] || y < (float)r[1] || x >= (float)(r[0] + r[2]) ||
        y >= (float)(r[1] + r[3]))
        return false;
    *px = (x - (float)r[0]) / (float)r[2];
    *py = (y - (float)r[1]) / (float)r[3];
    return true;
}

static void pad_down(finger *f)
{
    const rl_touchpad_layout *L = lay();
    placed p[MAX_ITEMS];
    place(state(), p);
    int i = L && active() ? hit(p, f->x, f->y, false, -1) : -1;
    float px, py;
    if (i < 0) {
        f->own = g.guest && g.guest->game_touch && in_picture(f->x, f->y, &px, &py) ? OWN_GAME : OWN_NONE;
        return;
    }
    const rl_touchpad_item *it = &L->items[i];
    if (it->shape == RL_TOUCHPAD_MENU) {
        f->own = OWN_NONE;
        open_ui(UI_PAUSE);
        return;
    }
    if (it->shape == RL_TOUCHPAD_HIDE) {
        f->own = OWN_NONE;
        g.hidden = !g.hidden;
        if (g.hidden)
            release_pad();
        g.dirty = true;
        return;
    }
    f->own = OWN_ITEM;
    f->item = i;
    if (it->shape != RL_TOUCHPAD_DPAD || dpad_dirs(&p[i], f->x, f->y))
        tick_haptic();
    g.dirty = true;
}

static void pad_move(finger *f)
{
    const rl_touchpad_layout *L = lay();
    if (f->own != OWN_ITEM || !L)
        return;
    const rl_touchpad_item *it = &L->items[f->item];
    placed p[MAX_ITEMS];
    place(state(), p);
    if (it->shape == RL_TOUCHPAD_DPAD) {
        uint8_t before = g.dirs[f->item], now = dpad_dirs(&p[f->item], f->x, f->y);
        if (now && now != before)
            tick_haptic();
        for (int k = 0; k < 4; k++) {
            int idx = g.index[orient()][f->item][k];
            if ((before & ~now & (1u << k)) && idx >= 0)
                g.held_until[idx] = 0;
        }
        g.dirty |= now != before;
    } else if (it->shape == RL_TOUCHPAD_STICK) {
        g.dirty = true;
    } else if (it->slide) {
        int o = hit(p, f->x, f->y, true, group_of(L, (uint32_t)f->item));
        if (o >= 0 && o != f->item) {
            int old = g.index[orient()][f->item][0];
            if (old >= 0)
                g.held_until[old] = 0;
            f->item = o;
            tick_haptic();
            g.dirty = true;
        }
    }
}

static finger *find_finger(uint64_t id, bool make)
{
    for (int i = 0; i < FINGERS; i++)
        if (g.f[i].on && g.f[i].id == id)
            return &g.f[i];
    if (!make)
        return NULL;
    for (int i = 0; i < FINGERS; i++)
        if (!g.f[i].on) {
            memset(&g.f[i], 0, sizeof g.f[i]);
            g.f[i].on = true;
            g.f[i].id = id;
            g.f[i].item = -1;
            g.f[i].hold_item = -1;
            return &g.f[i];
        }
    return NULL;
}

static void after(void)
{
    bool tick = g.tick, leave = g.leave;
    void (*haptic)(void) = g.host.haptic, (*leave_fn)(void) = g.host.leave, (*wake)(void) = g.host.wake;
    bool dirty = g.dirty;
    g.tick = false;
    g.leave = false;
    if (dirty)
        pthread_cond_broadcast(&g.resumed);
    pthread_mutex_unlock(&g.lock);
    if (tick && haptic)
        haptic();
    if (leave && leave_fn)
        leave_fn();
    if (dirty && wake)
        wake();
}

void rl_touchpad_finger(uint64_t id, int phase, float x, float y)
{
    pthread_mutex_lock(&g.lock);
    g.now = rl_clock_ns();
    finger *f = find_finger(id, phase == RL_TOUCHPAD_DOWN);
    if (!f || !g.guest) {
        pthread_mutex_unlock(&g.lock);
        return;
    }
    f->x = x;
    f->y = y;
    if (phase == RL_TOUCHPAD_DOWN) {
        f->x0 = x;
        f->y0 = y;
        f->t0 = g.now;
        if (g.ui == UI_NONE) {
            pad_down(f);
        } else {
            f->own = OWN_UI;
            if (g.ui == UI_EDIT)
                edit_down(f);
            else
                sheet_down(f);
        }
    } else if (phase == RL_TOUCHPAD_MOVE) {
        if (f->own == OWN_UI && g.ui == UI_EDIT)
            edit_move(f);
        else if (f->own == OWN_UI)
            sheet_move(f);
        else
            pad_move(f);
    } else {
        if (f->own == OWN_UI && phase == RL_TOUCHPAD_UP) {
            if (g.ui == UI_EDIT)
                edit_up(f);
            else if (g.ui != UI_NONE)
                sheet_up(f);
        }
        if (f->own == OWN_ITEM)
            g.dirty = true;
        f->on = false;
    }
    compute_values();
    after();
}

void rl_touchpad_back(void)
{
    pthread_mutex_lock(&g.lock);
    if (!g.guest) {
        pthread_mutex_unlock(&g.lock);
        return;
    }
    g.now = rl_clock_ns();
    switch (g.ui) {
    case UI_NONE:
        open_ui(UI_PAUSE);
        break;
    case UI_SETTINGS:
        open_ui(UI_PAUSE);
        break;
    default:
        open_ui(UI_NONE);
        break;
    }
    compute_values();
    after();
}

bool rl_touchpad_tick(uint64_t now_ns)
{
    pthread_mutex_lock(&g.lock);
    g.now = now_ns;
    uint64_t dt = g.last ? now_ns - g.last : 0;
    g.last = now_ns;
    float target = active() || g.ui == UI_EDIT ? 1.0f : 0.0f;
    if (g.fade < 0)
        g.fade = 0.0f;
    if (g.fade != target) {
        float step = (float)dt / (float)FADE_NS;
        g.fade = g.fade < target ? fminf(target, g.fade + step) : fmaxf(target, g.fade - step);
        g.dirty = true;
    }
    bool busy = g.fade != target;
    if (g.toast[0]) {
        if (now_ns - g.toast_at >= TOAST_NS) {
            g.toast[0] = 0;
            g.dirty = true;
        } else {
            busy = true;
            if (now_ns - g.toast_at >= TOAST_NS - TOAST_FADE_NS)
                g.dirty = true;
        }
    }
    for (int i = 0; i < FINGERS; i++)
        if (g.f[i].on && g.f[i].own == OWN_UI && g.ui == UI_EDIT && g.f[i].hold_item >= 0) {
            edit_hold(&g.f[i]);
            busy = true;
        }
    bool dirty = g.dirty;
    after();
    return busy || dirty;
}

void rl_touchpad_gamepad(bool connected, const char *name, uint32_t count)
{
    pthread_mutex_lock(&g.lock);
    g.now = rl_clock_ns();
    bool was = active();
    g.pads = count;
    bool now = active();
    const char *n = name && *name ? name : "Controller";
    if (was && !now) {
        release_pad();
        toast(n, " connected. On-screen controller hidden.");
    } else if (!was && now && !connected) {
        toast(n, " disconnected. On-screen controller shown.");
    }
    g.dirty = true;
    compute_values();
    after();
}

void rl_touchpad_view(uint32_t width, uint32_t height, float px_per_dp, const int32_t safe[4])
{
    pthread_mutex_lock(&g.lock);
    bool turned = g.have_view && ((g.h > g.w) != (height > width));
    if (turned) {
        release_pad();
        if (g.ui == UI_EDIT)
            open_ui(UI_PAUSE);
    }
    if (!g.have_view) {
        g.have_view = true;
        g.view_ns = rl_clock_ns();
    }
    g.w = width;
    g.h = height;
    float short_side = (float)(width < height ? width : height);
    g.ppd = px_per_dp > 0 ? px_per_dp : short_side > 0 ? short_side / 411.0f : 1.0f;
    for (int i = 0; i < 4; i++)
        g.safe[i] = safe ? safe[i] : 0;
    g.dirty = true;
    pthread_mutex_unlock(&g.lock);
}

void rl_touchpad_view_size(uint32_t *width, uint32_t *height)
{
    pthread_mutex_lock(&g.lock);
    *width = g.w;
    *height = g.h;
    pthread_mutex_unlock(&g.lock);
}

void rl_touchpad_picture(int32_t r[4])
{
    r[0] = r[1] = r[2] = r[3] = 0;
    const rl_touchpad_guest *gu = g.guest;
    if (!gu || !g.w || !g.h || !gu->picture_width || !gu->picture_height)
        return;
    float s = fminf((float)g.w / (float)gu->picture_width, (float)g.h / (float)gu->picture_height);
    if (gu->integer_scale && s >= 1.0f)
        s = floorf(s);
    int32_t w = (int32_t)((float)gu->picture_width * s), h = (int32_t)((float)gu->picture_height * s);
    r[0] = ((int32_t)g.w - w) / 2;
    r[1] = ((int32_t)g.h - h) / 2;
    if (g.h > g.w && active() && r[1] > g.safe[1])
        r[1] = g.safe[1];
    r[2] = w;
    r[3] = h;
}

void rl_touchpad_fill(float *values, uint32_t count)
{
    uint64_t now = rl_clock_ns();
    pthread_mutex_lock(&g.lock);
    for (uint32_t i = 0; i < count; i++)
        values[i] = i >= RL_INPUT_TOUCH_PAD ? 0.0f : g.values[i] != 0.0f || now >= g.held_until[i] ? g.values[i] : 1.0f;
    pthread_mutex_unlock(&g.lock);
}

uint32_t rl_touchpad_game_touches(rl_input_touch *out, uint32_t max)
{
    uint32_t n = 0;
    pthread_mutex_lock(&g.lock);
    bool pad = active() && g.guest;
    for (int i = 0; i < FINGERS && n < max; i++) {
        finger *f = &g.f[i];
        float px, py;
        if (!f->on || g.ui != UI_NONE)
            continue;
        bool inside = in_picture(f->x, f->y, &px, &py);
        if (pad ? f->own != OWN_GAME : f->own == OWN_UI || !inside)
            continue;
        if (!inside) {
            int32_t r[4];
            rl_touchpad_picture(r);
            px = r[2] > 0 ? fminf(1.0f, fmaxf(0.0f, (f->x - (float)r[0]) / (float)r[2])) : 0.0f;
            py = r[3] > 0 ? fminf(1.0f, fmaxf(0.0f, (f->y - (float)r[1]) / (float)r[3])) : 0.0f;
        }
        out[n].id = (uint32_t)f->id;
        out[n].x = (uint16_t)(px * 65535.0f);
        out[n].y = (uint16_t)(py * 65535.0f);
        n++;
    }
    pthread_mutex_unlock(&g.lock);
    return n;
}

bool rl_touchpad_paused(void)
{
    pthread_mutex_lock(&g.lock);
    bool p = g.ui != UI_NONE;
    pthread_mutex_unlock(&g.lock);
    return p;
}

bool rl_touchpad_hold(uint32_t ms)
{
    pthread_mutex_lock(&g.lock);
    if (g.ui != UI_NONE && !g.dirty) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        uint64_t ns = (uint64_t)t.tv_nsec + (uint64_t)ms * 1000000ull;
        t.tv_sec += (time_t)(ns / 1000000000ull);
        t.tv_nsec = (long)(ns % 1000000000ull);
        pthread_cond_timedwait(&g.resumed, &g.lock, &t);
    }
    bool paused = g.ui != UI_NONE;
    pthread_mutex_unlock(&g.lock);
    return paused;
}

/* ---- drawing */

static rl_tp_color C(uint32_t rgb, float a) { return rl_tp_rgb(rgb, a); }

static void draw_label(float x, float y, const char *s, float size, bool pressed, float fade, float k)
{
    rl_tp_text(&g.cv, x, y, s, size, true, RL_TP_CENTER, pressed ? C(0x222222, fade) : C(0xffffff, fade),
               C(0x000000, 0.55f * fade), pressed ? 0 : 3.0f * k);
}

static void draw_arrow(float cx, float cy, float s, int dir, bool pressed, float fade)
{
    float a = s / 2, t = s * 0.09f, p[6];
    float tip = a - 2.2f * t, base = a - 3.6f * t, half = t * 1.3f;
    static const float dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    float ux = dx[dir], uy = dy[dir], vx = -uy, vy = ux;
    p[0] = cx + ux * tip;
    p[1] = cy + uy * tip;
    p[2] = cx + ux * base + vx * half;
    p[3] = cy + uy * base + vy * half;
    p[4] = cx + ux * base - vx * half;
    p[5] = cy + uy * base - vy * half;
    if (pressed)
        rl_tp_triangle(&g.cv, p, C(0x222222, fade), C(0, 0), 0);
    else
        rl_tp_triangle(&g.cv, p, C(0xffffff, 0.9f * fade), C(0x000000, 0.5f * fade), s * 0.01f);
}

static void draw_item(const rl_touchpad_item *it, const placed *p, int idx, float op, float fade, bool edit)
{
    float k = p->w / fmaxf(it->w, 1.0f);
    bool pressed = !edit && g.dirs[idx];
    rl_tp_color base = C(0xd8dce3, 1), white = C(0xffffff, 1);
    switch (it->shape) {
    case RL_TOUCHPAD_BUTTON: {
        float r = p->w / 2;
        rl_tp_circle(&g.cv, p->x, p->y, r, pressed ? white : base, white, 6 * k, pressed ? 0.95f * fade : op);
        if (it->label)
            draw_label(p->x, p->y, it->label, r * 0.9f, pressed, fade, k);
        break;
    }
    case RL_TOUCHPAD_PILL:
    case RL_TOUCHPAD_HIDE: {
        float o = it->shape == RL_TOUCHPAD_HIDE ? op * 0.6f : op;
        rl_tp_rrect(&g.cv, p->x - p->w / 2, p->y - p->h / 2, p->x + p->w / 2, p->y + p->h / 2, p->h / 2,
                    pressed ? white : base, white, 6 * k, pressed ? 0.95f * fade : o);
        const char *label = it->shape == RL_TOUCHPAD_HIDE ? (g.hidden && !edit ? "Show" : "Hide") : it->label;
        if (label)
            draw_label(p->x, p->y, label, p->h * 0.46f, pressed, fade, k);
        break;
    }
    case RL_TOUCHPAD_MENU:
        rl_tp_rrect(&g.cv, p->x - p->w / 2, p->y - p->h / 2, p->x + p->w / 2, p->y + p->h / 2, p->h / 2,
                    C(0x2a2d33, 1), white, 5 * k, op * 0.8f);
        draw_label(p->x, p->y, "Menu", p->h * 0.46f, false, fade, k);
        break;
    case RL_TOUCHPAD_DPAD: {
        float s = p->w, arm = s * 0.34f;
        rl_tp_cross(&g.cv, p->x, p->y, s, arm, base, white, 10 * k, op);
        uint8_t d = edit ? 0 : g.dirs[idx];
        float a = s / 2, w = arm / 2;
        for (int dir = 0; dir < 4; dir++) {
            bool on = d & (1u << dir);
            if (on) {
                static const float sx[4] = { 0, 1, 0, -1 }, sy[4] = { -1, 0, 1, 0 };
                float x0 = sx[dir] > 0 ? p->x + w : sx[dir] < 0 ? p->x - a : p->x - w;
                float x1 = sx[dir] > 0 ? p->x + a : sx[dir] < 0 ? p->x - w : p->x + w;
                float y0 = sy[dir] > 0 ? p->y + w : sy[dir] < 0 ? p->y - a : p->y - w;
                float y1 = sy[dir] > 0 ? p->y + a : sy[dir] < 0 ? p->y - w : p->y + w;
                rl_tp_rrect(&g.cv, x0, y0, x1, y1, 10 * k, white, white, 0, 0.9f * fade);
            }
            draw_arrow(p->x, p->y, s, dir, on, fade);
        }
        break;
    }
    case RL_TOUCHPAD_STICK: {
        float r = p->w / 2, kx = 0, ky = 0;
        if (!edit && g.dirs[idx]) {
            int ix = g.index[orient()][idx][0], iy = g.index[orient()][idx][1];
            for (int i = 0; i < FINGERS; i++)
                if (g.f[i].on && g.f[i].own == OWN_ITEM && g.f[i].item == idx) {
                    float dx = g.f[i].x - p->x, dy = g.f[i].y - p->y, m = hypotf(dx, dy);
                    if (m > r * 0.5f) {
                        dx *= r * 0.5f / m;
                        dy *= r * 0.5f / m;
                    }
                    kx = dx;
                    ky = dy;
                }
            (void)ix;
            (void)iy;
        }
        rl_tp_circle(&g.cv, p->x, p->y, r, C(0x9aa0aa, 1), white, 6 * k, op);
        rl_tp_circle(&g.cv, p->x + kx, p->y + ky, r * 0.5f, C(0xeef0f4, 1), white, 5 * k,
                     fminf(fade, op + 0.25f * fade));
        break;
    }
    default:
        break;
    }
}

static void draw_pad(bool edit)
{
    const rl_touchpad_layout *L = lay();
    if (!L)
        return;
    placed p[MAX_ITEMS];
    place(edit ? &g.edit : state(), p);
    float fade = edit ? 1.0f : g.fade;
    float op = opacity() * fade;
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
        if (item_visible(&L->items[i]))
            draw_item(&L->items[i], &p[i], (int)i, op, fade, edit);
}

static void button(rect r, const char *text, float k, bool primary, bool down)
{
    rl_tp_rrect(&g.cv, r.x0, r.y0, r.x1, r.y1, 24 * k, primary ? C(0x8ab4ff, 1) : C(down ? 0x3b4049 : 0x2b2e35, 1),
                C(0, 0), 0, 1.0f);
    rl_tp_text(&g.cv, (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2, text, 36 * k, primary, RL_TP_CENTER,
               primary ? C(0x10131a, 1) : C(0xffffff, 1), C(0, 0), 0);
}

static bool finger_on(int target, int arg)
{
    for (int i = 0; i < FINGERS; i++)
        if (g.f[i].on && g.f[i].own == OWN_UI && g.f[i].pressed && g.f[i].target == target && g.f[i].arg == arg)
            return true;
    return false;
}

static void draw_pause(void)
{
    sheet s;
    pause_sheet(&s);
    float k = s.k;
    rl_tp_rrect(&g.cv, 0, 0, (float)g.w, (float)g.h, 0, C(0, 0.55f), C(0, 0), 0, 1.0f);
    rl_tp_rrect(&g.cv, s.card.x0, s.card.y0, s.card.x1, s.card.y1, 36 * k, C(0x1b1d22, 1), C(0x555555, 1), 4 * k,
                1.0f);
    rl_tp_text(&g.cv, (s.card.x0 + s.card.x1) / 2, s.title_y, "Paused", 52 * k, true, RL_TP_CENTER, C(0xffffff, 1),
               C(0, 0), 0);
    static const char *const names[4] = { "Resume", "Edit controller layout", "Settings", "Leave game" };
    for (int i = 0; i < 4; i++)
        button(s.rows[i], names[i], k * 40.0f / 36.0f, false, finger_on(s.targets[i], i));
}

static void draw_settings(void)
{
    sheet s;
    settings_sheet(&s);
    float k = s.k;
    rl_tp_rrect(&g.cv, 0, 0, (float)g.w, (float)g.h, 0, C(0, 0.55f), C(0, 0), 0, 1.0f);
    rl_tp_rrect(&g.cv, s.card.x0, s.card.y0, s.card.x1, s.card.y1, 36 * k, C(0x14161a, 1), C(0x555555, 1), 4 * k,
                1.0f);
    rl_tp_text(&g.cv, s.card.x0 + 40 * k, s.title_y, "On-screen controller", 50 * k, true, RL_TP_LEFT,
               C(0xffffff, 1), C(0, 0), 0);
    button(s.back, "Back", k, false, finger_on(T_BACK, 0));
    rl_tp_color blue = C(0x8ab4ff, 1), grey = C(0x9aa1ad, 1), white = C(0xffffff, 1);
    for (int i = 0; i < s.n; i++) {
        rect r = s.rows[i];
        bool down = finger_on(s.targets[i], i) && s.targets[i] != T_OPACITY && s.targets[i] != T_SIZE;
        rl_tp_rrect(&g.cv, r.x0, r.y0, r.x1, r.y1, 28 * k, C(down ? 0x2a2e36 : 0x1e2127, 1), C(0, 0), 0, 1.0f);
        float tx = r.x0 + 40 * k, ty = r.y0 + 50 * k, sy = r.y0 + 100 * k, rx = r.x1 - 40 * k;
        char v[32];
        switch (s.targets[i]) {
        case T_SHOW: {
            static const char *const words[3] = { "Automatic", "Always", "Never" };
            static const char *const subs[3] = { "Only when no controller is connected",
                                                 "Even when a controller is connected",
                                                 "Not shown; the back gesture opens the menu" };
            int m = show_mode();
            rl_tp_text(&g.cv, tx, ty, "Show", 40 * k, false, RL_TP_LEFT, white, C(0, 0), 0);
            rl_tp_text(&g.cv, tx, sy, subs[m], 30 * k, false, RL_TP_LEFT, grey, C(0, 0), 0);
            rl_tp_text(&g.cv, rx, ty, words[m], 34 * k, false, RL_TP_RIGHT, blue, C(0, 0), 0);
            break;
        }
        case T_OPACITY:
        case T_SIZE: {
            bool op = s.targets[i] == T_OPACITY;
            int64_t val = rl_setting_int(op ? "input.touch_pad.opacity" : "input.touch_pad.size");
            int lo = op ? 20 : 70, hi = op ? 100 : 150;
            rl_tp_text(&g.cv, tx, ty, op ? "Opacity" : "Button size", 40 * k, false, RL_TP_LEFT, white, C(0, 0), 0);
            rl_tp_text(&g.cv, tx, sy - 8 * k,
                       op ? "How see-through the buttons are" : "Bigger buttons are easier to hit", 30 * k, false,
                       RL_TP_LEFT, grey, C(0, 0), 0);
            snprintf(v, sizeof v, "%lld%%", (long long)val);
            rl_tp_text(&g.cv, rx, ty, v, 34 * k, false, RL_TP_RIGHT, blue, C(0, 0), 0);
            float x0, x1, y;
            slider_track(&s, i, &x0, &x1, &y);
            float t = (float)(val - lo) / (float)(hi - lo);
            float xx = x0 + (x1 - x0) * t;
            rl_tp_rrect(&g.cv, x0, y - 5 * k, x1, y + 5 * k, 5 * k, C(0x3a3f48, 1), C(0, 0), 0, 1.0f);
            rl_tp_rrect(&g.cv, x0, y - 5 * k, xx, y + 5 * k, 5 * k, blue, C(0, 0), 0, 1.0f);
            rl_tp_circle(&g.cv, xx, y, 24 * k, blue, C(0, 0), 0, 1.0f);
            break;
        }
        case T_HAPTICS: {
            bool on = rl_setting_bool("input.touch_pad.haptics");
            rl_tp_text(&g.cv, tx, ty, "Vibrate on press", 40 * k, false, RL_TP_LEFT, white, C(0, 0), 0);
            rl_tp_text(&g.cv, tx, sy, "A short tick when a button goes down", 30 * k, false, RL_TP_LEFT, grey,
                       C(0, 0), 0);
            float y = ty;
            rl_tp_rrect(&g.cv, rx - 100 * k, y - 28 * k, rx, y + 28 * k, 28 * k, on ? blue : C(0x3a3f48, 1),
                        C(0, 0), 0, 1.0f);
            rl_tp_circle(&g.cv, on ? rx - 28 * k : rx - 72 * k, y, 22 * k, white, C(0, 0), 0, 1.0f);
            break;
        }
        default:
            rl_tp_text(&g.cv, tx, ty, "Edit layout", 40 * k, false, RL_TP_LEFT, white, C(0, 0), 0);
            rl_tp_text(&g.cv, tx, sy, "Move and resize the buttons", 30 * k, false, RL_TP_LEFT, grey, C(0, 0), 0);
            rl_tp_text(&g.cv, rx, ty, ">", 44 * k, false, RL_TP_RIGHT, blue, C(0, 0), 0);
            break;
        }
    }
}

static void draw_edit(void)
{
    const rl_touchpad_layout *L = lay();
    edit_view v;
    edit_layout(&v);
    float k = v.k;
    rl_tp_rrect(&g.cv, 0, 0, (float)g.w, (float)g.h, 0, C(0, 0.25f), C(0, 0), 0, 1.0f);
    draw_pad(true);
    rl_tp_color amber = C(0xffc233, 1), teal = C(0x5eead4, 1);
    float lw = fmaxf(2.0f, dp(2.0f)), dot = dp(6.0f);
    for (uint32_t i = 0; i < L->ngroups && i < MAX_GROUPS; i++) {
        if (!v.gshown[i])
            continue;
        rect b = v.gbox[i];
        rl_tp_dashed_rect(&g.cv, b.x0, b.y0, b.x1, b.y1, lw, dp(6.0f), dp(4.0f), amber);
        rl_tp_circle(&g.cv, b.x1 - 4, b.y1 - 4, dot, amber, C(0, 0), 0, 1.0f);
        uint32_t attached = 0;
        for (uint32_t j = L->groups[i].first; j < L->groups[i].first + L->groups[i].count; j++)
            attached += !g.edit.d[j].detached;
        if (attached > 1) {
            float hx = (b.x0 + b.x1) / 2, hy = b.y0 - 30 * k;
            rl_tp_rrect(&g.cv, hx - 44 * k, hy - 24 * k, hx + 44 * k, hy + 24 * k, 24 * k, amber, C(0, 0.5f), 3 * k,
                        1.0f);
            float l[6] = { hx - 34 * k, hy, hx - 16 * k, hy - 13 * k, hx - 16 * k, hy + 13 * k };
            float r[6] = { hx + 34 * k, hy, hx + 16 * k, hy - 13 * k, hx + 16 * k, hy + 13 * k };
            rl_tp_triangle(&g.cv, l, C(0x222222, 1), C(0, 0), 0);
            rl_tp_triangle(&g.cv, r, C(0x222222, 1), C(0, 0), 0);
            rl_tp_line(&g.cv, hx - 14 * k, hy, hx + 14 * k, hy, 6 * k, C(0x222222, 1));
        }
    }
    for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++) {
        if (!g.edit.d[i].detached)
            continue;
        rect b = v.ibox[i];
        rl_tp_dashed_rect(&g.cv, b.x0, b.y0, b.x1, b.y1, lw, dp(6.0f), dp(4.0f), teal);
        rl_tp_circle(&g.cv, b.x1 - 4, b.y1 - 4, dot, teal, C(0, 0), 0, 1.0f);
        rect a = attach_rect(&v, i);
        rl_tp_rrect(&g.cv, a.x0, a.y0, a.x1, a.y1, 24 * k, C(0x1b1d22, 1), teal, 4 * k, 1.0f);
        rl_tp_text(&g.cv, (a.x0 + a.x1) / 2, (a.y0 + a.y1) / 2, "Attach", 26 * k, false, RL_TP_CENTER,
                   C(0xffffff, 1), C(0, 0), 0);
    }
    rl_tp_rrect(&g.cv, v.bar.x0, v.bar.y0, v.bar.x1, v.bar.y1, 50 * k, C(0x1b1d22, 1), C(0x666666, 1), 3 * k, 1.0f);
    rl_tp_text(&g.cv, v.bar.x0 + 40 * k, (v.bar.y0 + v.bar.y1) / 2,
               "Drag to move. Drag the dot to resize. Hold a button to detach it.", 30 * k, false, RL_TP_LEFT,
               C(0xffffff, 1), C(0, 0), 0);
    button(v.reset, "Reset", k * 30.0f / 36.0f, false, finger_on(T_RESET, 0));
    button(v.done, "Done", k * 30.0f / 36.0f, true, false);
}

static void draw_toast(void)
{
    if (!g.toast[0])
        return;
    uint64_t age = g.now - g.toast_at;
    float a = age > TOAST_NS - TOAST_FADE_NS ? (float)(TOAST_NS - age) / (float)TOAST_FADE_NS : 1.0f;
    a = a < 0 ? 0 : a;
    float k = ui_scale(1300, 1080);
    float w = rl_tp_text_width(g.toast, 36 * k, false) + 100 * k;
    float cx = (float)g.w / 2, cy = (float)g.h - (float)g.safe[3] - 110 * k;
    rl_tp_rrect(&g.cv, cx - w / 2, cy - 50 * k, cx + w / 2, cy + 50 * k, 50 * k, C(0x1b1d22, 0.96f),
                C(0x666666, 1), 3 * k, a);
    rl_tp_text(&g.cv, cx, cy, g.toast, 36 * k, false, RL_TP_CENTER, C(0xffffff, a), C(0, 0), 0);
}

const uint8_t *rl_touchpad_pixels(uint32_t *width, uint32_t *height, int32_t dirty[4])
{
    pthread_mutex_lock(&g.lock);
    dirty[0] = dirty[1] = dirty[2] = dirty[3] = 0;
    *width = g.w;
    *height = g.h;
    if (!g.guest || !g.w || !g.h) {
        pthread_mutex_unlock(&g.lock);
        return NULL;
    }
    if (g.cv.w != g.w || g.cv.h != g.h) {
        free(g.cv.px);
        g.cv.px = calloc((size_t)g.w * g.h, 4);
        g.cv.w = g.cv.px ? g.w : 0;
        g.cv.h = g.cv.px ? g.h : 0;
        g.cv.x0 = g.cv.y0 = g.cv.x1 = g.cv.y1 = 0;
        memset(g.drawn, 0, sizeof g.drawn);
        g.dirty = true;
    }
    if (!g.cv.px) {
        pthread_mutex_unlock(&g.lock);
        return NULL;
    }
    if (g.fade < 0)
        g.fade = active() ? 1.0f : 0.0f;
    if (g.dirty) {
        g.now = rl_clock_ns();
        int32_t old[4] = { g.cv.x0, g.cv.y0, g.cv.x1, g.cv.y1 };
        rl_tp_clear(&g.cv);
        if (g.ui == UI_EDIT) {
            draw_edit();
        } else {
            if (g.fade > 0)
                draw_pad(false);
            if (g.ui == UI_PAUSE)
                draw_pause();
            else if (g.ui == UI_SETTINGS)
                draw_settings();
        }
        draw_toast();
        int32_t x0 = old[0], y0 = old[1], x1 = old[2], y1 = old[3];
        if (x1 <= x0 || y1 <= y0) {
            x0 = g.cv.x0;
            y0 = g.cv.y0;
            x1 = g.cv.x1;
            y1 = g.cv.y1;
        } else if (g.cv.x1 > g.cv.x0 && g.cv.y1 > g.cv.y0) {
            x0 = x0 < g.cv.x0 ? x0 : g.cv.x0;
            y0 = y0 < g.cv.y0 ? y0 : g.cv.y0;
            x1 = x1 > g.cv.x1 ? x1 : g.cv.x1;
            y1 = y1 > g.cv.y1 ? y1 : g.cv.y1;
        }
        dirty[0] = x0;
        dirty[1] = y0;
        dirty[2] = x1 - x0;
        dirty[3] = y1 - y0;
        g.dirty = false;
    }
    bool any = g.cv.x1 > g.cv.x0 && g.cv.y1 > g.cv.y0;
    const uint8_t *px = any || dirty[2] > 0 ? g.cv.px : NULL;
    pthread_mutex_unlock(&g.lock);
    return px;
}

/* ---- declaration */

static void add_key(const char *key)
{
    key_slot **n = realloc(g.keys, (g.nkeys + 1) * sizeof *n);
    if (!n)
        return;
    g.keys = n;
    key_slot *k = calloc(1, sizeof *k);
    if (!k)
        return;
    snprintf(k->key, sizeof k->key, "%s", key);
    k->def.key = k->key;
    k->def.type = RL_SETTING_STRING;
    k->def.label = "Layout";
    k->def.description = "A place and size the layout editor saved";
    k->def.apply = RL_APPLY_LIVE;
    g.keys[g.nkeys++] = k;
}

void rl_touchpad_declare(const rl_touchpad_guest *gu)
{
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
    pthread_mutex_lock(&g.lock);
    if (g.guest == gu) {
        pthread_mutex_unlock(&g.lock);
        return;
    }
    g.guest = gu;
    memset(g.index, -1, sizeof g.index);
    memset(g.loaded, 0, sizeof g.loaded);
    size_t first = g.nkeys;
    char key[160];
    for (int o = 0; o < 2; o++) {
        const rl_touchpad_layout *L = layout_of(o);
        for (uint32_t i = 0; i < L->ngroups; i++) {
            group_key(key, sizeof key, o, L->groups[i].id);
            add_key(key);
        }
        for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
            if (detachable(&L->items[i])) {
                control_key(key, sizeof key, o, L->items[i].controls[0]);
                add_key(key);
            }
    }
    for (int o = 0; o < 2; o++) {
        const rl_touchpad_layout *L = layout_of(o);
        for (uint32_t i = 0; i < L->nitems && i < MAX_ITEMS; i++)
            for (int c = 0; c < 4; c++)
                g.index[o][i][c] = (int8_t)touch_index(L->items[i].controls[c]);
    }
    pthread_mutex_unlock(&g.lock);
    for (size_t i = first; i < g.nkeys; i++)
        rl_settings_declare(&g.keys[i]->def, 1);
}

const rl_touchpad_guest *rl_touchpad_current(void) { return g.guest; }

bool rl_touchpad_wanted(void) { return wanted(); }

void rl_touchpad_set_touch_screen(bool on) { g.touch_screen = on; }

void rl_touchpad_set_host(const rl_touchpad_host *h)
{
    pthread_mutex_lock(&g.lock);
    if (h)
        g.host = *h;
    else
        memset(&g.host, 0, sizeof g.host);
    pthread_mutex_unlock(&g.lock);
}

void rl_touchpad_reset(void)
{
    pthread_mutex_lock(&g.lock);
    free(g.cv.px);
    memset(&g.cv, 0, sizeof g.cv);
    g.guest = NULL;
    memset(&g.host, 0, sizeof g.host);
    g.w = g.h = 0;
    g.ppd = 1.0f;
    memset(g.safe, 0, sizeof g.safe);
    g.have_view = false;
    g.pads = 0;
    g.fade = -1.0f;
    g.now = g.last = 0;
    g.hidden = false;
    g.ui = UI_NONE;
    g.toast[0] = 0;
    memset(g.live, 0, sizeof g.live);
    memset(g.loaded, 0, sizeof g.loaded);
    memset(g.values, 0, sizeof g.values);
    memset(g.dirs, 0, sizeof g.dirs);
    memset(g.f, 0, sizeof g.f);
    g.dirty = g.tick = g.leave = false;
    pthread_cond_broadcast(&g.resumed);
    pthread_mutex_unlock(&g.lock);
}
