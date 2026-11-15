/**
 * @file ports.c
 * @brief Declared ports, bindings and control values.
 */
#include <relift/override.h>
#include <relift/ports.h>

#include <relift/input_names.h>
#include <relift/settings.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BINDINGS 8

enum { DEV_PAD, DEV_AXIS, DEV_KEY, DEV_MOUSE };
enum { HALF_FULL, HALF_POS, HALF_NEG };

typedef struct binding {
    uint8_t dev;
    uint8_t half;
    int8_t pad;
    uint16_t value;
} binding;

typedef struct control {
    char *id;
    uint8_t kind;
    char *defaults;
    binding b[MAX_BINDINGS];
    uint8_t nb;
} control;

typedef struct port {
    char *id;
    char *kind;
    control *c;
    uint32_t n;
    int pad;
} port;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static port *g_ports;
static uint32_t g_nports;

static bool lookup(const char *dev, size_t dlen, const char *name, size_t nlen, binding *out)
{
    static const struct {
        const char *kind;
        const char *name;
        unsigned value;
    } table[] = {
#define X(kind, name, value) { #kind, name, value },
        RL_INPUT_NAMES(X)
#undef X
    };
    const char *want = NULL;
    if (dlen >= 7 && !strncmp(dev, "gamepad", 7))
        want = "GAMEPAD";
    else if (dlen == 3 && !strncmp(dev, "key", 3))
        want = "KEY";
    else if (dlen == 5 && !strncmp(dev, "mouse", 5))
        want = "MOUSE";
    if (!want)
        return false;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        bool match = strlen(table[i].name) == nlen && !strncmp(table[i].name, name, nlen);
        if (!match)
            continue;
        if (!strcmp(table[i].kind, want)) {
            out->dev = !strcmp(want, "GAMEPAD") ? DEV_PAD : !strcmp(want, "KEY") ? DEV_KEY : DEV_MOUSE;
            out->value = (uint16_t)table[i].value;
            return true;
        }
        if (!strcmp(want, "GAMEPAD") && !strcmp(table[i].kind, "AXIS")) {
            out->dev = DEV_AXIS;
            out->value = (uint16_t)table[i].value;
            return true;
        }
    }
    return false;
}

static bool parse_one(const char *s, size_t len, binding *out)
{
    while (len && *s == ' ') {
        s++;
        len--;
    }
    while (len && s[len - 1] == ' ')
        len--;
    const char *colon = memchr(s, ':', len);
    if (!colon)
        return false;
    size_t dlen = (size_t)(colon - s);
    const char *name = colon + 1;
    size_t nlen = len - dlen - 1;
    memset(out, 0, sizeof *out);
    out->pad = -1;
    if (nlen && name[nlen - 1] == '+') {
        out->half = HALF_POS;
        nlen--;
    } else if (nlen && name[nlen - 1] == '-') {
        out->half = HALF_NEG;
        nlen--;
    }
    if (dlen > 7 && !strncmp(s, "gamepad", 7)) {
        if (dlen != 8 || s[7] < '1' || s[7] > '8')
            return false;
        out->pad = (int8_t)(s[7] - '1');
    }
    return lookup(s, dlen, name, nlen, out);
}

static bool parse_list(const char *text, binding *b, uint8_t *n)
{
    *n = 0;
    if (!text)
        return true;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) {
            if (*n == MAX_BINDINGS || !parse_one(p, len, &b[*n]))
                return false;
            (*n)++;
        }
        if (!e)
            break;
        p = e + 1;
    }
    return true;
}

static int pad_of(const char *id)
{
    if (id[0] == 'p' && id[1] >= '1' && id[1] <= '8' && !id[2])
        return id[1] - '1';
    return 0;
}

static port *find_port(const char *id)
{
    for (uint32_t i = 0; i < g_nports; i++)
        if (!strcmp(g_ports[i].id, id))
            return &g_ports[i];
    return NULL;
}

static bool find_in(const port *p, const char *id, uint32_t *idx)
{
    for (uint32_t i = 0; i < p->n; i++)
        if (!strcmp(p->c[i].id, id)) {
            *idx = i;
            return true;
        }
    return false;
}

static char *dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r)
        memcpy(r, s, n);
    return r;
}

static void free_port(port *p)
{
    for (uint32_t i = 0; i < p->n; i++) {
        free(p->c[i].id);
        free(p->c[i].defaults);
    }
    free(p->c);
    free(p->id);
    free(p->kind);
}

