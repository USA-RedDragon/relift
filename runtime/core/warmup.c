/**
 * @file warmup.c
 * @brief The shader compile screen's model: the counters the compile queue feeds, the rate and time left they give,
 * the state machine from measuring to done, the input the screen takes and the overlay pixels (warmup.h). The
 * drawing is warmup_draw.c.
 */
#include <relift/warmup.h>

#include "core.h"
#include "warmup_int.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief The least time between two draws while the counts change (ns). */
#define REDRAW_NS (100ull * 1000000ull)
/** @brief The least time between two wakes of the host (ns). */
#define WAKE_NS (50ull * 1000000ull)

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    rl_warmup_host host;
    int phase, mode;
    uint32_t total, done, failed;
    uint64_t t0, tend, corner_since, last_wake, last_draw, drawn_sig, drawn_lay;
    bool dark, keyboard, dismissed, answered, wants_base, started;
    int answer;
    char name[256], gpu[256], report[1100];
    uint8_t *icon;
    uint32_t icon_w, icon_h, view_w, view_h;
    rl_wu_canvas cv_px;
    rl_wu_hits hits;
    bool drawn;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .dark = true };

static uint64_t (*g_clock)(void);

/** @brief Replaces the clock the model reads (tests). */
void rl_wu_test_clock(uint64_t (*clock)(void)) { g_clock = clock; }

static uint64_t now_ns(void) { return g_clock ? g_clock() : rl_clock_ns(); }

bool rl_wu_eta(uint32_t done, uint32_t total, uint64_t elapsed_ns, uint32_t *seconds)
{
    uint32_t need = total / 2 < RL_WU_RATE_SHADERS ? total / 2 : RL_WU_RATE_SHADERS;
    if (need < 1)
        need = 1;
    if (elapsed_ns < (uint64_t)RL_WU_RATE_SECONDS * 1000000000ull || done < need || done >= total)
        return false;
    /* (total - done) / (done / elapsed), in seconds, rounded up. */
    long double left = (long double)(total - done) * (long double)elapsed_ns / (long double)done / 1e9L;
    uint64_t s = (uint64_t)left;
    if ((long double)s < left)
        s++;
    if (s > 0xffffffffull)
        s = 0xffffffffull;
    *seconds = (uint32_t)s;
    return true;
}

void rl_wu_eta_text(uint32_t seconds, char *out, size_t n)
{
    if (seconds < 1)
        seconds = 1;
    if (seconds < 60) {
        snprintf(out, n, "About %u second%s left", seconds, seconds == 1 ? "" : "s");
        return;
    }
    uint32_t minutes = (seconds + 30) / 60;
    if (minutes < 120) {
        snprintf(out, n, "About %u minute%s left", minutes, minutes == 1 ? "" : "s");
        return;
    }
    uint32_t hours = (minutes + 30) / 60;
    snprintf(out, n, "About %u hours left", hours);
}

void rl_wu_time_text(uint64_t seconds, char *out, size_t n)
{
    if (seconds >= 3600)
        snprintf(out, n, "%llu:%02llu:%02llu", (unsigned long long)(seconds / 3600),
                 (unsigned long long)(seconds / 60 % 60), (unsigned long long)(seconds % 60));
    else
        snprintf(out, n, "%llu:%02llu", (unsigned long long)(seconds / 60), (unsigned long long)(seconds % 60));
}

