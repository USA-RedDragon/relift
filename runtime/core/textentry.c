/**
 * @file textentry.c
 * @brief Text entry (textentry.h): where the answer to a guest's request
 * for text comes from, and the panel's text and keys.
 */
#include "core.h"
#include "textentry_int.h"

#include <relift/settings.h>

#include <stdio.h>
#include <string.h>

/** @brief text_entry.panel choices. */
enum { PANEL_AUTO = 0, PANEL_ALWAYS = 1, PANEL_NEVER = 2 };

pthread_mutex_t rl_te_lock = PTHREAD_MUTEX_INITIALIZER;
rl_te_state rl_te;

static pthread_mutex_t g_done_lock = PTHREAD_MUTEX_INITIALIZER;
static const rl_text_entry_host *g_host;
static rl_text_entry_done_fn *g_done;
static void *g_user;

size_t rl_te_seq(unsigned char c) { return c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1; }

uint32_t rl_te_chars(const char *s)
{
    uint32_t n = 0;
    for (size_t i = 0; s[i]; i += rl_te_seq((unsigned char)s[i]))
        n++;
    return n;
}

void rl_text_entry_set_host(const rl_text_entry_host *host)
{
    pthread_mutex_lock(&rl_te_lock);
    g_host = host;
    pthread_mutex_unlock(&rl_te_lock);
}

/** @brief Copies at most max_chars code points of UTF-8 text into out (RL_TEXT_ENTRY_MAX bytes). */
static void clip(char *out, const char *text, uint32_t max_chars)
{
    size_t n = 0, chars = 0;
    while (text && text[n]) {
        size_t len = rl_te_seq((unsigned char)text[n]);
        if ((max_chars && chars == max_chars) || n + len >= RL_TEXT_ENTRY_MAX)
            break;
        n += len;
        chars++;
    }
    memcpy(out, text ? text : "", n);
    out[n] = 0;
}


/** @brief The answer without a panel: the text setting, else the guest's initial text, else the default setting. */
static void settings_answer(const rl_text_entry_args *a, char *out)
{
    const char *set = rl_setting_str("text_entry.text");
    const char *from = "text_entry.text";
    if (!set || !*set) {
        bool own = a->initial && *a->initial;
        set = own ? a->initial : rl_setting_str("text_entry.default");
        from = own ? "the game's initial text" : "text_entry.default";
    }
    clip(out, set, a->max_chars);
    rl_log(RL_LOG_INFO, "text", "text entry \"%s\": answered \"%s\" from %s", a->header ? a->header : "", out, from);
}

/** @brief Fills the state for args (lock held). */
static void fill(const rl_text_entry_args *a)
{
    memset(&rl_te, 0, sizeof rl_te);
    rl_te.open = true;
    char tmp[RL_TEXT_ENTRY_MAX];
    clip(tmp, a->header, 0);
    snprintf(rl_te.header, sizeof rl_te.header, "%s", tmp);
    clip(tmp, a->sub, 0);
    snprintf(rl_te.sub, sizeof rl_te.sub, "%s", tmp);
    clip(tmp, a->guide, 0);
    snprintf(rl_te.guide, sizeof rl_te.guide, "%s", tmp);
    clip(tmp, a->message, 0);
    snprintf(rl_te.message, sizeof rl_te.message, "%s", tmp);
    snprintf(rl_te.ok, sizeof rl_te.ok, "%s", a->ok_label && *a->ok_label ? a->ok_label : "OK");
    rl_te.max_chars = a->max_chars;
    rl_te.min_chars = a->min_chars;
    rl_te.can_cancel = a->can_cancel;
    rl_te.secret = a->secret;
    clip(rl_te.text, a->initial, a->max_chars);
    rl_te.caret = strlen(rl_te.text);
}

