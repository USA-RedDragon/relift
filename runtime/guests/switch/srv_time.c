/**
 * @file srv_time.c
 * @brief The time services: time:u, time:a, time:s, time:su and time:r with
 * the system clocks, the steady clock, the time zone service and the shared
 * memory page libnx reads.
 *
 * Command ids, permissions, result codes and structure layouts follow
 * Ryujinx (MIT) Services/Time; the shared memory layout is the one libnx
 * (ISC) time.c reads. The user, local and network clocks follow the host's
 * wall clock until a program sets them; the steady clock is the host's
 * monotonic clock. Only the UTC time zone exists.
 */
#include "services.h"
#include "srv_time_zone.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIME_RESULT(desc) RL_MAKE_RESULT(116, desc)
#define TIME_PERMISSION_DENIED TIME_RESULT(1)
#define TIME_MISMATCH TIME_RESULT(102)
#define TIME_NOT_FOUND TIME_RESULT(200)
#define TIME_OUT_OF_RANGE TIME_RESULT(902)
#define TIME_CONVERSION_FAILED TIME_RESULT(903)
#define TIME_ZONE_NOT_FOUND TIME_RESULT(989)
#define TIME_NOT_IMPLEMENTED TIME_RESULT(990)

enum {
    PERM_LOCAL_WRITABLE = 0x1,
    PERM_USER_WRITABLE = 0x2,
    PERM_NETWORK_WRITABLE = 0x4,
    PERM_TIME_ZONE_WRITABLE = 0x8,
    PERM_STEADY_WRITABLE = 0x10,
    PERM_ADMIN = PERM_LOCAL_WRITABLE | PERM_USER_WRITABLE | PERM_TIME_ZONE_WRITABLE
};

#define NS_PER_SECOND 1000000000ll
#define SHMEM_SIZE 0x1000u
#define SHMEM_STEADY 0x00u
#define SHMEM_LOCAL 0x38u
#define SHMEM_NETWORK 0x80u
#define SHMEM_AUTO_CORRECTION 0xC8u
#define SHMEM_HEADER 8u
#define LOCATION_NAME_SIZE 0x24u
#define STANDARD_USER_CLOCK_INITIAL_YEAR 2019u
#define HOST_DRIFT_SECONDS 2
#define SNAPSHOT_SIZE 0xD0u
#define POINTER_BUFFER 0x1000

typedef struct steady_ctx {
    int64_t base_time;
    uint8_t source[16];
} steady_ctx;

typedef struct clock_ctx {
    int64_t offset;
    int64_t time_point;
    uint8_t source[16];
} clock_ctx;

_Static_assert(sizeof(steady_ctx) == 24 && sizeof(clock_ctx) == 32, "libnx shared memory layout");

enum { CLOCK_LOCAL_USER = 0, CLOCK_NETWORK = 1, CLOCKS = 2 };

typedef struct clock_state {
    clock_ctx ctx;
    bool tracks_host;
    hos_revent *event;
} clock_state;

static struct {
    uint8_t source[16];
    int64_t internal_offset_ns;
    int64_t test_offset_ns;
    clock_state clocks[CLOCKS];
    bool auto_correction;
    clock_ctx auto_correction_updated;
    tz_rule rule;
    char location[LOCATION_NAME_SIZE];
    uint8_t rule_version[16];
    steady_ctx location_updated;
    hos_shmem *shm;
} g_time;

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static int64_t ticks_ns(void) { return (int64_t)rl_ticks_to_ns(rl_ns_to_ticks(rl_clock_ns())); }

static int64_t steady_ns(void) { return ticks_ns() + g_time.test_offset_ns + g_time.internal_offset_ns; }

static int64_t steady_seconds(void) { return floor_div(steady_ns(), NS_PER_SECOND); }

static int64_t wall_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

static void shm_put(uint32_t off, const void *data, uint32_t size)
{
    if (!g_time.shm)
        return;
    uint8_t *base = rl_hos_shmem_ptr(g_time.shm) + off;
    uint32_t counter;
    memcpy(&counter, base, 4);
    counter++;
    memcpy(base + SHMEM_HEADER + (counter & 1u) * size, data, size);
    __atomic_store_n((uint32_t *)(void *)base, counter, __ATOMIC_RELEASE);
}

static void shm_put_steady(void)
{
    steady_ctx s;
    s.base_time = g_time.test_offset_ns + g_time.internal_offset_ns;
    memcpy(s.source, g_time.source, 16);
    shm_put(SHMEM_STEADY, &s, sizeof s);
}

static void shm_put_clock(uint32_t clock)
{
    shm_put(clock == CLOCK_NETWORK ? SHMEM_NETWORK : SHMEM_LOCAL, &g_time.clocks[clock].ctx,
            sizeof g_time.clocks[clock].ctx);
}

