/**
 * @file srv_time_zone.h
 * @brief Time zone rules for the time services: the 0x4000-byte rule blob
 * the guest passes around, a parser for the TZif data block (RFC 8536,
 * version 1 layout) and conversions between POSIX time and calendar time.
 *
 * The blob layout and the limits follow Ryujinx (MIT) Services/Time/TimeZone.
 */
#ifndef RELIFT_SRV_TIME_ZONE_H
#define RELIFT_SRV_TIME_ZONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Size of a rule blob. */
#define TZ_RULE_SIZE 0x4000
/** @brief Limits of a rule. */
#define TZ_MAX_TYPES 128
#define TZ_MAX_TIMES 1000
#define TZ_CHARS_SIZE 512

/** @brief One local time type of a rule (16 bytes). */
typedef struct tz_type {
    int32_t gmt_offset;
    uint8_t is_dst;
    uint8_t pad1[3];
    int32_t abbreviation_index;
    uint8_t is_standard;
    uint8_t is_gmt;
    uint16_t pad2;
} tz_type;

/** @brief A parsed rule; the guest sees it as an opaque 0x4000-byte blob. */
typedef struct tz_rule {
    int32_t time_count;
    int32_t type_count;
    int32_t char_count;
    uint8_t go_back;
    uint8_t go_ahead;
    int64_t ats[TZ_MAX_TIMES];
    uint8_t types[TZ_MAX_TIMES];
    tz_type ttis[TZ_MAX_TYPES];
    uint8_t chars[TZ_CHARS_SIZE];
    int32_t default_type;
    uint8_t reserved[TZ_RULE_SIZE - 0x2D3C];
} tz_rule;

_Static_assert(sizeof(tz_rule) == TZ_RULE_SIZE, "rule blob size");
_Static_assert(offsetof(tz_rule, default_type) == 0x2D38, "rule blob layout");

/** @brief A calendar time as the guest sees it, with the extra fields of nn::time::sf::CalendarAdditionalInfo. */
typedef struct tz_calendar {
    int16_t year;
    uint8_t month, day, hour, minute, second;
    uint32_t wday, yday;
    char name[8];
    uint8_t is_dst;
    int32_t gmt_offset;
} tz_calendar;

/** @brief Parses a TZif file (header and the version 1 data block) into out. False when it is malformed or over the limits. */
bool tz_parse(tz_rule *out, const uint8_t *data, size_t size);
/** @brief Loads the built-in UTC rule. */
void tz_utc(tz_rule *out);
/** @brief Converts POSIX time t; false when the year does not fit the guest's 16 bits. */
bool tz_to_calendar(const tz_rule *rule, int64_t t, tz_calendar *out);
/** @brief Converts a calendar time (fields may be out of range and are normalized) to POSIX time. */
int64_t tz_to_posix(const tz_rule *rule, int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                    int32_t second);
/** @brief True when the counts and indexes of a rule from the guest are inside the limits, so the conversions stay in bounds. */
bool tz_check(const tz_rule *rule);

#endif