static bool validate(const rl_port_decl *d, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (!d[i].id || !d[i].kind)
            return false;
        const port *ex = find_port(d[i].id);
        if (ex && strcmp(ex->kind, d[i].kind))
            return false;
        for (uint32_t j = 0; j < d[i].count; j++) {
            const rl_control_decl *c = &d[i].controls[j];
            binding b[MAX_BINDINGS];
            uint8_t n;
            uint32_t at;
            if (!c->id || c->kind > RL_CONTROL_AXIS || !parse_list(c->defaults, b, &n))
                return false;
            if (ex && find_in(ex, c->id, &at))
                return false;
            for (uint32_t k = 0; k < j; k++)
                if (!strcmp(d[i].controls[k].id, c->id))
                    return false;
            for (uint32_t k = 0; k < i; k++)
                if (!strcmp(d[k].id, d[i].id)) {
                    for (uint32_t m = 0; m < d[k].count; m++)
                        if (!strcmp(d[k].controls[m].id, c->id))
                            return false;
                }
        }
    }
    return true;
}

typedef struct setting_slot {
    char key[96];
    rl_setting_def def;
    char defaults[256];
    struct setting_slot *next;
} setting_slot;

static setting_slot *g_slots;

static void declare_setting(const port *p, const control *c)
{
    char key[96];
    if (snprintf(key, sizeof key, "bind.%s.%s", p->id, c->id) >= (int)sizeof key)
        return;
    setting_slot *t = g_slots;
    while (t && strcmp(t->key, key))
        t = t->next;
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t)
            return;
        memcpy(t->key, key, sizeof key);
        t->def.key = t->key;
        t->def.type = RL_SETTING_BINDING;
        t->def.label = "Binding";
        t->def.description = "";
        if (c->defaults && strlen(c->defaults) < sizeof t->defaults) {
            strcpy(t->defaults, c->defaults);
            t->def.def_str = t->defaults;
        }
        t->next = g_slots;
        g_slots = t;
    }
    rl_settings_declare(&t->def, 1);
}

static void apply_settings(const port *p, const control *c)
{
    char key[96];
    if (snprintf(key, sizeof key, "bind.%s.%s", p->id, c->id) >= (int)sizeof key)
        return;
    const char *v = rl_setting_binding(key);
    if (v && *v) {
        binding b[MAX_BINDINGS];
        uint8_t n;
        if (parse_list(v, b, &n)) {
            control *w = (control *)c;
            memcpy(w->b, b, sizeof b);
            w->nb = n;
        }
    }
}