static void shm_put_auto_correction(void)
{
    if (!g_time.shm)
        return;
    uint8_t *base = rl_hos_shmem_ptr(g_time.shm) + SHMEM_AUTO_CORRECTION;
    uint32_t counter;
    memcpy(&counter, base, 4);
    counter++;
    base[4 + (counter & 1u)] = g_time.auto_correction;
    __atomic_store_n((uint32_t *)(void *)base, counter, __ATOMIC_RELEASE);
}

static void clock_follow_host(uint32_t clock)
{
    clock_state *c = &g_time.clocks[clock];
    if (!c->tracks_host)
        return;
    int64_t steady = steady_seconds();
    int64_t offset = wall_seconds() - steady;
    int64_t delta = offset - c->ctx.offset;
    if (delta < HOST_DRIFT_SECONDS && delta > -HOST_DRIFT_SECONDS && c->ctx.time_point != 0)
        return;
    c->ctx.offset = offset;
    c->ctx.time_point = steady;
    memcpy(c->ctx.source, g_time.source, 16);
    shm_put_clock(clock);
}

static void time_reset(void)
{
    if (g_time.shm)
        hos_kobj_unref(&g_time.shm->obj);
    for (uint32_t i = 0; i < CLOCKS; i++)
        if (g_time.clocks[i].event)
            hos_kobj_unref(&g_time.clocks[i].event->obj);
    memset(&g_time, 0, sizeof g_time);
    memcpy(g_time.source, &hos.entropy[0], 8);
    memcpy(g_time.source + 8, &hos.entropy[1], 8);
    for (uint32_t i = 0; i < CLOCKS; i++)
        g_time.clocks[i].tracks_host = true;
    g_time.auto_correction = true;
    tz_utc(&g_time.rule);
    memcpy(g_time.location, "UTC", 4);
    memcpy(g_time.rule_version, "relift-tz-utc-00", 16);
    memcpy(g_time.location_updated.source, g_time.source, 16);
    memcpy(g_time.auto_correction_updated.source, g_time.source, 16);
}

static void write_steady_time_point(void *dst)
{
    int64_t tp = steady_seconds();
    memcpy(dst, &tp, 8);
    memcpy((uint8_t *)dst + 8, g_time.source, 16);
}

static rl_result clock_time(uint32_t clock, int64_t *out)
{
    clock_follow_host(clock);
    const clock_ctx *c = &g_time.clocks[clock].ctx;
    if (memcmp(c->source, g_time.source, 16) != 0)
        return TIME_MISMATCH;
    *out = steady_seconds() + c->offset;
    return KR_SUCCESS;
}

static bool read_in(const rl_ipc_req *r, uint32_t off, void *dst, uint32_t size)
{
    if (r->in_size < off + size)
        return false;
    memcpy(dst, r->in + off, size);
    return true;
}

typedef struct service_state {
    uint32_t permissions;
    bool psc;
} service_state;

typedef struct clock_object {
    uint32_t clock;
    bool writable;
} clock_object;

typedef struct zone_object {
    bool writable;
    bool psc;
} zone_object;


static void put_calendar(uint8_t *time8, uint8_t *info, const tz_calendar *c)
{
    uint16_t year = (uint16_t)c->year;
    memcpy(time8, &year, 2);
    time8[2] = c->month;
    time8[3] = c->day;
    time8[4] = c->hour;
    time8[5] = c->minute;
    time8[6] = c->second;
    memcpy(info, &c->wday, 4);
    memcpy(info + 4, &c->yday, 4);
    memcpy(info + 8, c->name, 8);
    info[16] = c->is_dst;
    memcpy(info + 20, &c->gmt_offset, 4);
}

static rl_result clock_get_current_time(rl_ipc_req *r)
{
    const clock_object *o = r->self->state;
    int64_t t = 0;
    hos_lock();
    rl_result rc = clock_time(o->clock, &t);
    hos_unlock();
    if (rc == KR_SUCCESS)
        *(int64_t *)rl_ipc_out(r, 8) = t;
    return rc;
}

static void clock_signal(uint32_t clock)
{
    if (g_time.clocks[clock].event)
        rl_hos_event_signal(g_time.clocks[clock].event);
}

