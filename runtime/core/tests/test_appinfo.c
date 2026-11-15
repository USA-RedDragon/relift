/**
 * @file test_appinfo.c
 * @brief The title pick by locale with its fallbacks, and the icon decoded from its base64 text.
 */
#include "runner.h"

#include <relift/appinfo.h>

static const char *const k_langs[] = { "AmericanEnglish", "Japanese", "French", "CanadianFrench",
                                       "TraditionalChinese", "BrazilianPortuguese", "German" };
static const char *const k_names[] = { "A", "J", "F", "CF", "TC", "BP", "" };

static void reg(const char *def)
{
    rl_app_set_identity(k_langs, k_names, 7, def, NULL, 0);
}

static const char *pick1(const char *a)
{
    const char *t[1] = { a };
    return rl_app_title(t, 1);
}

TEST(a_locale_picks_the_language_it_stands_for)
{
    reg("AmericanEnglish");
    CHECK(strcmp(pick1("ja_JP.UTF-8"), "J") == 0);
    CHECK(strcmp(pick1("fr"), "F") == 0);
    CHECK(strcmp(pick1("fr_CA.UTF-8"), "CF") == 0);
    CHECK(strcmp(pick1("zh-Hant-TW"), "TC") == 0);
    CHECK(strcmp(pick1("zh_HK"), "TC") == 0);
    CHECK(strcmp(pick1("pt_PT"), "BP") == 0);
    CHECK(strcmp(pick1("en-GB"), "A") == 0);
    CHECK(strcmp(pick1("French"), "F") == 0);
}

TEST(a_missing_language_falls_back_to_the_default_then_the_first)
{
    reg("Japanese");
    CHECK(strcmp(pick1("de_DE"), "J") == 0);
    CHECK(strcmp(pick1("C"), "J") == 0);
    CHECK(strcmp(pick1("xx"), "J") == 0);
    reg(NULL);
    CHECK(strcmp(pick1("de"), "A") == 0);
    reg("German");
    CHECK(strcmp(pick1(""), "A") == 0);
}

TEST(the_first_matching_tag_wins)
{
    reg("AmericanEnglish");
    const char *t[] = { NULL, "C", "ko_KR", "fr_FR", "ja" };
    CHECK(strcmp(rl_app_title(t, 5), "F") == 0);
}

TEST(a_program_without_identity_has_no_title_and_no_icon)
{
    rl_app_set_identity(NULL, NULL, 0, NULL, NULL, 0);
    CHECK(pick1("ja") == NULL);
    size_t n = 7;
    CHECK(rl_app_icon_png(&n) == NULL && n == 0);
}

TEST(the_icon_is_decoded_from_base64)
{
    static const char b64[] = "iVBORw0KGgo=";
    rl_app_set_identity(k_langs, k_names, 7, NULL, b64, sizeof b64 - 1);
    size_t n = 0;
    const uint8_t *p = rl_app_icon_png(&n);
    CHECK(p != NULL && n == 8);
    CHECK(memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0);
    static const char bad[] = "iV*";
    rl_app_set_identity(k_langs, k_names, 7, NULL, bad, sizeof bad - 1);
    CHECK(rl_app_icon_png(&n) == NULL && n == 0);
}