rl_text_entry_status rl_text_entry_open(const rl_text_entry_args *args, rl_text_entry_done_fn *done, void *user)
{
    pthread_mutex_lock(&rl_te_lock);
    if (g_done) {
        pthread_mutex_unlock(&rl_te_lock);
        return RL_TEXT_ENTRY_BUSY;
    }
    const char *set = rl_setting_str("text_entry.text");
    const rl_text_entry_host *host = g_host;
    uint32_t mode = rl_setting_enum("text_entry.panel");
    bool panel = host && host->show && !(set && *set) && mode != PANEL_NEVER &&
                 (mode == PANEL_ALWAYS || !rl_ports_script_in_use()) && rl_text_entry_can_draw();
    if (panel) {
        g_done = done;
        g_user = user;
        fill(args);
    }
    pthread_mutex_unlock(&rl_te_lock);
    if (panel && host->show()) {
        rl_log(RL_LOG_INFO, "text", "text entry \"%s\": the panel is shown", args->header ? args->header : "");
        return RL_TEXT_ENTRY_SHOWN;
    }
    if (panel) {
        pthread_mutex_lock(&rl_te_lock);
        g_done = NULL;
        g_user = NULL;
        rl_te.open = false;
        pthread_mutex_unlock(&rl_te_lock);
    }
    static char out[RL_TEXT_ENTRY_MAX];
    pthread_mutex_lock(&g_done_lock);
    settings_answer(args, out);
    done(user, true, out);
    pthread_mutex_unlock(&g_done_lock);
    return RL_TEXT_ENTRY_ANSWERED;
}

/** @brief Ends the open entry: the host hides the panel, then done gets the text. */
static void end(bool confirmed)
{
    static char out[RL_TEXT_ENTRY_MAX];
    pthread_mutex_lock(&g_done_lock);
    pthread_mutex_lock(&rl_te_lock);
    rl_text_entry_done_fn *done = g_done;
    void *user = g_user;
    const rl_text_entry_host *host = g_host;
    bool was = rl_te.open;
    memcpy(out, rl_te.text, sizeof out);
    rl_te.open = false;
    g_done = NULL;
    g_user = NULL;
    pthread_mutex_unlock(&rl_te_lock);
    if (was && host && host->hide)
        host->hide();
    if (was && done) {
        rl_log(RL_LOG_INFO, "text", "text entry: the player %s \"%s\"", confirmed ? "entered" : "canceled", out);
        done(user, confirmed, out);
    }
    pthread_mutex_unlock(&g_done_lock);
}

void rl_text_entry_close(void *user)
{
    pthread_mutex_lock(&g_done_lock);
    pthread_mutex_lock(&rl_te_lock);
    bool mine = g_done && g_user == user;
    const rl_text_entry_host *host = g_host;
    if (mine) {
        g_done = NULL;
        g_user = NULL;
        rl_te.open = false;
    }
    pthread_mutex_unlock(&rl_te_lock);
    pthread_mutex_unlock(&g_done_lock);
    if (mine && host && host->hide)
        host->hide();
}

bool rl_text_entry_active(void)
{
    pthread_mutex_lock(&rl_te_lock);
    bool on = rl_te.open;
    pthread_mutex_unlock(&rl_te_lock);
    return on;
}

static void changed(void)
{
    pthread_mutex_lock(&rl_te_lock);
    const rl_text_entry_host *host = g_host;
    rl_te.typed_ns = rl_clock_ns();
    pthread_mutex_unlock(&rl_te_lock);
    if (host && host->changed)
        host->changed();
}

void rl_text_entry_input(const char *text)
{
    pthread_mutex_lock(&rl_te_lock);
    if (!rl_te.open) {
        pthread_mutex_unlock(&rl_te_lock);
        return;
    }
    uint32_t have = rl_te_chars(rl_te.text);
    for (size_t i = 0; text[i];) {
        size_t len = rl_te_seq((unsigned char)text[i]);
        if (strnlen(text + i, len) < len)
            break;
        bool control = (unsigned char)text[i] < 0x20 || text[i] == 0x7f;
        size_t used = strlen(rl_te.text);
        if (!control && (!rl_te.max_chars || have < rl_te.max_chars) && used + len < RL_TEXT_ENTRY_MAX) {
            memmove(rl_te.text + rl_te.caret + len, rl_te.text + rl_te.caret, used - rl_te.caret + 1);
            memcpy(rl_te.text + rl_te.caret, text + i, len);
            rl_te.caret += len;
            have++;
        }
        i += len;
    }
    pthread_mutex_unlock(&rl_te_lock);
    changed();
}

