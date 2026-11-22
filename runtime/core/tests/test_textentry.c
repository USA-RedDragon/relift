/**
 * @file test_textentry.c
 * @brief Text entry: answers from the settings, the panel's text editing,
 * limits and keys, the taps on its buttons, and the TrueType reader on a
 * font the test builds (one square glyph for 'A' and one for U+540D).
 */
#include "runner.h"

#include <relift/settings.h>
#include <relift/textentry.h>

#include "textentry_int.h"
#include "ttf.h"

#include <stdlib.h>
#include <string.h>

void rl_settings_declare_runtime(void);

typedef struct buf {
    uint8_t d[1024];
    size_t n;
} buf;

static void be16(buf *b, uint32_t v)
{
    b->d[b->n++] = (uint8_t)(v >> 8);
    b->d[b->n++] = (uint8_t)v;
}

static void be32(buf *b, uint32_t v)
{
    be16(b, v >> 16);
    be16(b, v & 0xffff);
}

/** @brief A TrueType font of 1000 units per em: glyph 1 is a square from (100, 0) to (500, 400), advance 600. */
static size_t make_font(uint8_t *out)
{
    buf t[7];
    memset(t, 0, sizeof t);
    static const char tags[7][5] = { "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };
    buf *cmap = &t[0], *glyf = &t[1], *head = &t[2], *hhea = &t[3], *hmtx = &t[4], *loca = &t[5], *maxp = &t[6];
    be16(cmap, 0);
    be16(cmap, 1);
    be16(cmap, 3);
    be16(cmap, 1);
    be32(cmap, 12);
    be16(cmap, 4);
    be16(cmap, 40);
    be16(cmap, 0);
    be16(cmap, 6);
    be16(cmap, 4);
    be16(cmap, 1);
    be16(cmap, 2);
    be16(cmap, 'A');
    be16(cmap, 0x540d);
    be16(cmap, 0xffff);
    be16(cmap, 0);
    be16(cmap, 'A');
    be16(cmap, 0x540d);
    be16(cmap, 0xffff);
    be16(cmap, (uint16_t)(1 - 'A'));
    be16(cmap, (uint16_t)(1 - 0x540d));
    be16(cmap, 1);
    be16(cmap, 0);
    be16(cmap, 0);
    be16(cmap, 0);
    be16(glyf, 1);
    be16(glyf, 100);
    be16(glyf, 0);
    be16(glyf, 500);
    be16(glyf, 400);
    be16(glyf, 3);
    be16(glyf, 0);
    static const uint8_t flags[4] = { 1, 1, 1, 1 };
    memcpy(glyf->d + glyf->n, flags, 4);
    glyf->n += 4;
    static const int16_t xs[4] = { 100, 0, 400, 0 }, ys[4] = { 0, 400, 0, -400 };
    for (int i = 0; i < 4; i++)
        be16(glyf, (uint16_t)xs[i]);
    for (int i = 0; i < 4; i++)
        be16(glyf, (uint16_t)ys[i]);
    if (glyf->n & 1)
        glyf->d[glyf->n++] = 0;
    be32(head, 0x10000);
    be32(head, 0);
    be32(head, 0);
    be32(head, 0x5f0f3cf5);
    be16(head, 0);
    be16(head, 1000);
    head->n += 16;
    be16(head, 100);
    be16(head, 0);
    be16(head, 500);
    be16(head, 400);
    be16(head, 0);
    be16(head, 8);
    be16(head, 2);
    be16(head, 0);
    be16(head, 0);
    be32(hhea, 0x10000);
    be16(hhea, 800);
    be16(hhea, (uint16_t)-200);
    hhea->n = 34;
    be16(hhea, 2);
    be16(hmtx, 500);
    be16(hmtx, 0);
    be16(hmtx, 600);
    be16(hmtx, 100);
    be16(loca, 0);
    be16(loca, 0);
    be16(loca, (uint16_t)(glyf->n / 2));
    be32(maxp, 0x5000);
    be16(maxp, 2);
    size_t n = 12 + 7 * 16, at = n;
    memset(out, 0, 4096);
    buf dir = { .n = 0 };
    be32(&dir, 0x10000);
    be16(&dir, 7);
    be16(&dir, 64);
    be16(&dir, 2);
    be16(&dir, 48);
    for (int i = 0; i < 7; i++) {
        memcpy(dir.d + dir.n, tags[i], 4);
        dir.n += 4;
        be32(&dir, 0);
        be32(&dir, (uint32_t)at);
        be32(&dir, (uint32_t)t[i].n);
        memcpy(out + at, t[i].d, t[i].n);
        at += (t[i].n + 3) & ~(size_t)3;
    }
    memcpy(out, dir.d, dir.n);
    return at;
}

static uint8_t g_font[4096];
static bool g_shown, g_hidden;
static bool show(void) { return g_shown = true; }
static void hide(void) { g_hidden = true; }
static const rl_text_entry_host g_host = { show, hide, NULL };

static int g_done;
static bool g_confirmed;
static char g_text[RL_TEXT_ENTRY_MAX];

static void done(void *user, bool confirmed, const char *text)
{
    (void)user;
    g_done++;
    g_confirmed = confirmed;
    snprintf(g_text, sizeof g_text, "%s", text);
}

static void setup(void)
{
    rl_settings_declare_runtime();
    rl_settings_set("text_entry.text", "");
    rl_settings_set("text_entry.default", "Player");
    rl_settings_set("text_entry.panel", "auto");
    rl_text_entry_reset();
    g_done = 0;
    g_shown = g_hidden = false;
    g_text[0] = 0;
}

TEST(ttf_square_glyph_has_exact_coverage)
{
    size_t n = make_font(g_font);
    rl_ttf f;
    CHECK(rl_ttf_init(&f, g_font, n));
    CHECK_EQ(rl_ttf_glyph(&f, 'A'), 1);
    CHECK_EQ(rl_ttf_glyph(&f, 0x540d), 1);
    CHECK_EQ(rl_ttf_glyph(&f, 'B'), 0);
    CHECK(rl_ttf_advance(&f, 1, 100) > 59.9f && rl_ttf_advance(&f, 1, 100) < 60.1f);
    rl_ttf_bitmap b;
    CHECK(rl_ttf_render(&f, 1, 100, 0, &b));
    CHECK_EQ(b.x, 10);
    CHECK_EQ(b.y, -40);
    uint32_t full = 0, empty = 0;
    for (uint32_t y = 0; y < b.h; y++)
        for (uint32_t x = 0; x < b.w; x++) {
            uint8_t v = b.px[y * b.w + x];
            full += v == 255;
            empty += v == 0;
        }
    CHECK_EQ(full, 40u * 40u);
    CHECK_EQ(full + empty, b.w * b.h);
    free(b.px);
    CHECK(!rl_ttf_render(&f, 0, 100, 0, &b));
}

TEST(text_entry_answers_from_the_setting_without_a_panel)
{
    setup();
    rl_text_entry_set_host(&g_host);
    rl_settings_set("text_entry.text", "Ada Lovelace");
    rl_text_entry_args a = { .header = "Name", .initial = "Mira", .max_chars = 3 };
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_ANSWERED);
    CHECK_EQ(g_done, 1);
    CHECK(g_confirmed);
    CHECK(!strcmp(g_text, "Ada"));
    CHECK(!g_shown);
}

