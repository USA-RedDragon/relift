/**
 * @file sm.c
 * @brief The service registry, the "sm:" named port and the sm: interface
 * (CMIF and TIPC share command ids).
 *
 * Commands and results from switchbrew.org (Services API) and Ryujinx (MIT)
 * IUserInterface: 0 RegisterClient, 1 GetServiceHandle, 2 RegisterService,
 * 3 UnregisterService, 4 DetachClient.
 */
#include "ipc.h"

#include <stdlib.h>
#include <string.h>

/** @brief Most registered services. */
#define MAX_SERVICES 256

static struct {
    uint64_t name;
    rl_ipc_factory *factory;
} g_services[MAX_SERVICES];
static uint32_t g_service_count;

uint64_t rl_ipc_name(const char *name)
{
    uint64_t v = 0;
    for (int i = 0; i < 8 && name[i]; i++)
        v |= (uint64_t)(uint8_t)name[i] << (8 * i);
    return v;
}

void rl_ipc_register_service(const char *name, rl_ipc_factory *factory)
{
    uint64_t n = rl_ipc_name(name);
    hos_lock();
    for (uint32_t i = 0; i < g_service_count; i++)
        if (g_services[i].name == n) {
            g_services[i].factory = factory;
            hos_unlock();
            return;
        }
    if (g_service_count < MAX_SERVICES) {
        g_services[g_service_count].name = n;
        g_services[g_service_count].factory = factory;
        g_service_count++;
    }
    hos_unlock();
}

void rl_ipc_reset_services(void)
{
    hos_lock();
    g_service_count = 0;
    hos_unlock();
}

rl_ipc_factory *hos_ipc_find_service(uint64_t name)
{
    rl_ipc_factory *f = NULL;
    hos_lock();
    for (uint32_t i = 0; i < g_service_count; i++)
        if (g_services[i].name == name)
            f = g_services[i].factory;
    hos_unlock();
    return f;
}

uint32_t hos_ipc_service_count(void)
{
    hos_lock();
    uint32_t n = g_service_count;
    hos_unlock();
    return n;
}

uint64_t hos_ipc_service_name(uint32_t i)
{
    hos_lock();
    uint64_t n = i < g_service_count ? g_services[i].name : 0;
    hos_unlock();
    return n;
}

/** @brief Per-session sm: state. */
typedef struct sm_state {
    bool registered;
} sm_state;

static rl_result sm_register_client(rl_ipc_req *r)
{
    sm_state *st = r->self->state;
    if (!r->has_pid && !r->tipc)
        return SM_RESULT_INVALID_CLIENT;
    st->registered = true;
    return KR_SUCCESS;
}

static bool valid_name(uint64_t name)
{
    bool end = false;
    if (name == 0)
        return false;
    for (int i = 0; i < 8; i++) {
        uint8_t c = (uint8_t)(name >> (8 * i));
        if (c == 0)
            end = true;
        else if (end)
            return false;
    }
    return true;
}

static rl_result sm_get_service_handle(rl_ipc_req *r)
{
    sm_state *st = r->self->state;
    uint64_t name;
    if (!st->registered)
        return SM_RESULT_INVALID_CLIENT;
    if (!rl_ipc_in(r, &name, 8) || !valid_name(name))
        return SM_RESULT_INVALID_SERVICE_NAME;
    rl_ipc_factory *f = hos_ipc_find_service(name);
    if (!f) {
        char s[9] = { 0 };
        memcpy(s, &name, 8);
        (void)rl_report_unknown(name ^ 0x5300000000000000ull, "sm", "GetServiceHandle(\"%s\"): no such service", s);
        return SM_RESULT_NOT_REGISTERED;
    }
    rl_ipc_object *o = f();
    if (!o)
        return KR_OUT_OF_RESOURCE;
    uint32_t h;
    rl_result rc = rl_ipc_session_open(o, &h);
    rl_ipc_object_unref(o);
    if (rc == KR_SUCCESS)
        r->out_move[r->n_out_move++] = h;
    return rc;
}

static rl_result sm_detach_client(rl_ipc_req *r)
{
    sm_state *st = r->self->state;
    st->registered = false;
    return KR_SUCCESS;
}

static const rl_ipc_cmd sm_cmds[] = {
    { 0, "RegisterClient", sm_register_client },
    { 1, "GetServiceHandle", sm_get_service_handle },
    { 4, "DetachClient", sm_detach_client },
};

static const rl_ipc_class sm_class = { "sm:", sm_cmds, sizeof sm_cmds / sizeof sm_cmds[0], 0, free };

rl_ipc_object *hos_sm_open(void)
{
    sm_state *st = calloc(1, sizeof *st);
    if (!st)
        return NULL;
    rl_ipc_object *o = rl_ipc_object_new(&sm_class, st);
    if (!o)
        free(st);
    return o;
}

rl_result hos_connect_named_port(uint32_t *out, uint64_t name_addr)
{
    char name[13] = { 0 };
    hos_lock();
    bool ok = hos_user_range(name_addr, 1, HOS_PERM_R);
    for (int i = 0; ok && i < 12; i++) {
        if (!hos_user_range(name_addr + (uint64_t)i, 1, HOS_PERM_R)) {
            ok = false;
            break;
        }
        name[i] = (char)hos.vm.base[name_addr + (uint64_t)i];
        if (!name[i])
            break;
    }
    hos_unlock();
    if (!ok)
        return KR_USER_COPY_FAILED;
    if (strcmp(name, "sm:") != 0) {
        if (rl_once(rl_ipc_name(name) ^ 0x504F525400000000ull))
            rl_log(RL_LOG_WARN, "kernel", "ConnectToNamedPort(\"%s\"): no such port", name);
        return KR_NOT_FOUND;
    }
    rl_ipc_object *o = hos_sm_open();
    if (!o)
        return KR_OUT_OF_RESOURCE;
    rl_result rc = rl_ipc_session_open(o, out);
    rl_ipc_object_unref(o);
    return rc;
}
