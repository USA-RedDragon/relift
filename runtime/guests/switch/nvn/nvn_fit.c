/**
 * @file nvn_fit.c
 * @brief Whether the shader tables fit the device a run makes: what the device the tables were made for offered (their
 * table.device) against what this run's device offers (the device.json it wrote, nvn_vk.c write_caps).
 *
 * Both files are the trimmed Vulkan profiles JSON write_caps writes. The translator uses what its device offers: Int64
 * with shaderInt64, subgroup operations in the stages subgroupSupportedStages names, the shader clock, and so on. A
 * table made for a device with a feature this device lacks holds modules the driver must refuse, and some drivers
 * crash on them instead of refusing (the Adreno 740 system driver, on Int64 vertex programs from tables made for
 * Turnip). So a run checks the two before it makes any module (nvn_shader_stamp_check), and a missing feature stops
 * the run, naming what is missing.
 *
 * What counts as missing, for each value of the table's device:
 *
 * - an extension that this device does not have;
 * - a feature that is true there and false or absent here;
 * - a stage or operation of subgroupSupportedStages, subgroupSupportedOperations or requiredSubgroupSizeStages that
 *   this device does not list;
 * - subgroupSize, when it differs and the table's device had no subgroupSizeControl (the translation then lays a warp
 *   onto that one size), minSubgroupSize when this device's is larger, and any other number (the limits, the maximum
 *   subgroup size) when this device's is smaller.
 *
 * Every other difference (the relift probes, which pick float math the table was made for; the names and driver
 * identity; a feature this device has and the table's did not) is reported as a difference but does not stop the run.
 */
#include "nvn_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief The kind of a flattened JSON value. */
enum { V_BOOL, V_NUM, V_STR, V_MEMBER };

/** @brief One flattened value: its dotted path, kind and text (a member is one string of an array, by value). */
typedef struct fit_val {
    char path[200];
    int kind;
    char text[96];
} fit_val;

/** @brief A flattened document. */
typedef struct fit_doc {
    fit_val *v;
    size_t n, cap;
    bool bad;
} fit_doc;

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
    return p;
}

static void add(fit_doc *d, const char *path, int kind, const char *text, size_t tn)
{
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64;
        fit_val *nv = realloc(d->v, cap * sizeof *nv);
        if (!nv) {
            d->bad = true;
            return;
        }
        d->v = nv;
        d->cap = cap;
    }
    fit_val *v = &d->v[d->n++];
    snprintf(v->path, sizeof v->path, "%s", path);
    v->kind = kind;
    if (tn >= sizeof v->text)
        tn = sizeof v->text - 1;
    memcpy(v->text, text, tn);
    v->text[tn] = 0;
}

/** @brief Reads a JSON string at p (at its opening quote) into out; returns the text after it, NULL on error. */
static const char *read_str(const char *p, char *out, size_t n)
{
    if (*p != '"')
        return NULL;
    p++;
    size_t k = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p++;
        if (k + 1 < n)
            out[k++] = *p;
        p++;
    }
    out[k] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static const char *value(fit_doc *d, const char *p, const char *path, int depth);

static const char *object(fit_doc *d, const char *p, const char *path, int depth)
{
    p = skip_ws(p + 1);
    if (*p == '}')
        return p + 1;
    for (;;) {
        char key[96], sub[200];
        p = read_str(skip_ws(p), key, sizeof key);
        if (!p)
            return NULL;
        p = skip_ws(p);
        if (*p != ':')
            return NULL;
        snprintf(sub, sizeof sub, "%s%s%s", path, *path ? "." : "", key);
        p = value(d, skip_ws(p + 1), sub, depth + 1);
        if (!p)
            return NULL;
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        return *p == '}' ? p + 1 : NULL;
    }
}

static const char *array(fit_doc *d, const char *p, const char *path, int depth)
{
    p = skip_ws(p + 1);
    if (*p == ']')
        return p + 1;
    for (unsigned i = 0;; i++) {
        p = skip_ws(p);
        if (*p == '"') {
            char s[96];
            p = read_str(p, s, sizeof s);
            if (!p)
                return NULL;
            add(d, path, V_MEMBER, s, strlen(s));
        } else {
            char sub[200];
            snprintf(sub, sizeof sub, "%s[%u]", path, i);
            p = value(d, p, sub, depth + 1);
            if (!p)
                return NULL;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        return *p == ']' ? p + 1 : NULL;
    }
}

static const char *value(fit_doc *d, const char *p, const char *path, int depth)
{
    if (depth > 16)
        return NULL;
    if (*p == '{')
        return object(d, p, path, depth);
    if (*p == '[')
        return array(d, p, path, depth);
    if (*p == '"') {
        char s[96];
        const char *e = read_str(p, s, sizeof s);
        if (e)
            add(d, path, V_STR, s, strlen(s));
        return e;
    }
    if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
        bool t = *p == 't';
        add(d, path, V_BOOL, t ? "true" : "false", t ? 4 : 5);
        return p + (t ? 4 : 5);
    }
    if (!strncmp(p, "null", 4))
        return p + 4;
    const char *s = p;
    while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))
        p++;
    if (p == s)
        return NULL;
    add(d, path, V_NUM, s, (size_t)(p - s));
    return p;
}

