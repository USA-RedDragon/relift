/**
 * @file srv_am_swkbd.c
 * @brief The software keyboard library applet (applet 0x11) in its full
 * screen mode: nn::swkbd::ShowKeyboard.
 *
 * The application pushes three storages to the in channel: the common
 * arguments (0x20 bytes, whose library version picks the config layout),
 * the keyboard config, and the work buffer (a transfer memory storage) that
 * holds the initial text as UTF-16 at the config's offset. The applet asks
 * the runtime's text entry (relift/textentry.h) for the text and ends by
 * pushing one storage of 0x7d8 bytes to the out channel: a u32 close result
 * (0 entered, 1 canceled) and the text, NUL-terminated, UTF-8 when the config
 * asks for it, else UTF-16. With the config's text check on, the applet
 * first pushes the text to the interactive out channel (a u64 byte count and
 * the text) and waits for the application's verdict on the interactive in
 * channel (a u32 result: 0 accept, 1 refuse with a message, 2 ask to
 * confirm with a message, 3 refuse silently; then the message).
 *
 * Layouts. Up to library version 0x8000d (Switch 8.0.0) the config is a
 * 0x3d4-byte common part and a tail (Ryujinx and libnx name the fields).
 * Library version 0x160012 (SDK 22) sends 0x530 bytes: the texts keep their
 * offsets up to 0x3aa, the rest moves. The offsets below were read from the
 * SDK's nn::swkbd::ShowKeyboard, which copies each KeyboardConfig field to
 * its place in the storage.
 */
#include "srv_am_priv.h"

#include <relift/settings.h>
#include <relift/textentry.h>

#include <stdio.h>
#include <stdlib.h>

/** @brief Library version of SDK 22, whose config is the 0x530-byte layout. */
#define SWKBD_VERSION_SDK22 0x160012u
#define SWKBD_CONFIG_SDK22 0x530u
#define SWKBD_CONFIG_COMMON 0x3d4u
#define SWKBD_CONFIG_NEW 0x4c8u
/** @brief Size of the out and text check storages: a u32 or u64 head and the string buffer. */
#define SWKBD_REPLY_SIZE 0x7d8u
#define SWKBD_STRING_SIZE 0x7d4u
/** @brief Most characters the keyboard takes. */
#define SWKBD_MAX_CHARS 500u

enum { CLOSE_ENTERED = 0, CLOSE_CANCELED = 1 };
enum { CHECK_ACCEPT = 0, CHECK_REFUSE = 1, CHECK_CONFIRM = 2, CHECK_SILENT = 3 };

/** @brief The config fields the applet uses, from either layout. */
typedef struct swkbd_config {
    char ok[64];
    char header[256];
    char sub[512];
    char guide[1024];
    uint32_t max_chars, min_chars;
    uint32_t password;
    uint32_t initial_offset, initial_length;
    bool utf8;
    bool text_check;
    bool disable_cancel;
} swkbd_config;

typedef struct swkbd {
    am_applet *applet;
    swkbd_config cfg;
    char initial[RL_TEXT_ENTRY_MAX];
    char text[RL_TEXT_ENTRY_MAX];
    char message[1024];
    bool waiting_check;
    bool refused_once;
    bool panel;
} swkbd;

/** @brief Converts n UTF-16 units (stopping at a NUL) to UTF-8 in out of size bytes. */
static void utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t size)
{
    size_t o = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000)
            c = 0xfffd;
        uint8_t b[4];
        size_t len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (len == 1) {
            b[0] = (uint8_t)c;
        } else if (len == 2) {
            b[0] = (uint8_t)(0xc0 | (c >> 6));
            b[1] = (uint8_t)(0x80 | (c & 0x3f));
        } else if (len == 3) {
            b[0] = (uint8_t)(0xe0 | (c >> 12));
            b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
            b[2] = (uint8_t)(0x80 | (c & 0x3f));
        } else {
            b[0] = (uint8_t)(0xf0 | (c >> 18));
            b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
            b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
            b[3] = (uint8_t)(0x80 | (c & 0x3f));
        }
        if (o + len + 1 > size)
            break;
        memcpy(out + o, b, len);
        o += len;
    }
    out[o] = 0;
}