static rl_result clock_set_current_time(rl_ipc_req *r)
{
    const clock_object *o = r->self->state;
    int64_t t;
    if (!o->writable)
        return TIME_PERMISSION_DENIED;
    if (!read_in(r, 0, &t, 8))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    clock_state *c = &g_time.clocks[o->clock];
    c->tracks_host = false;
    c->ctx.time_point = steady_seconds();
    c->ctx.offset = t - c->ctx.time_point;
    memcpy(c->ctx.source, g_time.source, 16);
    shm_put_clock(o->clock);
    clock_signal(o->clock);
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result clock_get_context(rl_ipc_req *r)
{
    const clock_object *o = r->self->state;
    hos_lock();
    clock_follow_host(o->clock);
    memcpy(rl_ipc_out(r, sizeof(clock_ctx)), &g_time.clocks[o->clock].ctx, sizeof(clock_ctx));
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result clock_set_context(rl_ipc_req *r)
{
    const clock_object *o = r->self->state;
    clock_ctx c;
    if (!o->writable)
        return TIME_PERMISSION_DENIED;
    if (!read_in(r, 0, &c, sizeof c))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    g_time.clocks[o->clock].tracks_host = false;
    g_time.clocks[o->clock].ctx = c;
    shm_put_clock(o->clock);
    clock_signal(o->clock);
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result clock_get_operation_event(rl_ipc_req *r)
{
    const clock_object *o = r->self->state;
    rl_result rc;
    hos_lock();
    clock_state *c = &g_time.clocks[o->clock];
    if (!c->event)
        c->event = rl_hos_event_create();
    rc = c->event ? rl_ipc_push_copy(r, &c->event->obj) : KR_OUT_OF_RESOURCE;
    hos_unlock();
    return rc;
}

static const rl_ipc_cmd system_clock_cmds[] = {
    { 0, "GetCurrentTime", clock_get_current_time },
    { 1, "SetCurrentTime", clock_set_current_time },
    { 2, "GetSystemClockContext", clock_get_context },
    { 3, "SetSystemClockContext", clock_set_context },
    { 4, "GetOperationEventReadableHandle", clock_get_operation_event },
};

static const rl_ipc_class system_clock_class = { "time::ISystemClock", system_clock_cmds,
                                                 sizeof system_clock_cmds / sizeof system_clock_cmds[0], 0, free };

static rl_result steady_get_current_time_point(rl_ipc_req *r)
{
    hos_lock();
    write_steady_time_point(rl_ipc_out(r, 24));
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result steady_get_test_offset(rl_ipc_req *r)
{
    *(int64_t *)rl_ipc_out(r, 8) = g_time.test_offset_ns;
    return KR_SUCCESS;
}

static rl_result steady_set_offset(rl_ipc_req *r, int64_t *field)
{
    const bool *writable = r->self->state;
    int64_t ns;
    if (!*writable)
        return TIME_PERMISSION_DENIED;
    if (!read_in(r, 0, &ns, 8))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    *field = ns;
    shm_put_steady();
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result steady_set_test_offset(rl_ipc_req *r) { return steady_set_offset(r, &g_time.test_offset_ns); }

static rl_result steady_get_rtc_value(rl_ipc_req *r)
{
    (void)r;
    return TIME_NOT_IMPLEMENTED;
}

static rl_result steady_is_rtc_reset_detected(rl_ipc_req *r)
{
    (void)rl_ipc_out(r, 1);
    return KR_SUCCESS;
}

static rl_result steady_get_setup_result_value(rl_ipc_req *r)
{
    (void)rl_ipc_out(r, 4);
    return KR_SUCCESS;
}

static rl_result steady_get_internal_offset(rl_ipc_req *r)
{
    *(int64_t *)rl_ipc_out(r, 8) = g_time.internal_offset_ns;
    return KR_SUCCESS;
}

static rl_result steady_set_internal_offset(rl_ipc_req *r) { return steady_set_offset(r, &g_time.internal_offset_ns); }

static const rl_ipc_cmd steady_clock_cmds[] = {
    { 0, "GetCurrentTimePoint", steady_get_current_time_point },
    { 2, "GetTestOffset", steady_get_test_offset },
    { 3, "SetTestOffset", steady_set_test_offset },
    { 100, "GetRtcValue", steady_get_rtc_value },
    { 101, "IsRtcResetDetected", steady_is_rtc_reset_detected },
    { 102, "GetSetupResultValue", steady_get_setup_result_value },
    { 200, "GetInternalOffset", steady_get_internal_offset },
    { 201, "SetInternalOffset", steady_set_internal_offset },
};

static const rl_ipc_class steady_clock_class = { "time::ISteadyClock", steady_clock_cmds,
                                                 sizeof steady_clock_cmds / sizeof steady_clock_cmds[0], 0, free };

static const char *const k_location_names[] = { "Etc/UTC", "UTC" };
#define N_LOCATIONS ((uint32_t)(sizeof k_location_names / sizeof k_location_names[0]))

static bool location_known(const char *name)
{
    for (uint32_t i = 0; i < N_LOCATIONS; i++)
        if (strncmp(k_location_names[i], name, LOCATION_NAME_SIZE) == 0)
            return true;
    return false;
}

static bool read_location(const rl_ipc_req *r, char name[LOCATION_NAME_SIZE])
{
    if (!read_in(r, 0, name, LOCATION_NAME_SIZE))
        return false;
    name[LOCATION_NAME_SIZE - 1] = 0;
    return true;
}

static rl_result zone_get_device_location_name(rl_ipc_req *r)
{
    hos_lock();
    memcpy(rl_ipc_out(r, LOCATION_NAME_SIZE), g_time.location, LOCATION_NAME_SIZE);
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result zone_set_device_location_name(rl_ipc_req *r)
{
    const zone_object *z = r->self->state;
    char name[LOCATION_NAME_SIZE];
    if (!z->writable)
        return TIME_PERMISSION_DENIED;
    if (z->psc)
        return TIME_NOT_IMPLEMENTED;
    if (!read_location(r, name))
        return SF_RESULT_INVALID_HEADER;
    if (!location_known(name))
        return TIME_ZONE_NOT_FOUND;
    hos_lock();
    tz_utc(&g_time.rule);
    memcpy(g_time.location, name, LOCATION_NAME_SIZE);
    g_time.location_updated.base_time = steady_seconds();
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result zone_get_total_location_name_count(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = N_LOCATIONS;
    return KR_SUCCESS;
}

static rl_result zone_load_location_name_list(rl_ipc_req *r)
{
    const zone_object *z = r->self->state;
    uint32_t index;
    if (z->psc)
        return TIME_NOT_IMPLEMENTED;
    if (!read_in(r, 0, &index, 4))
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    uint32_t room = (uint32_t)(b.size / LOCATION_NAME_SIZE), n = 0;
    uint8_t names[N_LOCATIONS][LOCATION_NAME_SIZE];
    memset(names, 0, sizeof names);
    for (uint32_t i = index; i < N_LOCATIONS && n < room; i++, n++)
        strncpy((char *)names[n], k_location_names[i], LOCATION_NAME_SIZE - 1);
    if (n && !rl_ipc_buf_write(&b, names, (uint64_t)n * LOCATION_NAME_SIZE))
        return KR_INVALID_MEM_STATE;
    *(uint32_t *)rl_ipc_out(r, 4) = n;
    return KR_SUCCESS;
}

static rl_result write_rule(rl_ipc_req *r, const tz_rule *rule)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    if (b.size != TZ_RULE_SIZE || !rl_ipc_buf_write(&b, rule, TZ_RULE_SIZE))
        return KR_INVALID_MEM_STATE;
    return KR_SUCCESS;
}

static rl_result zone_load_time_zone_rule(rl_ipc_req *r)
{
    const zone_object *z = r->self->state;
    char name[LOCATION_NAME_SIZE];
    if (z->psc)
        return TIME_NOT_IMPLEMENTED;
    if (!read_location(r, name))
        return SF_RESULT_INVALID_HEADER;
    if (!location_known(name))
        return TIME_ZONE_NOT_FOUND;
    tz_rule *rule = malloc(sizeof *rule);
    if (!rule)
        return KR_OUT_OF_RESOURCE;
    tz_utc(rule);
    rl_result rc = write_rule(r, rule);
    free(rule);
    return rc;
}

static rl_result zone_get_rule_version(rl_ipc_req *r)
{
    memcpy(rl_ipc_out(r, 16), g_time.rule_version, 16);
    return KR_SUCCESS;
}

static rl_result zone_get_location_name_and_updated_time(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 0x40);
    hos_lock();
    memcpy(o, g_time.location, LOCATION_NAME_SIZE);
    memcpy(o + 0x28, &g_time.location_updated.base_time, 8);
    memcpy(o + 0x30, g_time.source, 16);
    hos_unlock();
    return KR_SUCCESS;
}

static bool parse_binary(const rl_ipc_req *r, uint32_t index, tz_rule *rule)
{
    rl_ipc_buf b = rl_ipc_auto_in(r, index);
    uint8_t *data;
    bool ok = false;
    if (!b.addr || b.size == 0 || b.size > 0x10000)
        return false;
    data = malloc(b.size);
    if (data && rl_ipc_buf_read(&b, data, b.size))
        ok = tz_parse(rule, data, b.size);
    free(data);
    return ok;
}

static rl_result zone_set_name_with_rule(rl_ipc_req *r)
{
    const zone_object *z = r->self->state;
    char name[LOCATION_NAME_SIZE];
    if (!z->writable)
        return TIME_PERMISSION_DENIED;
    if (!read_location(r, name))
        return SF_RESULT_INVALID_HEADER;
    tz_rule *rule = malloc(sizeof *rule);
    if (!rule)
        return KR_OUT_OF_RESOURCE;
    rl_result rc = TIME_CONVERSION_FAILED;
    if (parse_binary(r, 0, rule)) {
        hos_lock();
        g_time.rule = *rule;
        memcpy(g_time.location, name, LOCATION_NAME_SIZE);
        g_time.location_updated.base_time = steady_seconds();
        hos_unlock();
        rc = KR_SUCCESS;
    }
    free(rule);
    return rc;
}

static rl_result zone_parse_binary(rl_ipc_req *r)
{
    tz_rule *rule = malloc(sizeof *rule);
    if (!rule)
        return KR_OUT_OF_RESOURCE;
    rl_result rc = TIME_CONVERSION_FAILED;
    if (parse_binary(r, 0, rule))
        rc = write_rule(r, rule);
    free(rule);
    return rc;
}

static tz_rule *read_rule_arg(const rl_ipc_req *r)
{
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    tz_rule *rule;
    if (b.size != TZ_RULE_SIZE)
        return NULL;
    rule = malloc(sizeof *rule);
    if (rule && (!rl_ipc_buf_read(&b, rule, TZ_RULE_SIZE) || !tz_check(rule))) {
        free(rule);
        rule = NULL;
    }
    return rule;
}

static rl_result calendar_out(rl_ipc_req *r, const tz_rule *rule, int64_t t)
{
    tz_calendar cal;
    if (!tz_to_calendar(rule, t, &cal))
        return TIME_OUT_OF_RANGE;
    uint8_t *o = rl_ipc_out(r, 0x20);
    put_calendar(o, o + 8, &cal);
    return KR_SUCCESS;
}

static rl_result zone_to_calendar_time(rl_ipc_req *r)
{
    int64_t t;
    if (!read_in(r, 0, &t, 8))
        return SF_RESULT_INVALID_HEADER;
    tz_rule *rule = read_rule_arg(r);
    if (!rule)
        return TIME_CONVERSION_FAILED;
    rl_result rc = calendar_out(r, rule, t);
    free(rule);
    return rc;
}

static rl_result zone_to_calendar_time_with_my_rule(rl_ipc_req *r)
{
    int64_t t;
    if (!read_in(r, 0, &t, 8))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    rl_result rc = calendar_out(r, &g_time.rule, t);
    hos_unlock();
    return rc;
}

static rl_result posix_out(rl_ipc_req *r, const tz_rule *rule)
{
    uint8_t c[8];
    uint16_t year;
    int64_t t;
    if (!read_in(r, 0, c, 8))
        return SF_RESULT_INVALID_HEADER;
    memcpy(&year, c, 2);
    t = tz_to_posix(rule, (int16_t)year, c[2], c[3], c[4], c[5], c[6]);
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    if (!rl_ipc_buf_write(&b, &t, 8))
        return KR_INVALID_MEM_STATE;
    *(uint32_t *)rl_ipc_out(r, 4) = 1;
    return KR_SUCCESS;
}

static rl_result zone_to_posix_time(rl_ipc_req *r)
{
    tz_rule *rule = read_rule_arg(r);
    if (!rule)
        return TIME_CONVERSION_FAILED;
    rl_result rc = posix_out(r, rule);
    free(rule);
    return rc;
}

static rl_result zone_to_posix_time_with_my_rule(rl_ipc_req *r)
{
    hos_lock();
    rl_result rc = posix_out(r, &g_time.rule);
    hos_unlock();
    return rc;
}

static const rl_ipc_cmd zone_cmds[] = {
    { 0, "GetDeviceLocationName", zone_get_device_location_name },
    { 1, "SetDeviceLocationName", zone_set_device_location_name },
    { 2, "GetTotalLocationNameCount", zone_get_total_location_name_count },
    { 3, "LoadLocationNameList", zone_load_location_name_list },
    { 4, "LoadTimeZoneRule", zone_load_time_zone_rule },
    { 5, "GetTimeZoneRuleVersion", zone_get_rule_version },
    { 6, "GetDeviceLocationNameAndUpdatedTime", zone_get_location_name_and_updated_time },
    { 7, "SetDeviceLocationNameWithTimeZoneRule", zone_set_name_with_rule },
    { 8, "ParseTimeZoneBinary", zone_parse_binary },
    { 100, "ToCalendarTime", zone_to_calendar_time },
    { 101, "ToCalendarTimeWithMyRule", zone_to_calendar_time_with_my_rule },
    { 201, "ToPosixTime", zone_to_posix_time },
    { 202, "ToPosixTimeWithMyRule", zone_to_posix_time_with_my_rule },
};

static const rl_ipc_class zone_class = { "time::ITimeZoneService", zone_cmds, sizeof zone_cmds / sizeof zone_cmds[0],
                                         POINTER_BUFFER, free };

static rl_result open_clock(rl_ipc_req *r, uint32_t clock, uint32_t writable_mask)
{
    const service_state *s = r->self->state;
    clock_object *o = malloc(sizeof *o);
    if (!o)
        return KR_OUT_OF_RESOURCE;
    o->clock = clock;
    o->writable = (s->permissions & writable_mask) != 0;
    return rl_ipc_push_object(r, rl_srv_object(&system_clock_class, o));
}

static rl_result svc_get_user_clock(rl_ipc_req *r) { return open_clock(r, CLOCK_LOCAL_USER, PERM_USER_WRITABLE); }

static rl_result svc_get_network_clock(rl_ipc_req *r) { return open_clock(r, CLOCK_NETWORK, PERM_NETWORK_WRITABLE); }

static rl_result svc_get_local_clock(rl_ipc_req *r) { return open_clock(r, CLOCK_LOCAL_USER, PERM_LOCAL_WRITABLE); }

static rl_result svc_get_steady_clock(rl_ipc_req *r)
{
    const service_state *s = r->self->state;
    bool *writable = malloc(sizeof *writable);
    if (!writable)
        return KR_OUT_OF_RESOURCE;
    *writable = (s->permissions & PERM_STEADY_WRITABLE) != 0;
    return rl_ipc_push_object(r, rl_srv_object(&steady_clock_class, writable));
}

static rl_result svc_get_time_zone_service(rl_ipc_req *r)
{
    const service_state *s = r->self->state;
    zone_object *z = malloc(sizeof *z);
    if (!z)
        return KR_OUT_OF_RESOURCE;
    z->writable = (s->permissions & PERM_TIME_ZONE_WRITABLE) != 0;
    z->psc = s->psc;
    return rl_ipc_push_object(r, rl_srv_object(&zone_class, z));
}

static rl_result svc_get_shared_memory(rl_ipc_req *r)
{
    hos_lock();
    if (!g_time.shm) {
        g_time.shm = rl_hos_shmem_create(SHMEM_SIZE, HOS_PERM_R);
        if (!g_time.shm) {
            hos_unlock();
            return KR_OUT_OF_RESOURCE;
        }
        shm_put_steady();
        clock_follow_host(CLOCK_LOCAL_USER);
        clock_follow_host(CLOCK_NETWORK);
        shm_put_clock(CLOCK_LOCAL_USER);
        shm_put_clock(CLOCK_NETWORK);
        shm_put_auto_correction();
    }
    rl_result rc = rl_ipc_push_copy(r, &g_time.shm->obj);
    hos_unlock();
    return rc;
}

static rl_result svc_set_steady_internal_offset(rl_ipc_req *r)
{
    const service_state *s = r->self->state;
    if (s->psc)
        return TIME_NOT_IMPLEMENTED;
    if (!(s->permissions & PERM_STEADY_WRITABLE))
        return TIME_PERMISSION_DENIED;
    int64_t ns;
    if (!read_in(r, 0, &ns, 8))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    g_time.internal_offset_ns = ns;
    shm_put_steady();
    hos_unlock();
    return KR_SUCCESS;
}

#define RTC_EPOCH_OFFSET 946684800ll

static rl_result svc_get_steady_rtc_value(rl_ipc_req *r)
{
    const service_state *s = r->self->state;
    if (s->psc)
        return TIME_NOT_IMPLEMENTED;
    *(uint64_t *)rl_ipc_out(r, 8) = (uint64_t)(wall_seconds() - RTC_EPOCH_OFFSET);
    return KR_SUCCESS;
}

static rl_result svc_is_auto_correction_enabled(rl_ipc_req *r)
{
    hos_lock();
    *(uint8_t *)rl_ipc_out(r, 1) = g_time.auto_correction;
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result svc_set_auto_correction_enabled(rl_ipc_req *r)
{
    const service_state *s = r->self->state;
    uint8_t v;
    if (!(s->permissions & PERM_USER_WRITABLE))
        return TIME_PERMISSION_DENIED;
    if (!read_in(r, 0, &v, 1))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    g_time.auto_correction = v != 0;
    shm_put_auto_correction();
    g_time.auto_correction_updated.time_point = steady_seconds();
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result svc_get_initial_year(rl_ipc_req *r)
{
    const service_state *s = r->self->state;
    if (s->psc)
        return TIME_NOT_IMPLEMENTED;
    *(uint32_t *)rl_ipc_out(r, 4) = STANDARD_USER_CLOCK_INITIAL_YEAR;
    return KR_SUCCESS;
}

static rl_result svc_is_network_accuracy_sufficient(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = 1;
    return KR_SUCCESS;
}

static rl_result svc_get_auto_correction_updated_time(rl_ipc_req *r)
{
    hos_lock();
    steady_ctx t;
    t.base_time = g_time.auto_correction_updated.time_point;
    memcpy(t.source, g_time.source, 16);
    memcpy(rl_ipc_out(r, sizeof t), &t, sizeof t);
    hos_unlock();
    return KR_SUCCESS;
}

static rl_result svc_calculate_monotonic_base(rl_ipc_req *r)
{
    clock_ctx c;
    if (!read_in(r, 0, &c, sizeof c))
        return SF_RESULT_INVALID_HEADER;
    if (memcmp(c.source, g_time.source, 16) != 0)
        return TIME_MISMATCH;
    hos_lock();
    int64_t ticks_s = floor_div(ticks_ns(), NS_PER_SECOND);
    *(int64_t *)rl_ipc_out(r, 8) = c.offset + steady_seconds() - ticks_s;
    hos_unlock();
    return KR_SUCCESS;
}

static bool snapshot_time(const uint8_t *steady_tp, const clock_ctx *c, int64_t *out)
{
    int64_t tp;
    memcpy(&tp, steady_tp, 8);
    if (memcmp(steady_tp + 8, c->source, 16) != 0)
        return false;
    *out = tp + c->offset;
    return true;
}

static rl_result build_snapshot(uint8_t type, const clock_ctx *user, const clock_ctx *network, uint8_t *s)
{
    uint8_t steady_tp[24];
    int64_t user_time = 0, network_time = 0;
    tz_calendar cal;
    write_steady_time_point(steady_tp);
    memset(s, 0, SNAPSHOT_SIZE);
    memcpy(s, user, 32);
    memcpy(s + 0x20, network, 32);
    if (!snapshot_time(steady_tp, user, &user_time))
        return TIME_MISMATCH;
    if (!snapshot_time(steady_tp, network, &network_time))
        network_time = 0;
    memcpy(s + 0x40, &user_time, 8);
    memcpy(s + 0x48, &network_time, 8);
    if (!tz_to_calendar(&g_time.rule, user_time, &cal))
        return TIME_OUT_OF_RANGE;
    put_calendar(s + 0x50, s + 0x60, &cal);
    if (!tz_to_calendar(&g_time.rule, network_time, &cal))
        return TIME_OUT_OF_RANGE;
    put_calendar(s + 0x58, s + 0x78, &cal);
    memcpy(s + 0x90, steady_tp, 24);
    memcpy(s + 0xA8, g_time.location, LOCATION_NAME_SIZE);
    s[0xCC] = g_time.auto_correction;
    s[0xCD] = type;
    return KR_SUCCESS;
}

static rl_result write_snapshot(rl_ipc_req *r, const uint8_t *s)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    if (b.size != SNAPSHOT_SIZE || !rl_ipc_buf_write(&b, s, SNAPSHOT_SIZE))
        return KR_INVALID_MEM_STATE;
    return KR_SUCCESS;
}

static rl_result svc_get_clock_snapshot(rl_ipc_req *r)
{
    uint8_t type, s[SNAPSHOT_SIZE];
    if (!read_in(r, 0, &type, 1))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    clock_follow_host(CLOCK_LOCAL_USER);
    clock_follow_host(CLOCK_NETWORK);
    rl_result rc = build_snapshot(type, &g_time.clocks[CLOCK_LOCAL_USER].ctx, &g_time.clocks[CLOCK_NETWORK].ctx, s);
    hos_unlock();
    return rc == KR_SUCCESS ? write_snapshot(r, s) : rc;
}

static rl_result svc_get_clock_snapshot_from_contexts(rl_ipc_req *r)
{
    uint8_t type, s[SNAPSHOT_SIZE];
    clock_ctx user, network;
    if (!read_in(r, 0, &type, 1) || !read_in(r, 8, &user, 32) || !read_in(r, 40, &network, 32))
        return SF_RESULT_INVALID_HEADER;
    hos_lock();
    rl_result rc = build_snapshot(type, &user, &network, s);
    hos_unlock();
    return rc == KR_SUCCESS ? write_snapshot(r, s) : rc;
}

static bool read_snapshots(const rl_ipc_req *r, uint8_t a[SNAPSHOT_SIZE], uint8_t b[SNAPSHOT_SIZE])
{
    rl_ipc_buf ba = rl_ipc_auto_in(r, 0), bb = rl_ipc_auto_in(r, 1);
    return ba.size == SNAPSHOT_SIZE && bb.size == SNAPSHOT_SIZE && rl_ipc_buf_read(&ba, a, SNAPSHOT_SIZE) &&
           rl_ipc_buf_read(&bb, b, SNAPSHOT_SIZE);
}

static rl_result svc_difference_by_user(rl_ipc_req *r)
{
    uint8_t a[SNAPSHOT_SIZE], b[SNAPSHOT_SIZE];
    int64_t oa, ob, ns;
    if (!read_snapshots(r, a, b))
        return KR_INVALID_MEM_STATE;
    memcpy(&oa, a, 8);
    memcpy(&ob, b, 8);
    ns = (ob - oa) * NS_PER_SECOND;
    if (memcmp(a + 16, b + 16, 16) != 0 || (a[0xCC] && b[0xCC]))
        ns = 0;
    *(int64_t *)rl_ipc_out(r, 8) = ns;
    return KR_SUCCESS;
}

static rl_result svc_span_between(rl_ipc_req *r)
{
    uint8_t a[SNAPSHOT_SIZE], b[SNAPSHOT_SIZE];
    int64_t ta, tb, na, nb, span;
    if (!read_snapshots(r, a, b))
        return KR_INVALID_MEM_STATE;
    memcpy(&ta, a + 0x90, 8);
    memcpy(&tb, b + 0x90, 8);
    memcpy(&na, a + 0x48, 8);
    memcpy(&nb, b + 0x48, 8);
    if (memcmp(a + 0x98, b + 0x98, 16) == 0)
        span = tb - ta;
    else if (na != 0 && nb != 0)
        span = nb - na;
    else
        return TIME_NOT_FOUND;
    *(int64_t *)rl_ipc_out(r, 8) = span * NS_PER_SECOND;
    return KR_SUCCESS;
}

#define STATIC_COMMANDS                                                                                        \
    { 0, "GetStandardUserSystemClock", svc_get_user_clock },                                                  \
    { 1, "GetStandardNetworkSystemClock", svc_get_network_clock },                                            \
    { 2, "GetStandardSteadyClock", svc_get_steady_clock },                                                    \
    { 3, "GetTimeZoneService", svc_get_time_zone_service },                                                   \
    { 4, "GetStandardLocalSystemClock", svc_get_local_clock },                                                \
    { 5, "GetEphemeralNetworkSystemClock", svc_get_network_clock },                                           \
    { 20, "GetSharedMemoryNativeHandle", svc_get_shared_memory },                                             \
    { 50, "SetStandardSteadyClockInternalOffset", svc_set_steady_internal_offset },                           \
    { 51, "GetStandardSteadyClockRtcValue", svc_get_steady_rtc_value },                                       \
    { 100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", svc_is_auto_correction_enabled },           \
    { 101, "SetStandardUserSystemClockAutomaticCorrectionEnabled", svc_set_auto_correction_enabled },         \
    { 102, "GetStandardUserSystemClockInitialYear", svc_get_initial_year },                                   \
    { 200, "IsStandardNetworkSystemClockAccuracySufficient", svc_is_network_accuracy_sufficient },            \
    { 201, "GetStandardUserSystemClockAutomaticCorrectionUpdatedTime", svc_get_auto_correction_updated_time }, \
    { 300, "CalculateMonotonicSystemClockBaseTimePoint", svc_calculate_monotonic_base },                      \
    { 400, "GetClockSnapshot", svc_get_clock_snapshot },                                                      \
    { 401, "GetClockSnapshotFromSystemClockContext", svc_get_clock_snapshot_from_contexts },                  \
    { 500, "CalculateStandardUserSystemClockDifferenceByUser", svc_difference_by_user },                      \
    { 501, "CalculateSpanBetween", svc_span_between }

static const rl_ipc_cmd static_cmds[] = { STATIC_COMMANDS };

#define STATIC_CLASS(var, nm) \
    static const rl_ipc_class var = { nm, static_cmds, sizeof static_cmds / sizeof static_cmds[0], POINTER_BUFFER, NULL }

STATIC_CLASS(class_time_u, "time:u");
STATIC_CLASS(class_time_a, "time:a");
STATIC_CLASS(class_time_s, "time:s");
STATIC_CLASS(class_time_su, "time:su");
STATIC_CLASS(class_time_r, "time:r");

static const service_state state_u = { 0, false };
static const service_state state_a = { PERM_ADMIN, false };
static const service_state state_s = { PERM_NETWORK_WRITABLE, true };
static const service_state state_su = { 0, true };
static const service_state state_r = { PERM_STEADY_WRITABLE, false };

static rl_ipc_object *make_u(void) { return rl_srv_object(&class_time_u, (void *)(uintptr_t)&state_u); }
static rl_ipc_object *make_a(void) { return rl_srv_object(&class_time_a, (void *)(uintptr_t)&state_a); }
static rl_ipc_object *make_s(void) { return rl_srv_object(&class_time_s, (void *)(uintptr_t)&state_s); }
static rl_ipc_object *make_su(void) { return rl_srv_object(&class_time_su, (void *)(uintptr_t)&state_su); }
static rl_ipc_object *make_r(void) { return rl_srv_object(&class_time_r, (void *)(uintptr_t)&state_r); }

void rl_srv_install_time(void)
{
    time_reset();
    rl_ipc_register_service("time:u", make_u);
    rl_ipc_register_service("time:a", make_a);
    rl_ipc_register_service("time:s", make_s);
    rl_ipc_register_service("time:su", make_su);
    rl_ipc_register_service("time:r", make_r);
}