static bool parse(fit_doc *d, const char *text)
{
    memset(d, 0, sizeof *d);
    const char *e = value(d, skip_ws(text), "", 0);
    return e && !d->bad;
}

static const fit_val *find(const fit_doc *d, const char *path, int kind, const char *member)
{
    for (size_t i = 0; i < d->n; i++)
        if (d->v[i].kind == kind && !strcmp(d->v[i].path, path) && (!member || !strcmp(d->v[i].text, member)))
            return &d->v[i];
    return NULL;
}

/** @brief The last part of a dotted path. */
static const char *leaf(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : path;
}

/** @brief Appends text to out (n bytes), separated by ", " from what is there. */
static void note(char *out, size_t n, unsigned *count, const char *fmt, const char *a, const char *b)
{
    (*count)++;
    if (!out || !n)
        return;
    size_t at = strlen(out);
    if (at + 4 >= n)
        return;
    char one[260];
    snprintf(one, sizeof one, fmt, a, b);
    snprintf(out + at, n - at, "%s%s", at ? ", " : "", one);
}

/** @brief The device's name and driver from a flattened device.json, as "<name> (<driver>)". */
static void identity(const fit_doc *d, char *out, size_t n)
{
    const char *name = "an unnamed device", *drv = "unknown driver";
    for (size_t i = 0; i < d->n; i++) {
        if (d->v[i].kind != V_STR)
            continue;
        if (!strcmp(leaf(d->v[i].path), "deviceName"))
            name = d->v[i].text;
        else if (!strcmp(leaf(d->v[i].path), "driverID"))
            drv = d->v[i].text;
    }
    snprintf(out, n, "%s (%s)", name, drv);
}

/**
 * @brief Whether this device's number b falls short of the table device's a for the property name at path: a
 * different subgroupSize when the table's device could not pick one (no subgroupSizeControl: the translation then
 * lays a warp onto that size), a larger minSubgroupSize, or a smaller limit or maximum.
 */
static bool number_short(const char *path, const char *name, double a, double b, bool size_control)
{
    if (!strcmp(name, "subgroupSize"))
        return !size_control;
    if (!strncmp(name, "minSubgroup", 11))
        return b > a;
    if (!strncmp(name, "max", 3) || strstr(path, ".limits."))
        return b < a;
    return false;
}

unsigned nvn_fit_compare(const char *table_json, const char *device_json, nvn_fit *out)
{
    memset(out, 0, sizeof *out);
    fit_doc t, h;
    bool ok_t = parse(&t, table_json), ok_h = parse(&h, device_json);
    char *ms = out->missing, *os = out->other;
    size_t mn = sizeof out->missing, on = sizeof out->other;
    if (!ok_t || !ok_h) {
        note(ms, mn, &out->missing_count, "%s%s", ok_t ? "this run's device.json" : "the table's table.device",
             " is not device JSON");
        free(t.v);
        free(h.v);
        return out->missing_count;
    }
    bool size_control = false;
    for (size_t i = 0; i < t.n; i++)
        if (t.v[i].kind == V_BOOL && !strcmp(leaf(t.v[i].path), "subgroupSizeControl"))
            size_control = !strcmp(t.v[i].text, "true");
    for (size_t i = 0; i < t.n; i++) {
        const fit_val *v = &t.v[i];
        const char *name = leaf(v->path);
        bool probe = strstr(v->path, ".ReliftDeviceProbes.") != NULL;
        bool feature = strstr(v->path, ".features.") != NULL;
        bool extension = strstr(v->path, ".extensions.") != NULL;
        if (v->kind == V_MEMBER) {
            if (!find(&h, v->path, V_MEMBER, v->text))
                note(ms, mn, &out->missing_count, "%s %s", name, v->text);
            continue;
        }
        const fit_val *w = find(&h, v->path, v->kind, NULL);
        if (extension) {
            if (!w)
                note(ms, mn, &out->missing_count, "%s%s", name, "");
            continue;
        }
        if (v->kind == V_BOOL) {
            bool here = w && !strcmp(w->text, "true");
            bool there = !strcmp(v->text, "true");
            if (here == there)
                continue;
            if (there && !probe)
                note(ms, mn, &out->missing_count, "%s%s", name, "");
            else
                note(os, on, &out->other_count, "%s %s", name, here ? "true here" : "false here");
            continue;
        }
        if (v->kind == V_NUM && w) {
            double a = strtod(v->text, NULL), b = strtod(w->text, NULL);
            if (a == b)
                continue;
            char what[200];
            snprintf(what, sizeof what, "%s here", w->text);
            if (!probe && !feature && number_short(v->path, name, a, b, size_control))
                note(ms, mn, &out->missing_count, "%s %s", name, what);
            else
                note(os, on, &out->other_count, "%s %s", name, what);
            continue;
        }
        if (!w || strcmp(w->text, v->text))
            note(os, on, &out->other_count, "%s %s", name, w ? w->text : "absent here");
    }
    identity(&t, out->table_device, sizeof out->table_device);
    identity(&h, out->this_device, sizeof out->this_device);
    free(t.v);
    free(h.v);
    return out->missing_count;
}