void rl_wu_count_text(uint64_t v, char *out, size_t n)
{
    char digits[32];
    int k = snprintf(digits, sizeof digits, "%llu", (unsigned long long)v);
    size_t o = 0;
    for (int i = 0; i < k && o + 2 < n; i++) {
        if (i && (k - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = digits[i];
    }
    if (n)
        out[o < n ? o : n - 1] = 0;
}

uint32_t rl_wu_percent(uint32_t done, uint32_t total)
{
    if (!total)
        return 0;
    uint64_t p = (uint64_t)done * 100u / total;
    return p > 100 ? 100u : (uint32_t)p;
}

void rl_warmup_set_host(const rl_warmup_host *host)
{
    pthread_mutex_lock(&g.lock);
    if (host)
        g.host = *host;
    else
        memset(&g.host, 0, sizeof g.host);
    pthread_mutex_unlock(&g.lock);
}

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void rl_warmup_set_game(const char *name, const uint8_t *icon_rgba, uint32_t width, uint32_t height)
{
    uint8_t *icon = NULL;
    if (icon_rgba && width && height && width <= 1024 && height <= 1024) {
        icon = malloc((size_t)width * height * 4);
        if (icon)
            memcpy(icon, icon_rgba, (size_t)width * height * 4);
    }
    pthread_mutex_lock(&g.lock);
    copy(g.name, sizeof g.name, name);
    free(g.icon);
    g.icon = icon;
    g.icon_w = icon ? width : 0;
    g.icon_h = icon ? height : 0;
    g.drawn_sig = g.drawn_lay = 0;
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_set_gpu(const char *name)
{
    pthread_mutex_lock(&g.lock);
    copy(g.gpu, sizeof g.gpu, name);
    g.drawn_sig = g.drawn_lay = 0;
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_set_dark(bool dark)
{
    pthread_mutex_lock(&g.lock);
    g.dark = dark;
    g.drawn_sig = g.drawn_lay = 0;
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_set_keyboard(bool keyboard)
{
    pthread_mutex_lock(&g.lock);
    g.keyboard = keyboard;
    g.drawn_sig = g.drawn_lay = 0;
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_set_report(const char *path)
{
    pthread_mutex_lock(&g.lock);
    copy(g.report, sizeof g.report, path);
    g.drawn_sig = g.drawn_lay = 0;
    pthread_mutex_unlock(&g.lock);
}

/** @brief Asks the host to draw again, at most every WAKE_NS unless force is set (lock held; released around the call). */
static void wake(bool force)
{
    uint64_t t = now_ns();
    void (*fn)(void) = g.host.wake;
    if (!fn || (!force && t - g.last_wake < WAKE_NS))
        return;
    g.last_wake = t;
    pthread_mutex_unlock(&g.lock);
    fn();
    pthread_mutex_lock(&g.lock);
}

void rl_warmup_begin(uint32_t total, int mode)
{
    if (!total)
        return;
    pthread_mutex_lock(&g.lock);
    g.phase = RL_WARMUP_COMPILING;
    g.mode = mode == RL_WARMUP_BLOCKING ? RL_WARMUP_BLOCKING : RL_WARMUP_BACKGROUND;
    g.total = total;
    g.done = g.failed = 0;
    g.t0 = now_ns();
    g.tend = 0;
    g.corner_since = 0;
    g.dismissed = g.answered = g.started = false;
    g.answer = RL_WARMUP_START;
    g.drawn_sig = g.drawn_lay = 0;
    const char *theme = getenv("RELIFT_WARM_THEME");
    if (theme && !strcmp(theme, "light"))
        g.dark = false;
    else if (theme && !strcmp(theme, "dark"))
        g.dark = true;
    void (*base)(bool) = g.host.base;
    g.wants_base = true;
    pthread_mutex_unlock(&g.lock);
    if (base)
        base(true);
    pthread_mutex_lock(&g.lock);
    wake(true);
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_progress(uint32_t done, uint32_t failed)
{
    pthread_mutex_lock(&g.lock);
    if (g.phase == RL_WARMUP_COMPILING) {
        g.done = done > g.total ? g.total : done;
        g.failed = failed > g.done ? g.done : failed;
        wake(false);
    }
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_saving(void)
{
    pthread_mutex_lock(&g.lock);
    if (g.phase == RL_WARMUP_COMPILING) {
        g.phase = RL_WARMUP_SAVING;
        g.done = g.total;
        g.drawn_sig = g.drawn_lay = 0;
        wake(true);
    }
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_end(void)
{
    pthread_mutex_lock(&g.lock);
    if (g.phase == RL_WARMUP_COMPILING || g.phase == RL_WARMUP_SAVING) {
        g.phase = RL_WARMUP_FINISHED;
        g.done = g.total;
        g.tend = now_ns();
        /* A compile that ended before the corner card ever showed, with nothing to report, leaves nothing to say. */
        if (g.mode == RL_WARMUP_BACKGROUND && g.started && !g.corner_since && !g.failed)
            g.phase = RL_WARMUP_IDLE;
        g.drawn_sig = g.drawn_lay = 0;
        pthread_cond_broadcast(&g.cv);
        wake(true);
    }
    pthread_mutex_unlock(&g.lock);
}

void rl_warmup_game_started(void)
{
    pthread_mutex_lock(&g.lock);
    void (*base)(bool) = g.host.base;
    bool had = g.wants_base;
    g.started = true;
    if (g.mode == RL_WARMUP_BLOCKING && g.phase != RL_WARMUP_IDLE) {
        g.mode = RL_WARMUP_BACKGROUND;
        g.drawn_sig = g.drawn_lay = 0;
    }
    /* A compile that ended with nothing to report before anything showed leaves nothing to say. */
    if (g.phase == RL_WARMUP_FINISHED && !g.failed && !g.corner_since)
        g.phase = RL_WARMUP_IDLE;
    g.drawn_sig = g.drawn_lay = 0;
    g.wants_base = false;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.lock);
    if (had && base)
        base(false);
    pthread_mutex_lock(&g.lock);
    wake(true);
    pthread_mutex_unlock(&g.lock);
}

int rl_warmup_wait(void)
{
    pthread_mutex_lock(&g.lock);
    int answer = RL_WARMUP_START;
    while (g.mode == RL_WARMUP_BLOCKING && g.phase != RL_WARMUP_IDLE && g.answer != RL_WARMUP_QUIT) {
        if (g.phase == RL_WARMUP_FINISHED) {
            if (g.failed ? g.answered : now_ns() - g.tend >= RL_WU_DONE_NS)
                break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g.cv, &g.lock, &ts);
    }
    answer = g.answer;
    pthread_mutex_unlock(&g.lock);
    return answer;
}

bool rl_warmup_active(void)
{
    pthread_mutex_lock(&g.lock);
    bool on = g.phase != RL_WARMUP_IDLE;
    pthread_mutex_unlock(&g.lock);
    return on;
}

bool rl_warmup_blocking(void)
{
    pthread_mutex_lock(&g.lock);
    bool on = g.phase != RL_WARMUP_IDLE && g.mode == RL_WARMUP_BLOCKING;
    pthread_mutex_unlock(&g.lock);
    return on;
}

void rl_warmup_view(uint32_t width, uint32_t height)
{
    pthread_mutex_lock(&g.lock);
    if (g.view_w != width || g.view_h != height) {
        g.view_w = width;
        g.view_h = height;
        g.drawn_sig = g.drawn_lay = 0;
    }
    pthread_mutex_unlock(&g.lock);
}

/** @brief The state the counters are in (lock held). */
static int state_of(uint64_t now)
{
    switch (g.phase) {
    case RL_WARMUP_SAVING:
        return RL_WU_FINISHING;
    case RL_WARMUP_FINISHED:
        return g.failed ? RL_WU_ERROR : RL_WU_DONE;
    default: {
        uint32_t s;
        return rl_wu_eta(g.done, g.total, now - g.t0, &s) ? RL_WU_COMPILING : RL_WU_MEASURING;
    }
    }
}

/** @brief True when the screen shows something at now (lock held); a finished corner card of a good run times out. */
static bool visible(uint64_t now)
{
    if (g.phase == RL_WARMUP_IDLE || g.dismissed)
        return false;
    if (g.mode == RL_WARMUP_BACKGROUND && g.phase == RL_WARMUP_FINISHED && !g.failed && now - g.tend >= RL_WU_READY_NS)
        return false;
    return true;
}

/** @brief Fills the scene from the counters at now (lock held). */
static void scene_of(rl_wu_scene *s, uint64_t now)
{
    memset(s, 0, sizeof *s);
    s->mode = g.mode;
    s->state = state_of(now);
    s->total = g.total;
    s->done = g.done;
    s->failed = g.failed;
    uint64_t end = g.phase == RL_WARMUP_FINISHED ? g.tend : now;
    s->elapsed_ns = end > g.t0 ? end - g.t0 : 0;
    if (s->state == RL_WU_COMPILING)
        rl_wu_eta(g.done, g.total, s->elapsed_ns, &s->eta_s);
    s->dark = g.dark;
    s->fill = !g.started;
    s->starting = !g.started && g.mode == RL_WARMUP_BACKGROUND;
    s->keyboard = g.keyboard;
    s->note = g.mode == RL_WARMUP_BACKGROUND && g.corner_since && now - g.corner_since < RL_WU_NOTE_NS &&
              (s->state == RL_WU_MEASURING || s->state == RL_WU_COMPILING);
    copy(s->name, sizeof s->name, g.name);
    copy(s->gpu, sizeof s->gpu, g.gpu);
    copy(s->report, sizeof s->report, g.failed ? g.report : "");
    s->icon = g.icon;
    s->icon_w = g.icon_w;
    s->icon_h = g.icon_h;
}

/** @brief What changes what the screen looks like, folded into one number (lock held). */
static uint64_t signature(const rl_wu_scene *s)
{
    uint64_t h = 1469598103934665603ull;
    uint64_t v[] = { s->total, s->done, s->failed, s->elapsed_ns / 1000000000ull, s->eta_s };
    for (size_t i = 0; i < sizeof v / sizeof *v; i++)
        h = (h ^ v[i]) * 1099511628211ull;
    return h | 1;
}

/** @brief What changes the screen's layout rather than its numbers: a change redraws at once (lock held). */
static uint64_t layout_signature(const rl_wu_scene *s)
{
    return ((uint64_t)s->mode << 1 | (uint64_t)s->state << 4 | (uint64_t)s->note << 8 | (uint64_t)s->dark << 9 |
            (uint64_t)s->keyboard << 10 | (uint64_t)s->fill << 11 | (uint64_t)(s->icon != NULL) << 12) ^
           ((uint64_t)g.view_w << 20 | (uint64_t)g.view_h << 40);
}

const uint8_t *rl_warmup_pixels(uint64_t now_ns_, uint32_t *width, uint32_t *height, int32_t dirty[4])
{
    pthread_mutex_lock(&g.lock);
    dirty[0] = dirty[1] = dirty[2] = dirty[3] = 0;
    *width = g.view_w;
    *height = g.view_h;
    uint64_t now = g_clock ? g_clock() : now_ns_;
    if (!visible(now) || !g.view_w || !g.view_h) {
        if (g.phase == RL_WARMUP_FINISHED && g.mode == RL_WARMUP_BACKGROUND && !g.failed &&
            now - g.tend >= RL_WU_READY_NS)
            g.phase = RL_WARMUP_IDLE;
        g.drawn = false;
        g.drawn_sig = g.drawn_lay = 0;
        memset(&g.hits, 0, sizeof g.hits);
        pthread_mutex_unlock(&g.lock);
        return NULL;
    }
    if (g.mode == RL_WARMUP_BACKGROUND && !g.corner_since)
        g.corner_since = now;
    if (g.cv_px.w != g.view_w || g.cv_px.h != g.view_h) {
        free(g.cv_px.px);
        g.cv_px.px = calloc((size_t)g.view_w * g.view_h, 4);
        g.cv_px.w = g.cv_px.px ? g.view_w : 0;
        g.cv_px.h = g.cv_px.px ? g.view_h : 0;
        g.cv_px.x0 = g.cv_px.y0 = g.cv_px.x1 = g.cv_px.y1 = 0;
        g.drawn_sig = g.drawn_lay = 0;
    }
    if (!g.cv_px.px) {
        pthread_mutex_unlock(&g.lock);
        return NULL;
    }
    rl_wu_scene s;
    scene_of(&s, now);
    uint64_t sig = signature(&s), lay = layout_signature(&s);
    bool counts_only = g.drawn && lay == g.drawn_lay && now - g.last_draw < REDRAW_NS;
    if (!g.drawn || lay != g.drawn_lay || (sig != g.drawn_sig && !counts_only)) {
        int32_t old[4] = { g.cv_px.x0, g.cv_px.y0, g.cv_px.x1, g.cv_px.y1 };
        rl_wu_clear(&g.cv_px);
        rl_wu_draw(&g.cv_px, &s, &g.hits);
        int32_t x0 = old[0], y0 = old[1], x1 = old[2], y1 = old[3];
        if (g.cv_px.x1 > g.cv_px.x0 && g.cv_px.y1 > g.cv_px.y0) {
            if (x1 <= x0 || y1 <= y0) {
                x0 = g.cv_px.x0;
                y0 = g.cv_px.y0;
                x1 = g.cv_px.x1;
                y1 = g.cv_px.y1;
            } else {
                x0 = x0 < g.cv_px.x0 ? x0 : g.cv_px.x0;
                y0 = y0 < g.cv_px.y0 ? y0 : g.cv_px.y0;
                x1 = x1 > g.cv_px.x1 ? x1 : g.cv_px.x1;
                y1 = y1 > g.cv_px.y1 ? y1 : g.cv_px.y1;
            }
        }
        dirty[0] = x0;
        dirty[1] = y0;
        dirty[2] = x1 - x0;
        dirty[3] = y1 - y0;
        g.drawn = true;
        g.drawn_sig = sig;
        g.drawn_lay = lay;
        g.last_draw = now;
    }
    const uint8_t *px = g.cv_px.px;
    pthread_mutex_unlock(&g.lock);
    return px;
}

void rl_warmup_extent(int32_t out[4])
{
    pthread_mutex_lock(&g.lock);
    bool has = g.drawn && g.cv_px.x1 > g.cv_px.x0 && g.cv_px.y1 > g.cv_px.y0;
    out[0] = has ? g.cv_px.x0 : 0;
    out[1] = has ? g.cv_px.y0 : 0;
    out[2] = has ? g.cv_px.x1 : 0;
    out[3] = has ? g.cv_px.y1 : 0;
    pthread_mutex_unlock(&g.lock);
}

/** @brief True when (x, y) is inside the rectangle r (x, y, width, height). */
static bool inside(const float r[4], float x, float y)
{
    return r[2] > 0 && r[3] > 0 && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
}

/** @brief Ends the blocking wait with answer (lock held), and asks the host to quit for Quit. */
static void answer_now(int answer)
{
    g.answered = true;
    g.answer = answer;
    pthread_cond_broadcast(&g.cv);
    if (answer == RL_WARMUP_QUIT && g.host.quit) {
        void (*quit)(void) = g.host.quit;
        pthread_mutex_unlock(&g.lock);
        quit();
        pthread_mutex_lock(&g.lock);
    }
}

bool rl_warmup_key(int key)
{
    pthread_mutex_lock(&g.lock);
    bool took = false;
    uint64_t now = now_ns();
    if (visible(now) && g.mode == RL_WARMUP_BLOCKING) {
        took = true;
        bool saving = g.phase == RL_WARMUP_SAVING, finished = g.phase == RL_WARMUP_FINISHED;
        if (key == RL_WARMUP_KEY_ESCAPE && !saving && !(finished && !g.failed))
            answer_now(RL_WARMUP_QUIT);
        else if (key == RL_WARMUP_KEY_ENTER && finished && g.failed)
            answer_now(RL_WARMUP_START);
    } else if (visible(now) && g.phase == RL_WARMUP_FINISHED && g.failed && key == RL_WARMUP_KEY_ESCAPE) {
        g.dismissed = true;
        g.drawn_sig = g.drawn_lay = 0;
        took = true;
        wake(true);
    }
    pthread_mutex_unlock(&g.lock);
    return took;
}

bool rl_warmup_tap(float x, float y)
{
    pthread_mutex_lock(&g.lock);
    bool took = false;
    uint64_t now = now_ns();
    if (visible(now) && g.mode == RL_WARMUP_BLOCKING) {
        took = true;
        if (inside(g.hits.quit, x, y))
            answer_now(RL_WARMUP_QUIT);
        else if (inside(g.hits.start, x, y))
            answer_now(RL_WARMUP_START);
    } else if (visible(now) && g.phase == RL_WARMUP_FINISHED && g.failed && inside(g.hits.card, x, y)) {
        g.dismissed = true;
        g.drawn_sig = g.drawn_lay = 0;
        took = true;
        wake(true);
    }
    pthread_mutex_unlock(&g.lock);
    return took;
}

void rl_warmup_reset(void)
{
    pthread_mutex_lock(&g.lock);
    free(g.icon);
    free(g.cv_px.px);
    g.dark = true;
    g.keyboard = false;
    g.phase = RL_WARMUP_IDLE;
    g.mode = RL_WARMUP_BLOCKING;
    g.total = g.done = g.failed = 0;
    g.t0 = g.tend = g.corner_since = g.last_wake = g.last_draw = g.drawn_sig = g.drawn_lay = 0;
    g.dismissed = g.answered = g.wants_base = g.drawn = g.started = false;
    g.answer = RL_WARMUP_START;
    g.name[0] = g.gpu[0] = g.report[0] = 0;
    g.icon = NULL;
    g.icon_w = g.icon_h = g.view_w = g.view_h = 0;
    memset(&g.cv_px, 0, sizeof g.cv_px);
    memset(&g.hits, 0, sizeof g.hits);
    memset(&g.host, 0, sizeof g.host);
    g_clock = NULL;
    pthread_mutex_unlock(&g.lock);
}
