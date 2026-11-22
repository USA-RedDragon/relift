/**
 * @file textentry.c
 * @brief Text entry: where the answer to a guest's request for text comes
 * from (textentry.h). A host that can show a panel registers itself; the
 * module keeps the one open entry and hands the host's answer to the guest.
 */
#include "core.h"

#include <relift/settings.h>
#include <relift/textentry.h>

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_done_lock = PTHREAD_MUTEX_INITIALIZER;
static const rl_text_entry_host *g_host;
static rl_text_entry_done_fn *g_done;
static void *g_user;

void rl_text_entry_set_host(const rl_text_entry_host *host)
{
    pthread_mutex_lock(&g_lock);
    g_host = host;
    pthread_mutex_unlock(&g_lock);
}

/** @brief Copies at most max_chars code points of UTF-8 text into out (RL_TEXT_ENTRY_MAX bytes). */
static void clip(char *out, const char *text, uint32_t max_chars)
{
    size_t n = 0, chars = 0;
    while (text && text[n] && n + 1 < RL_TEXT_ENTRY_MAX) {
        size_t len = 1;
        unsigned char c = (unsigned char)text[n];
        if (c >= 0xf0)
            len = 4;
        else if (c >= 0xe0)
            len = 3;
        else if (c >= 0xc0)
            len = 2;
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
        set = a->initial && *a->initial ? a->initial : rl_setting_str("text_entry.default");
        from = a->initial && *a->initial ? "the game's initial text" : "text_entry.default";
    }
    clip(out, set, a->max_chars);
    rl_log(RL_LOG_INFO, "text", "text entry \"%s\": answered \"%s\" from %s", a->header ? a->header : "", out, from);
}

rl_text_entry_status rl_text_entry_open(const rl_text_entry_args *args, rl_text_entry_done_fn *done, void *user)
{
    pthread_mutex_lock(&g_lock);
    if (g_done) {
        pthread_mutex_unlock(&g_lock);
        return RL_TEXT_ENTRY_BUSY;
    }
    const char *set = rl_setting_str("text_entry.text");
    const rl_text_entry_host *host = g_host;
    bool panel = host && host->show && !(set && *set) && !rl_ports_script_in_use();
    if (panel) {
        g_done = done;
        g_user = user;
    }
    pthread_mutex_unlock(&g_lock);
    if (panel && host->show(args)) {
        rl_log(RL_LOG_INFO, "text", "text entry \"%s\": the panel is shown", args->header ? args->header : "");
        return RL_TEXT_ENTRY_SHOWN;
    }
    if (panel) {
        pthread_mutex_lock(&g_lock);
        g_done = NULL;
        g_user = NULL;
        pthread_mutex_unlock(&g_lock);
    }
    static char out[RL_TEXT_ENTRY_MAX];
    pthread_mutex_lock(&g_done_lock);
    settings_answer(args, out);
    done(user, true, out);
    pthread_mutex_unlock(&g_done_lock);
    return RL_TEXT_ENTRY_ANSWERED;
}

void rl_text_entry_end(bool confirmed, const char *text)
{
    pthread_mutex_lock(&g_done_lock);
    pthread_mutex_lock(&g_lock);
    rl_text_entry_done_fn *done = g_done;
    void *user = g_user;
    g_done = NULL;
    g_user = NULL;
    pthread_mutex_unlock(&g_lock);
    if (done)
        done(user, confirmed, text ? text : "");
    pthread_mutex_unlock(&g_done_lock);
}

void rl_text_entry_close(void *user)
{
    pthread_mutex_lock(&g_done_lock);
    pthread_mutex_lock(&g_lock);
    bool mine = g_done && g_user == user;
    const rl_text_entry_host *host = g_host;
    if (mine) {
        g_done = NULL;
        g_user = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_done_lock);
    if (mine && host && host->hide)
        host->hide();
}

void rl_text_entry_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_host = NULL;
    g_done = NULL;
    g_user = NULL;
    pthread_mutex_unlock(&g_lock);
}
