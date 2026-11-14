/**
 * @file srv_time_zone.c
 * @brief Time zone rule parsing and calendar conversions. See srv_time_zone.h.
 */
#include "srv_time_zone.h"

#include <string.h>

#define SECONDS_PER_DAY 86400
#define TZIF_HEADER_SIZE 44
#define TZIF_TYPE_SIZE 6
#define TZIF_LEAP_SIZE 8
#define TZ_NAME_COPY 7

static const uint8_t k_utc_tzif[] = {
    [0] = 'T', [1] = 'Z', [2] = 'i', [3] = 'f', [39] = 1, [43] = 4, [50] = 'U', [51] = 'T', [52] = 'C', [53] = 0,
};

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= m <= 2;
    int64_t era = floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int64_t *m, int64_t *d)
{
    z += 719468;
    int64_t era = floor_div(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static int32_t be32(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
}

bool tz_parse(tz_rule *out, const uint8_t *data, size_t size)
{
    if (size < TZIF_HEADER_SIZE || memcmp(data, "TZif", 4) != 0)
        return false;
    int32_t isut = be32(data + 20), isstd = be32(data + 24), leaps = be32(data + 28), times = be32(data + 32),
            types = be32(data + 36), chars = be32(data + 40);
    if (isut < 0 || isstd < 0 || leaps < 0 || times < 0 || times > TZ_MAX_TIMES || types < 1 || types > TZ_MAX_TYPES ||
        chars < 1 || chars >= TZ_CHARS_SIZE || (isut && isut != types) || (isstd && isstd != types))
        return false;
    uint64_t need = (uint64_t)TZIF_HEADER_SIZE + (uint64_t)times * 5 + (uint64_t)types * TZIF_TYPE_SIZE +
                    (uint64_t)chars + (uint64_t)leaps * TZIF_LEAP_SIZE + (uint64_t)isstd + (uint64_t)isut;
    if (size < need)
        return false;

    memset(out, 0, sizeof *out);
    out->time_count = times;
    out->type_count = types;
    out->char_count = chars;
    const uint8_t *p = data + TZIF_HEADER_SIZE;
    for (int32_t i = 0; i < times; i++, p += 4) {
        out->ats[i] = be32(p);
        if (i > 0 && out->ats[i] <= out->ats[i - 1])
            return false;
    }
    for (int32_t i = 0; i < times; i++, p++) {
        if (*p >= types)
            return false;
        out->types[i] = *p;
    }
    for (int32_t i = 0; i < types; i++, p += TZIF_TYPE_SIZE) {
        if (p[4] > 1 || p[5] >= chars)
            return false;
        out->ttis[i].gmt_offset = be32(p);
        out->ttis[i].is_dst = p[4];
        out->ttis[i].abbreviation_index = p[5];
    }
    memcpy(out->chars, p, (size_t)chars);
    p += chars + (size_t)leaps * TZIF_LEAP_SIZE;
    for (int32_t i = 0; i < isstd; i++)
        out->ttis[i].is_standard = *p++ != 0;
    for (int32_t i = 0; i < isut; i++)
        out->ttis[i].is_gmt = *p++ != 0;
    out->default_type = 0;
    if (out->ttis[0].is_dst)
        for (int32_t i = 1; i < types; i++)
            if (!out->ttis[i].is_dst) {
                out->default_type = i;
                break;
            }
    return true;
}

void tz_utc(tz_rule *out)
{
    (void)tz_parse(out, k_utc_tzif, sizeof k_utc_tzif);
}

static const tz_type *type_at(const tz_rule *rule, int64_t t)
{
    int32_t idx = rule->default_type;
    if (rule->time_count > 0 && t >= rule->ats[0]) {
        int32_t lo = 0, hi = rule->time_count - 1;
        while (lo < hi) {
            int32_t mid = lo + (hi - lo + 1) / 2;
            if (rule->ats[mid] <= t)
                lo = mid;
            else
                hi = mid - 1;
        }
        idx = rule->types[lo];
    }
    if (idx < 0 || idx >= rule->type_count)
        idx = 0;
    return &rule->ttis[idx];
}

bool tz_to_calendar(const tz_rule *rule, int64_t t, tz_calendar *out)
{
    const tz_type *ty = type_at(rule, t);
    int64_t local;
    if (__builtin_add_overflow(t, (int64_t)ty->gmt_offset, &local))
        return false;
    int64_t days = floor_div(local, SECONDS_PER_DAY);
    int64_t secs = local - days * SECONDS_PER_DAY;
    int64_t y, m, d;
    civil_from_days(days, &y, &m, &d);
    if (y < INT16_MIN || y > INT16_MAX)
        return false;
    memset(out, 0, sizeof *out);
    out->year = (int16_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->hour = (uint8_t)(secs / 3600);
    out->minute = (uint8_t)(secs % 3600 / 60);
    out->second = (uint8_t)(secs % 60);
    out->wday = (uint32_t)(((days % 7) + 11) % 7);
    out->yday = (uint32_t)(days - days_from_civil(y, 1, 1));
    const char *abbr = (const char *)rule->chars + ty->abbreviation_index;
    size_t max = (size_t)(rule->char_count - ty->abbreviation_index);
    size_t n = 0;
    while (n < max && n < TZ_NAME_COPY && abbr[n]) {
        out->name[n] = abbr[n];
        n++;
    }
    out->is_dst = ty->is_dst;
    out->gmt_offset = ty->gmt_offset;
    return true;
}

int64_t tz_to_posix(const tz_rule *rule, int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                    int32_t second)
{
    int64_t m0 = (int64_t)month - 1;
    int64_t y = (int64_t)year + floor_div(m0, 12);
    m0 -= floor_div(m0, 12) * 12;
    int64_t days = days_from_civil(y, m0 + 1, 1) + ((int64_t)day - 1);
    int64_t local = days * SECONDS_PER_DAY + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
    int64_t t = local - type_at(rule, local)->gmt_offset;
    for (int i = 0; i < 3; i++) {
        const tz_type *ty = type_at(rule, t);
        int64_t next = local - ty->gmt_offset;
        if (next == t)
            break;
        t = next;
    }
    return t;
}

bool tz_check(const tz_rule *rule)
{
    if (rule->time_count < 0 || rule->time_count > TZ_MAX_TIMES || rule->type_count < 1 ||
        rule->type_count > TZ_MAX_TYPES || rule->char_count < 1 || rule->char_count > TZ_CHARS_SIZE ||
        rule->default_type < 0 || rule->default_type >= rule->type_count)
        return false;
    for (int32_t i = 0; i < rule->time_count; i++)
        if (rule->types[i] >= rule->type_count)
            return false;
    for (int32_t i = 0; i < rule->type_count; i++)
        if (rule->ttis[i].abbreviation_index < 0 || rule->ttis[i].abbreviation_index >= rule->char_count)
            return false;
    return true;
}