TEST(text_entry_answers_with_the_initial_text_then_the_default)
{
    setup();
    rl_text_entry_args a = { .header = "Name", .initial = "Mira" };
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_ANSWERED);
    CHECK(!strcmp(g_text, "Mira"));
    a.initial = NULL;
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_ANSWERED);
    CHECK(!strcmp(g_text, "Player"));
}

TEST(text_entry_panel_edits_and_confirms)
{
    setup();
    size_t n = make_font(g_font);
    rl_text_entry_add_font(g_font, n);
    rl_text_entry_set_host(&g_host);
    rl_text_entry_args a = { .header = "Name", .guide = "Your name", .initial = "Al", .max_chars = 4, .min_chars = 2,
                             .can_cancel = true };
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_SHOWN);
    CHECK(g_shown);
    CHECK(rl_text_entry_active());
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_BUSY);
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_BACKSPACE);
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_CONFIRM);
    CHECK_EQ(g_done, 0);
    rl_text_entry_input("\xe5\x90\x8d\x01x\xe2\x80\xa2yz");
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_HOME);
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_DELETE);
    rl_text_entry_input("B");
    uint8_t *rgb = malloc(640 * 360 * 3);
    uint8_t under[4 * 4 * 4];
    memset(under, 200, sizeof under);
    CHECK(rl_text_entry_draw(rgb, 640, 360, under, 4, 4, false, 0));
    CHECK(rl_text_entry_draw(rgb, 640, 360, under, 4, 4, true, 0));
    free(rgb);
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_CONFIRM);
    CHECK_EQ(g_done, 1);
    CHECK(g_confirmed);
    CHECK(!strcmp(g_text, "B\xe5\x90\x8dx\xe2\x80\xa2"));
    CHECK(g_hidden);
    CHECK(!rl_text_entry_active());
}