/** @brief The start of the code point before byte at. */
static size_t back(size_t at)
{
    while (at > 0 && ((unsigned char)rl_te.text[--at] & 0xc0) == 0x80)
        ;
    return at;
}

void rl_text_entry_key(int key)
{
    pthread_mutex_lock(&rl_te_lock);
    if (!rl_te.open) {
        pthread_mutex_unlock(&rl_te_lock);
        return;
    }
    size_t used = strlen(rl_te.text), c = rl_te.caret;
    bool done = false, confirmed = false;
    switch (key) {
    case RL_TEXT_ENTRY_KEY_CONFIRM:
        done = confirmed = rl_te_chars(rl_te.text) >= rl_te.min_chars;
        break;
    case RL_TEXT_ENTRY_KEY_CANCEL:
        done = rl_te.can_cancel;
        break;
    case RL_TEXT_ENTRY_KEY_BACKSPACE:
        if (c) {
            size_t b = back(c);
            memmove(rl_te.text + b, rl_te.text + c, used - c + 1);
            rl_te.caret = b;
        }
        break;
    case RL_TEXT_ENTRY_KEY_DELETE:
        if (c < used) {
            size_t n = rl_te_seq((unsigned char)rl_te.text[c]);
            memmove(rl_te.text + c, rl_te.text + c + n, used - c - n + 1);
        }
        break;
    case RL_TEXT_ENTRY_KEY_LEFT:
        rl_te.caret = back(c);
        break;
    case RL_TEXT_ENTRY_KEY_RIGHT:
        if (c < used)
            rl_te.caret = c + rl_te_seq((unsigned char)rl_te.text[c]);
        break;
    case RL_TEXT_ENTRY_KEY_HOME:
        rl_te.caret = 0;
        break;
    case RL_TEXT_ENTRY_KEY_END:
        rl_te.caret = used;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&rl_te_lock);
    if (done)
        end(confirmed);
    else
        changed();
}

bool rl_text_entry_tap(float x, float y, uint32_t width, uint32_t height, bool touch)
{
    pthread_mutex_lock(&rl_te_lock);
    if (!rl_te.open) {
        pthread_mutex_unlock(&rl_te_lock);
        return false;
    }
    rl_te_place pl;
    rl_te_layout(width, height, touch, &pl);
    pthread_mutex_unlock(&rl_te_lock);
    static const int keys[RL_TE_BUTTONS] = { RL_TEXT_ENTRY_KEY_CONFIRM, RL_TEXT_ENTRY_KEY_CANCEL,
                                             RL_TEXT_ENTRY_KEY_BACKSPACE };
    for (int b = 0; b < RL_TE_BUTTONS; b++) {
        const float *r = pl.button[b];
        if (pl.has_button[b] && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3]) {
            rl_text_entry_key(keys[b]);
            return true;
        }
    }
    return false;
}

void rl_text_entry_field(uint32_t width, uint32_t height, bool touch, int32_t out[4])
{
    pthread_mutex_lock(&rl_te_lock);
    rl_te_place pl;
    rl_te_layout(width, height, touch, &pl);
    pthread_mutex_unlock(&rl_te_lock);
    for (int i = 0; i < 4; i++)
        out[i] = (int32_t)pl.field[i];
}

void rl_text_entry_reset(void)
{
    pthread_mutex_lock(&rl_te_lock);
    g_host = NULL;
    g_done = NULL;
    g_user = NULL;
    memset(&rl_te, 0, sizeof rl_te);
    pthread_mutex_unlock(&rl_te_lock);
}