/** @brief Converts UTF-8 text to at most max UTF-16 units; returns the count. */
static size_t utf8_to_utf16(const char *s, uint16_t *out, size_t max)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o < max) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        c &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
        for (int k = 0; k < more && (*p & 0xc0) == 0x80; k++)
            c = (c << 6) | (*p++ & 0x3f);
        if (c >= 0x10000) {
            if (o + 2 > max)
                break;
            c -= 0x10000;
            out[o++] = (uint16_t)(0xd800 + (c >> 10));
            out[o++] = (uint16_t)(0xdc00 + (c & 0x3ff));
        } else {
            out[o++] = (uint16_t)c;
        }
    }
    return o;
}

static uint32_t rd32(const uint8_t *b, size_t off)
{
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

/** @brief A UTF-16 text field of n units at off, as UTF-8. */
static void field(const uint8_t *b, size_t off, size_t n, char *out, size_t size)
{
    uint16_t tmp[260];
    memcpy(tmp, b + off, n * 2);
    utf16_to_utf8(tmp, n, out, size);
}

/** @brief Reads the config of size bytes in the layout of library version; false when it is no known layout. */
static bool parse_config(const uint8_t *b, uint64_t size, uint32_t version, swkbd_config *c)
{
    memset(c, 0, sizeof *c);
    if (size < 0x3aa)
        return false;
    field(b, 0x04, 9, c->ok, sizeof c->ok);
    field(b, 0x24, 65, c->header, sizeof c->header);
    field(b, 0xa6, 129, c->sub, sizeof c->sub);
    field(b, 0x1a8, 257, c->guide, sizeof c->guide);
    if (version >= SWKBD_VERSION_SDK22) {
        if (size != SWKBD_CONFIG_SDK22)
            return false;
        c->utf8 = b[0x3aa] != 0;
        c->max_chars = rd32(b, 0x3ac);
        c->min_chars = rd32(b, 0x3b0);
        c->password = rd32(b, 0x3b4);
        c->initial_offset = rd32(b, 0x3bc);
        c->initial_length = rd32(b, 0x3c0);
        c->text_check = b[0x48d] != 0;
        c->disable_cancel = b[0x48e] != 0;
        return true;
    }
    if (size < SWKBD_CONFIG_COMMON)
        return false;
    c->max_chars = rd32(b, 0x3ac);
    c->min_chars = rd32(b, 0x3b0);
    c->password = rd32(b, 0x3b4);
    c->utf8 = b[0x3bd] != 0;
    c->initial_offset = rd32(b, 0x3c0);
    c->initial_length = rd32(b, 0x3c4);
    c->text_check = b[0x3d0] != 0;
    c->disable_cancel = size >= SWKBD_CONFIG_NEW && b[SWKBD_CONFIG_COMMON + 0xe1] != 0;
    return true;
}

/**
 * @brief Fills buf with a head of head_size bytes and, behind it, text in the config's encoding in a string buffer of
 * SWKBD_STRING_SIZE bytes; bytes gets the text's size with its NUL.
 */
static void reply(const swkbd *k, uint8_t *buf, const void *head, size_t head_size, const char *text, uint64_t *bytes)
{
    memset(buf, 0, head_size + SWKBD_STRING_SIZE);
    memcpy(buf, head, head_size);
    size_t room = SWKBD_STRING_SIZE;
    if (k->cfg.utf8) {
        size_t n = strlen(text);
        if (n + 1 > room)
            n = room - 1;
        memcpy(buf + head_size, text, n);
        *bytes = n + 1;
    } else {
        uint16_t u[SWKBD_STRING_SIZE / 2];
        size_t n = utf8_to_utf16(text, u, room / 2 - 1);
        memcpy(buf + head_size, u, n * 2);
        *bytes = (n + 1) * 2;
    }
}

static void finish(swkbd *k, uint32_t close, const char *text)
{
    uint8_t buf[SWKBD_REPLY_SIZE];
    uint64_t bytes;
    reply(k, buf, &close, 4, text, &bytes);
    rl_log(RL_LOG_INFO, "am", "software keyboard: %s \"%s\"", close == CLOSE_ENTERED ? "entered" : "canceled", text);
    am_applet_push_out(k->applet, buf, sizeof buf);
    am_applet_exit(k->applet, KR_SUCCESS);
}

static void ask(swkbd *k);

static void answered(void *user, bool confirmed, const char *text)
{
    swkbd *k = user;
    snprintf(k->text, sizeof k->text, "%s", text);
    if (!confirmed) {
        finish(k, CLOSE_CANCELED, k->text);
        return;
    }
    if (!k->cfg.text_check) {
        finish(k, CLOSE_ENTERED, k->text);
        return;
    }
    uint8_t buf[8 + SWKBD_STRING_SIZE];
    uint64_t bytes = 0;
    reply(k, buf, &bytes, 8, k->text, &bytes);
    memcpy(buf, &bytes, 8);
    k->waiting_check = true;
    am_applet_push_interactive_out(k->applet, buf, sizeof buf);
}

static void ask(swkbd *k)
{
    rl_text_entry_args args = {
        .header = k->cfg.header,
        .sub = k->cfg.sub,
        .guide = k->cfg.guide,
        .initial = k->initial,
        .ok_label = k->cfg.ok,
        .message = k->message[0] ? k->message : NULL,
        .max_chars = k->cfg.max_chars,
        .min_chars = k->cfg.min_chars,
        .can_cancel = !k->cfg.disable_cancel,
        .secret = k->cfg.password != 0,
    };
    rl_text_entry_status st = rl_text_entry_open(&args, answered, k);
    k->panel = st == RL_TEXT_ENTRY_SHOWN;
    if (st == RL_TEXT_ENTRY_BUSY) {
        rl_log(RL_LOG_WARN, "am", "software keyboard: another text entry is open; this one is canceled");
        finish(k, CLOSE_CANCELED, "");
    }
}

/** @brief Pops a storage and reads all of it into a new buffer; NULL when the channel is empty. */
static uint8_t *pop_bytes(am_applet *a, uint64_t *size, rl_ipc_object **keep)
{
    rl_ipc_object *o = am_applet_pop_in(a);
    if (!o)
        return NULL;
    *size = am_storage_size(o);
    uint8_t *b = malloc(*size ? *size : 1);
    if (b && !am_storage_read(o, 0, b, *size)) {
        free(b);
        b = NULL;
    }
    if (keep)
        *keep = o;
    else
        rl_ipc_object_unref(o);
    return b;
}

static void swkbd_start(am_applet *a)
{
    if (am_applet_mode(a) != 0) {
        rl_log(RL_LOG_WARN, "am", "software keyboard: the inline keyboard (mode %u) is not implemented; it ends as "
                                  "terminated",
               am_applet_mode(a));
        am_applet_exit(a, AM_LIBRARY_APPLET_TERMINATED);
        return;
    }
    swkbd *k = calloc(1, sizeof *k);
    if (!k) {
        am_applet_exit(a, AM_LIBRARY_APPLET_TERMINATED);
        return;
    }
    k->applet = a;
    am_applet_set_state(a, k);
    uint64_t common_size = 0, config_size = 0, work_size = 0;
    uint8_t *common = pop_bytes(a, &common_size, NULL);
    uint8_t *config = pop_bytes(a, &config_size, NULL);
    rl_ipc_object *work = NULL;
    rl_ipc_object *w = am_applet_pop_in(a);
    if (w) {
        work = w;
        work_size = am_storage_size(w);
    }
    uint32_t version = common && common_size >= 12 ? rd32(common, 8) : 0;
    bool ok = config && parse_config(config, config_size, version, &k->cfg);
    if (!ok) {
        rl_log(RL_LOG_WARN, "am", "software keyboard: a config of %llu bytes for library version 0x%x is no known "
                                  "layout; it ends as terminated",
               (unsigned long long)config_size, version);
    } else {
        if (!k->cfg.max_chars || k->cfg.max_chars > SWKBD_MAX_CHARS)
            k->cfg.max_chars = SWKBD_MAX_CHARS;
        if (k->cfg.min_chars > k->cfg.max_chars)
            k->cfg.min_chars = 0;
        uint32_t n = k->cfg.initial_length;
        if (work && n && n <= SWKBD_MAX_CHARS && k->cfg.initial_offset + (uint64_t)n * 2 <= work_size) {
            uint16_t u[SWKBD_MAX_CHARS];
            if (am_storage_read(work, k->cfg.initial_offset, u, (uint64_t)n * 2))
                utf16_to_utf8(u, n, k->initial, sizeof k->initial);
        }
        rl_log(RL_LOG_INFO, "am",
               "software keyboard: library version 0x%x, header \"%s\", sub \"%s\", guide \"%s\", initial \"%s\", %u "
               "to %u characters, %s, text check %s, cancel %s",
               version, k->cfg.header, k->cfg.sub, k->cfg.guide, k->initial, k->cfg.min_chars, k->cfg.max_chars,
               k->cfg.utf8 ? "UTF-8" : "UTF-16", k->cfg.text_check ? "on" : "off",
               k->cfg.disable_cancel ? "off" : "on");
    }
    free(common);
    free(config);
    if (work)
        rl_ipc_object_unref(work);
    if (!ok) {
        am_applet_exit(a, AM_LIBRARY_APPLET_TERMINATED);
        return;
    }
    ask(k);
}

static void swkbd_interactive(am_applet *a)
{
    swkbd *k = am_applet_state(a);
    rl_ipc_object *o = am_applet_pop_interactive_in(a);
    if (!k || !o || !k->waiting_check) {
        if (o)
            rl_ipc_object_unref(o);
        return;
    }
    uint8_t buf[SWKBD_REPLY_SIZE];
    memset(buf, 0, sizeof buf);
    uint64_t size = am_storage_size(o);
    am_storage_read(o, 0, buf, size < sizeof buf ? size : sizeof buf);
    rl_ipc_object_unref(o);
    k->waiting_check = false;
    uint32_t verdict = rd32(buf, 0);
    if (k->cfg.utf8) {
        size_t n = strnlen((const char *)buf + 4, SWKBD_STRING_SIZE);
        if (n >= sizeof k->message)
            n = sizeof k->message - 1;
        memcpy(k->message, buf + 4, n);
        k->message[n] = 0;
    } else {
        uint16_t u[SWKBD_STRING_SIZE / 2];
        memcpy(u, buf + 4, sizeof u);
        utf16_to_utf8(u, SWKBD_STRING_SIZE / 2, k->message, sizeof k->message);
    }
    rl_log(RL_LOG_INFO, "am", "software keyboard: text check %u, message \"%s\"", verdict, k->message);
    if (verdict == CHECK_ACCEPT) {
        finish(k, CLOSE_ENTERED, k->text);
        return;
    }
    if (verdict == CHECK_SILENT)
        k->message[0] = 0;
    if (!k->panel && k->refused_once) {
        finish(k, verdict == CHECK_CONFIRM ? CLOSE_ENTERED : CLOSE_CANCELED, k->text);
        return;
    }
    k->refused_once = true;
    snprintf(k->initial, sizeof k->initial, "%s", k->text);
    ask(k);
}

static void swkbd_stop(am_applet *a)
{
    swkbd *k = am_applet_state(a);
    if (k)
        rl_text_entry_close(k);
}

static void swkbd_destroy(am_applet *a)
{
    free(am_applet_state(a));
    am_applet_set_state(a, NULL);
}

const am_applet_impl am_swkbd_applet = {
    "software keyboard", swkbd_start, swkbd_interactive, swkbd_stop, swkbd_destroy,
};
