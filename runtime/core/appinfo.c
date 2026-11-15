/**
 * @file appinfo.c
 * @brief The registered title per language and icon of the game (appinfo.h).
 */
#include <relift/appinfo.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const *g_langs;
static const char *const *g_names;
static size_t g_count;
static const char *g_default;
static const char *g_b64;
static size_t g_b64_len;
static uint8_t *g_png;
static size_t g_png_len;
static int g_png_done;

void rl_app_set_identity(const char *const *langs, const char *const *names, size_t count, const char *default_lang,
                         const char *icon_b64, size_t icon_b64_len)
{
    g_langs = langs;
    g_names = names;
    g_count = count;
    g_default = default_lang;
    g_b64 = icon_b64;
    g_b64_len = icon_b64_len;
    free(g_png);
    g_png = NULL;
    g_png_len = 0;
    g_png_done = 0;
}

static const char *find(const char *key)
{
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_langs[i], key) == 0 && g_names[i] && *g_names[i])
            return g_names[i];
    return NULL;
}

static int ieq(const char *s, size_t n, const char *lit)
{
    if (strlen(lit) != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)lit[i]))
            return 0;
    return 1;
}

typedef struct tag {
    char lang[4];
    char region[4];
    int hant;
    int hans;
} tag;

static int parse_tag(const char *t, tag *out)
{
    memset(out, 0, sizeof *out);
    size_t n = strcspn(t, "_-.@");
    if (n < 2 || n > 3)
        return 0;
    for (size_t i = 0; i < n; i++)
        out->lang[i] = (char)tolower((unsigned char)t[i]);
    t += n;
    while (*t == '_' || *t == '-') {
        t++;
        size_t m = strcspn(t, "_-.@");
        if (m == 4 && ieq(t, 4, "Hant"))
            out->hant = 1;
        else if (m == 4 && ieq(t, 4, "Hans"))
            out->hans = 1;
        else if ((m == 2 || m == 3) && !out->region[0]) {
            for (size_t i = 0; i < m; i++)
                out->region[i] = (char)toupper((unsigned char)t[i]);
        }
        t += m;
    }
    return 1;
}

static int region_is(const tag *t, const char *list)
{
    size_t n = strlen(t->region);
    if (!n)
        return 0;
    for (const char *p = list; *p;) {
        size_t m = strcspn(p, ",");
        if (m == n && memcmp(p, t->region, n) == 0)
            return 1;
        p += m + (p[m] == ',');
    }
    return 0;
}

/** @brief The language keys, best first, a locale tag stands for. */
static size_t keys_of(const char *text, const char *out[3])
{
    tag t;
    if (!parse_tag(text, &t))
        return 0;
    const char *l = t.lang;
    if (!strcmp(l, "en")) {
        int gb = region_is(&t, "GB,AU,NZ,IE,ZA,IN,SG,HK");
        out[0] = gb ? "BritishEnglish" : "AmericanEnglish";
        out[1] = gb ? "AmericanEnglish" : "BritishEnglish";
        return 2;
    }
    if (!strcmp(l, "fr")) {
        int ca = region_is(&t, "CA");
        out[0] = ca ? "CanadianFrench" : "French";
        out[1] = ca ? "French" : "CanadianFrench";
        return 2;
    }
    if (!strcmp(l, "es")) {
        int spain = !t.region[0] || region_is(&t, "ES");
        out[0] = spain ? "Spanish" : "LatinAmericanSpanish";
        out[1] = spain ? "LatinAmericanSpanish" : "Spanish";
        return 2;
    }
    if (!strcmp(l, "pt")) {
        int br = region_is(&t, "BR");
        out[0] = br ? "BrazilianPortuguese" : "Portuguese";
        out[1] = br ? "Portuguese" : "BrazilianPortuguese";
        return 2;
    }
    if (!strcmp(l, "zh")) {
        int trad = t.hant || (!t.hans && region_is(&t, "TW,HK,MO"));
        out[0] = trad ? "TraditionalChinese" : "SimplifiedChinese";
        out[1] = trad ? "SimplifiedChinese" : "TraditionalChinese";
        return 2;
    }
    static const struct {
        const char *code, *key;
    } one[] = { { "ja", "Japanese" }, { "de", "German" }, { "it", "Italian" },
                { "nl", "Dutch" },    { "ru", "Russian" }, { "ko", "Korean" } };
    for (size_t i = 0; i < sizeof one / sizeof one[0]; i++)
        if (!strcmp(l, one[i].code)) {
            out[0] = one[i].key;
            return 1;
        }
    return 0;
}

const char *rl_app_title(const char *const *tags, size_t ntags)
{
    if (!g_count)
        return NULL;
    for (size_t i = 0; i < ntags; i++) {
        if (!tags[i] || !*tags[i])
            continue;
        const char *hit = find(tags[i]);
        if (hit)
            return hit;
        const char *keys[3];
        size_t n = keys_of(tags[i], keys);
        for (size_t k = 0; k < n; k++)
            if ((hit = find(keys[k])))
                return hit;
    }
    const char *hit = g_default ? find(g_default) : NULL;
    if (hit)
        return hit;
    for (size_t i = 0; i < g_count; i++)
        if (g_names[i] && *g_names[i])
            return g_names[i];
    return NULL;
}

static int b64v(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

const uint8_t *rl_app_icon_png(size_t *size)
{
    if (!g_png_done) {
        g_png_done = 1;
        if (g_b64 && g_b64_len) {
            uint8_t *p = malloc(g_b64_len / 4 * 3 + 3);
            size_t n = 0;
            uint32_t acc = 0;
            int bits = 0, ok = p != NULL;
            for (size_t i = 0; ok && i < g_b64_len; i++) {
                if (g_b64[i] == '=')
                    break;
                int v = b64v((unsigned char)g_b64[i]);
                if (v < 0) {
                    ok = 0;
                    break;
                }
                acc = (acc << 6) | (uint32_t)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    p[n++] = (uint8_t)(acc >> bits);
                    acc &= (1u << bits) - 1;
                }
            }
            if (ok && n) {
                g_png = p;
                g_png_len = n;
            } else {
                free(p);
            }
        }
    }
    *size = g_png_len;
    return g_png;
}