bool rl_ports_declare(const rl_port_decl *ports, uint32_t count)
{
    pthread_mutex_lock(&g_lock);
    bool ok = validate(ports, count);
    for (uint32_t i = 0; ok && i < count; i++) {
        const rl_port_decl *d = &ports[i];
        port *p = find_port(d->id);
        if (!p) {
            port *np = realloc(g_ports, (g_nports + 1) * sizeof *np);
            if (!np) {
                ok = false;
                break;
            }
            g_ports = np;
            p = &g_ports[g_nports++];
            memset(p, 0, sizeof *p);
            p->id = dup(d->id);
            p->kind = dup(d->kind);
            p->pad = pad_of(d->id);
        }
        control *nc = realloc(p->c, (p->n + d->count) * sizeof *nc);
        if (!nc && d->count) {
            ok = false;
            break;
        }
        p->c = nc;
        for (uint32_t j = 0; j < d->count; j++) {
            control *c = &p->c[p->n++];
            memset(c, 0, sizeof *c);
            c->id = dup(d->controls[j].id);
            c->kind = d->controls[j].kind;
            c->defaults = d->controls[j].defaults ? dup(d->controls[j].defaults) : NULL;
            parse_list(c->defaults, c->b, &c->nb);
            declare_setting(p, c);
            apply_settings(p, c);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void rl_ports_reset(void)
{
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_nports; i++)
        free_port(&g_ports[i]);
    free(g_ports);
    g_ports = NULL;
    g_nports = 0;
    pthread_mutex_unlock(&g_lock);
}

uint32_t rl_ports_count(void)
{
    pthread_mutex_lock(&g_lock);
    uint32_t n = g_nports;
    pthread_mutex_unlock(&g_lock);
    return n;
}

const char *rl_ports_id(uint32_t index)
{
    pthread_mutex_lock(&g_lock);
    const char *r = index < g_nports ? g_ports[index].id : NULL;
    pthread_mutex_unlock(&g_lock);
    return r;
}

uint32_t rl_ports_control_count(const char *port_id)
{
    pthread_mutex_lock(&g_lock);
    const port *p = find_port(port_id);
    uint32_t n = p ? p->n : 0;
    pthread_mutex_unlock(&g_lock);
    return n;
}

rl_control_ref rl_ports_find(const char *port_id, const char *control_id)
{
    rl_control_ref r = RL_PORT_NONE;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_nports; i++) {
        uint32_t c;
        if (!strcmp(g_ports[i].id, port_id) && find_in(&g_ports[i], control_id, &c)) {
            r = (i << 16) | c;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

bool rl_ports_bind(const char *port_id, const char *control_id, const char *text)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    port *p = find_port(port_id);
    uint32_t c;
    if (p && find_in(p, control_id, &c)) {
        binding b[MAX_BINDINGS];
        uint8_t n;
        if (parse_list(text, b, &n)) {
            memcpy(p->c[c].b, b, sizeof b);
            p->c[c].nb = n;
            ok = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void rl_ports_bind_defaults(void)
{
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_nports; i++)
        for (uint32_t j = 0; j < g_ports[i].n; j++) {
            control *c = &g_ports[i].c[j];
            parse_list(c->defaults, c->b, &c->nb);
        }
    pthread_mutex_unlock(&g_lock);
}

bool rl_ports_apply_setting(const char *key, const char *value)
{
    if (strncmp(key, "bind.", 5))
        return false;
    const char *dot = strchr(key + 5, '.');
    if (!dot || dot == key + 5 || !dot[1])
        return false;
    char port_id[64];
    size_t n = (size_t)(dot - (key + 5));
    if (n >= sizeof port_id)
        return false;
    memcpy(port_id, key + 5, n);
    port_id[n] = 0;
    return rl_ports_bind(port_id, dot + 1, value);
}

static float one(const binding *b, int port_pad, const rl_input_snapshot *s)
{
    float v = 0.0f;
    int pad = b->pad >= 0 ? b->pad : port_pad;
    switch (b->dev) {
    case DEV_PAD:
        if (pad < RL_INPUT_PADS && s->pads[pad].connected && (s->pads[pad].buttons >> b->value & 1u))
            v = 1.0f;
        break;
    case DEV_AXIS: {
        if (pad >= RL_INPUT_PADS || !s->pads[pad].connected)
            break;
        const rl_input_pad *p = &s->pads[pad];
        int16_t raw = b->value == 0 ? p->lx : b->value == 1 ? p->ly : b->value == 2 ? p->rx : p->ry;
        v = (raw < -32767 ? -32767 : raw) / 32767.0f;
        break;
    }
    case DEV_KEY:
        if (b->value < 256 && (s->keys[b->value / 64] >> (b->value % 64) & 1u))
            v = 1.0f;
        break;
    default:
        if (s->pointer.buttons >> b->value & 1u)
            v = 1.0f;
        break;
    }
    if (b->half == HALF_POS)
        return v > 0.0f ? v : 0.0f;
    if (b->half == HALF_NEG)
        return b->dev == DEV_AXIS ? (v < 0.0f ? v : 0.0f) : -v;
    return v;
}

static float strongest(const binding *b, uint8_t n, int pad, uint8_t kind, const rl_input_snapshot *s)
{
    float best = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        float v = one(&b[i], pad, s);
        if ((v < 0 ? -v : v) > (best < 0 ? -best : best))
            best = v;
    }
    if (kind == RL_CONTROL_BUTTON)
        best = best > 0.5f || best < -0.5f ? 1.0f : 0.0f;
    else if (kind == RL_CONTROL_TRIGGER && best < 0.0f)
        best = 0.0f;
    return best;
}

bool rl_ports_valid_binding(const char *text)
{
    binding b[MAX_BINDINGS];
    uint8_t n;
    return text && parse_list(text, b, &n);
}

float rl_ports_read_binding(const char *text, int pad)
{
    binding b[MAX_BINDINGS];
    uint8_t n;
    if (!text || !parse_list(text, b, &n))
        return 0.0f;
    rl_input_snapshot s;
    rl_input_sample(&s);
    return strongest(b, n, pad, RL_CONTROL_AXIS, &s);
}

float rl_ports_eval(rl_control_ref ref, const rl_input_snapshot *s)
{
    if (ref == RL_PORT_NONE)
        return 0.0f;
    float best = 0.0f;
    pthread_mutex_lock(&g_lock);
    uint32_t pi = ref >> 16, ci = ref & 0xffffu;
    if (pi < g_nports && ci < g_ports[pi].n) {
        const port *p = &g_ports[pi];
        const control *c = &p->c[ci];
        best = strongest(c->b, c->nb, p->pad, c->kind, s);
        float held;
        if (rl_ports_script_value(p->id, c->id, &held))
            best = c->kind == RL_CONTROL_BUTTON ? (held > 0.5f || held < -0.5f ? 1.0f : 0.0f)
                   : c->kind == RL_CONTROL_TRIGGER && held < 0.0f ? 0.0f
                                                                  : held;
    }
    pthread_mutex_unlock(&g_lock);
    return best;
}

float rl_ports_read(rl_control_ref ref)
{
    if (ref == RL_PORT_NONE)
        return 0.0f;
    rl_input_snapshot s;
    rl_input_sample(&s);
    return rl_ports_eval(ref, &s);
}

float relift_input(const char *port_id, const char *control_id)
{
    return rl_ports_read(rl_ports_find(port_id, control_id));
}