TEST(text_entry_taps_hit_the_buttons_of_either_layout)
{
    setup();
    size_t n = make_font(g_font);
    rl_text_entry_add_font(g_font, n);
    rl_text_entry_set_host(&g_host);
    rl_text_entry_args a = { .header = "Name", .initial = "Ada", .max_chars = 10, .can_cancel = true };
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_SHOWN);
    CHECK(!rl_text_entry_tap(1, 1, 1920, 1080, true));
    int32_t f[4];
    rl_text_entry_field(1920, 1080, true, f);
    CHECK(f[2] > 0 && f[3] > 0 && f[1] + f[3] < 540);
    CHECK(rl_text_entry_tap((float)1660, (float)(f[1] + f[3] / 2), 1920, 1080, true));
    CHECK_EQ(g_done, 1);
    CHECK(g_confirmed);
    CHECK(!strcmp(g_text, "Ada"));
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_SHOWN);
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_CANCEL);
    CHECK_EQ(g_done, 2);
    CHECK(!g_confirmed);
}

TEST(text_entry_close_drops_the_entry_without_an_answer)
{
    setup();
    size_t n = make_font(g_font);
    rl_text_entry_add_font(g_font, n);
    rl_text_entry_set_host(&g_host);
    int me;
    rl_text_entry_args a = { .header = "Name", .can_cancel = false };
    CHECK_EQ(rl_text_entry_open(&a, done, &me), RL_TEXT_ENTRY_SHOWN);
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_CANCEL);
    CHECK(rl_text_entry_active());
    rl_text_entry_close(&me);
    CHECK(!rl_text_entry_active());
    CHECK(g_hidden);
    CHECK_EQ(g_done, 0);
}

TEST(text_entry_panel_never_answers_at_once)
{
    setup();
    size_t n = make_font(g_font);
    rl_text_entry_add_font(g_font, n);
    rl_text_entry_set_host(&g_host);
    rl_settings_set("text_entry.panel", "never");
    rl_text_entry_args a = { .header = "Name", .initial = "Mira" };
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_ANSWERED);
    CHECK(!g_shown);
    CHECK(!strcmp(g_text, "Mira"));
    rl_settings_set("text_entry.panel", "auto");
}

TEST(text_entry_prompt_wraps_whole_above_the_field)
{
    setup();
    size_t n = make_font(g_font);
    rl_text_entry_add_font(g_font, n);
    rl_text_entry_set_host(&g_host);
    rl_text_entry_args a = { .header = "AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA", .max_chars = 8 };
    CHECK_EQ(rl_text_entry_open(&a, done, NULL), RL_TEXT_ENTRY_SHOWN);
    rl_te_place pl;
    pthread_mutex_lock(&rl_te_lock);
    rl_te_layout(400, 900, true, &pl);
    pthread_mutex_unlock(&rl_te_lock);
    CHECK(pl.nheader > 1);
    size_t total = 0;
    for (uint32_t i = 0; i < pl.nheader; i++)
        total += pl.header_len[i];
    CHECK_EQ(total + pl.nheader - 1, strlen(a.header));
    CHECK(pl.field[1] > pl.strip_y);
    CHECK(pl.field[1] + pl.field[3] <= pl.keyboard_top);
    rl_text_entry_key(RL_TEXT_ENTRY_KEY_CONFIRM);
}
