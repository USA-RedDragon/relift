/**
 * @file input_script.c
 * @brief The input script: a debug tool that holds port controls at given
 * guest frames, so that runs without a person at the controls are
 * reproducible.
 *
 * The script comes from RELIFT_INPUT_SCRIPT or rl_ports_set_script: comma
 * separated steps `<frame>[+<frames>]:<port>.<control>[=<value>]`. A
 * step holds the control at the value (default 1) from guest frame `frame`
 * for `frames` frames (default 6). The guest layer counts frames with
 * rl_ports_script_frame; while a step holds a control, the step's value
 * replaces what the bindings read.
 */
#include "core.h"

#include <relift/ports.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_STEPS_MAX 1024u
#define SCRIPT_HOLD_DEFAULT 6u
#define SCRIPT_ID_MAX 64u

typedef struct step {
    uint64_t start;
    uint64_t frames;
    float value;
    char port[SCRIPT_ID_MAX];
    char control[SCRIPT_ID_MAX];
} step;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static step *g_steps;
static uint32_t g_nsteps;
static bool g_loaded;
static uint64_t g_frame;

static bool parse_u64(const char **p, uint64_t *out)
{
    char *end;
    if (**p < '0' || **p > '9')
        return false;
    errno = 0;
    unsigned long long v = strtoull(*p, &end, 10);
    if (errno)
        return false;
    *out = v;
    *p = end;
    return true;
}

static bool parse_id(const char *s, size_t n, char *out)
{
    if (!n || n >= SCRIPT_ID_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return false;
    memcpy(out, s, n);
    out[n] = 0;
    return true;
}

static bool parse_step(const char *s, size_t len, step *out)
{
    char buf[256];
    while (len && *s == ' ') {
        s++;
        len--;
    }
    while (len && s[len - 1] == ' ')
        len--;
    if (!len || len >= sizeof buf)
        return false;
    memcpy(buf, s, len);
    buf[len] = 0;
    const char *p = buf;
    memset(out, 0, sizeof *out);
    out->frames = SCRIPT_HOLD_DEFAULT;
    out->value = 1.0f;
    if (!parse_u64(&p, &out->start))
        return false;
    if (*p == '+') {
        p++;
        if (!parse_u64(&p, &out->frames) || out->frames == 0)
            return false;
    }
    if (*p++ != ':')
        return false;
    const char *dot = strchr(p, '.');
    if (!dot || !parse_id(p, (size_t)(dot - p), out->port))
        return false;
    const char *ctl = dot + 1;
    const char *eq = strchr(ctl, '=');
    if (!parse_id(ctl, eq ? (size_t)(eq - ctl) : strlen(ctl), out->control))
        return false;
    if (eq) {
        char *end;
        double v = strtod(eq + 1, &end);
        if (end == eq + 1 || *end || !(v >= -1.0 && v <= 1.0))
            return false;
        out->value = (float)v;
    }
    return true;
}

static bool parse_script(const char *text, step **out, uint32_t *n)
{
    *out = NULL;
    *n = 0;
    if (!text)
        return true;
    step *steps = NULL;
    uint32_t count = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        bool blank = true;
        for (size_t i = 0; i < len; i++)
            if (p[i] != ' ')
                blank = false;
        if (!blank) {
            step st;
            step *grown = count < SCRIPT_STEPS_MAX && parse_step(p, len, &st)
                              ? realloc(steps, (count + 1) * sizeof *steps)
                              : NULL;
            if (!grown) {
                free(steps);
                return false;
            }
            steps = grown;
            steps[count++] = st;
        }
        if (!e)
            break;
        p = e + 1;
    }
    *out = steps;
    *n = count;
    return true;
}

bool rl_ports_valid_script(const char *text)
{
    step *steps;
    uint32_t n;
    if (!parse_script(text, &steps, &n))
        return false;
    free(steps);
    return true;
}

bool rl_ports_set_script(const char *text)
{
    step *steps;
    uint32_t n;
    if (!parse_script(text, &steps, &n)) {
        rl_log(RL_LOG_WARN, "input", "input script \"%s\" is not <frame>[+<frames>]:<port>.<control>[=<value>],...; "
                                     "ignored",
               text);
        return false;
    }
    pthread_mutex_lock(&g_lock);
    free(g_steps);
    g_steps = steps;
    g_nsteps = n;
    g_loaded = true;
    g_frame = 0;
    pthread_mutex_unlock(&g_lock);
    if (n)
        rl_log(RL_LOG_INFO, "input", "input script: %u steps", n);
    return true;
}

static void load_locked(void)
{
    if (g_loaded)
        return;
    g_loaded = true;
    const char *text = getenv("RELIFT_INPUT_SCRIPT");
    if (!text || !*text)
        return;
    pthread_mutex_unlock(&g_lock);
    rl_ports_set_script(text);
    pthread_mutex_lock(&g_lock);
}

void rl_ports_script_frame(uint64_t frame)
{
    pthread_mutex_lock(&g_lock);
    load_locked();
    g_frame = frame;
    for (uint32_t i = 0; i < g_nsteps; i++)
        if (g_steps[i].start == frame)
            rl_log(RL_LOG_INFO, "input", "input script: frame %llu: %s.%s = %g for %llu frames",
                   (unsigned long long)frame, g_steps[i].port, g_steps[i].control, (double)g_steps[i].value,
                   (unsigned long long)g_steps[i].frames);
    pthread_mutex_unlock(&g_lock);
}

bool rl_ports_script_value(const char *port, const char *control, float *value)
{
    bool held = false;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_nsteps; i++) {
        const step *s = &g_steps[i];
        if (g_frame >= s->start && g_frame - s->start < s->frames && !strcmp(s->port, port) &&
            !strcmp(s->control, control)) {
            *value = s->value;
            held = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return held;
}

void rl_ports_script_reset(void)
{
    pthread_mutex_lock(&g_lock);
    free(g_steps);
    g_steps = NULL;
    g_nsteps = 0;
    g_loaded = false;
    g_frame = 0;
    pthread_mutex_unlock(&g_lock);
}
