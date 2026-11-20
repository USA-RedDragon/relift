/**
 * @file srv_nifm.c
 * @brief The network interface service nifm:u (nn::nifm::detail::IStaticService)
 * of a console that is offline: a client creates requests, submits them, and
 * is told no network is available. Command ids, in and out sizes come from
 * the SDK's own client (the nn::nifm proxies); results and states follow
 * Eglantine (GPL-3, src/core/hle/service/nifm/nifm.cpp) as values only. The
 * commands a game calls that this does not answer stop with relift's unknown
 * command report.
 */
#include "hos.h"
#include "services.h"

#include <stdlib.h>

/** @brief The nifm error module. */
#define NIFM_MODULE 110
#define NIFM_RESULT_PENDING_CONNECTION RL_MAKE_RESULT(NIFM_MODULE, 111)
#define NIFM_RESULT_NETWORK_COMMUNICATION_DISABLED RL_MAKE_RESULT(NIFM_MODULE, 1111)

/** @brief nn::nifm::RequestState values of a request on a console with no network. */
enum { REQUEST_NOT_SUBMITTED = 1, REQUEST_ON_HOLD = 2 };

typedef struct request {
    uint32_t state;
    hos_revent *events[2];
} request;

static request *req_of(rl_ipc_req *r) { return r->self->state; }

static void request_destroy(void *state)
{
    request *q = state;
    for (int i = 0; i < 2; i++)
        if (q->events[i])
            hos_kobj_unref(&q->events[i]->obj);
    free(q);
}

/** @brief GetRequestState (0): a u32 state, not submitted until Submit puts it on hold. */
static rl_result request_get_state(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = req_of(r)->state;
    return KR_SUCCESS;
}

/**
 * @brief GetResult (1): a request that was never submitted reports that communication is disabled; one on hold
 * reports the connection pending and falls back to not submitted, as no network answers it.
 */
static rl_result request_get_result(rl_ipc_req *r)
{
    request *q = req_of(r);
    if (q->state == REQUEST_ON_HOLD) {
        q->state = REQUEST_NOT_SUBMITTED;
        return NIFM_RESULT_PENDING_CONNECTION;
    }
    return NIFM_RESULT_NETWORK_COMMUNICATION_DISABLED;
}

/** @brief GetSystemEventReadableHandles (2): two copy handles of events, the first signaled when the state changes. */
static rl_result request_get_events(rl_ipc_req *r)
{
    request *q = req_of(r);
    for (int i = 0; i < 2; i++) {
        if (!q->events[i] && !(q->events[i] = rl_hos_event_create()))
            return KR_OUT_OF_RESOURCE;
        rl_result rc = rl_ipc_push_copy(r, &q->events[i]->obj);
        if (rc != KR_SUCCESS)
            return rc;
    }
    return KR_SUCCESS;
}

/** @brief Submit (4): a request not yet submitted goes on hold and its first event is signaled. */
static rl_result request_submit(rl_ipc_req *r)
{
    request *q = req_of(r);
    if (q->state == REQUEST_NOT_SUBMITTED) {
        q->state = REQUEST_ON_HOLD;
        if (q->events[0])
            rl_hos_event_signal(q->events[0]);
    }
    return KR_SUCCESS;
}

/** @brief SetRequirementPreset (6), SetPriority (8), Set* flags (10 to 17) and the like: the request keeps nothing of them. */
static rl_result request_accept_option(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

/** @brief GetAppletInfo (21) and GetAdditionalInfo (22): a receive buffer of applet data and three u32s, zeros. */
static rl_result request_get_applet_info(rl_ipc_req *r)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    uint8_t zero[256] = { 0 };
    for (uint64_t at = 0; at < b.size; at += sizeof zero) {
        uint64_t n = b.size - at < sizeof zero ? b.size - at : sizeof zero;
        rl_ipc_buf part = { b.addr + at, n, b.attr };
        if (!rl_ipc_buf_write(&part, zero, n))
            return SF_RESULT_INVALID_HEADER;
    }
    return rl_ipc_out(r, 12) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static const rl_ipc_cmd request_cmds[] = {
    { 0, "GetRequestState", request_get_state },
    { 1, "GetResult", request_get_result },
    { 2, "GetSystemEventReadableHandles", request_get_events },
    { 3, "Cancel", rl_srv_stub_ok },
    { 4, "Submit", request_submit },
    { 6, "SetRequirementPreset", request_accept_option },
    { 8, "SetPriority", request_accept_option },
    { 9, "SetNetworkProfileId", request_accept_option },
    { 10, "SetRejectable", request_accept_option },
    { 11, "SetConnectionConfirmationOption", request_accept_option },
    { 12, "SetPersistent", request_accept_option },
    { 13, "SetInstant", request_accept_option },
    { 14, "SetSustainable", request_accept_option },
    { 15, "SetRawPriority", request_accept_option },
    { 16, "SetGreedy", request_accept_option },
    { 17, "SetSharable", request_accept_option },
    { 21, "GetAppletInfo", request_get_applet_info },
};

static const rl_ipc_class request_class = { "nn::nifm::detail::IRequest", request_cmds,
                                            sizeof request_cmds / sizeof request_cmds[0], 0, request_destroy };

static rl_result push(rl_ipc_req *r, const rl_ipc_class *cls, void *state)
{
    rl_ipc_object *o = rl_srv_object(cls, state);
    if (!o) {
        free(state);
        return KR_OUT_OF_RESOURCE;
    }
    return rl_ipc_push_object(r, o);
}

/** @brief CreateRequest (4): a u32 requirement; an IRequest. */
static rl_result create_request(rl_ipc_req *r)
{
    uint32_t requirement;
    if (!rl_srv_in(r, &requirement, 4))
        return SF_RESULT_INVALID_HEADER;
    request *q = calloc(1, sizeof *q);
    if (!q)
        return KR_OUT_OF_RESOURCE;
    q->state = REQUEST_NOT_SUBMITTED;
    return push(r, &request_class, q);
}

/** @brief GetClientId (1): a nonzero id; zero would mean no client. */
static rl_result get_client_id(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 1;
    return KR_SUCCESS;
}

/** @brief GetCurrentNetworkProfile (5), GetCurrentIpAddress (12) and GetCurrentIpConfigInfo (15): there is no connection to describe. */
static rl_result no_connection(rl_ipc_req *r)
{
    (void)r;
    return NIFM_RESULT_NETWORK_COMMUNICATION_DISABLED;
}

/** @brief IsWirelessCommunicationEnabled (17): true, a console that is only offline has its radio on (inferred). */
static rl_result is_wireless_enabled(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 1);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 1;
    return KR_SUCCESS;
}

/** @brief IsEthernetCommunicationEnabled (20), IsAnyInternetRequestAccepted (21), IsAnyForegroundRequestAccepted (22): false. */
static rl_result answer_false(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 1);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/** @brief GetInternetConnectionStatus (18): u8 interface type (1 Wi-Fi), u8 signal bars, u8 status (0 disconnected). */
static rl_result get_internet_connection_status(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 3);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = 1;
    return KR_SUCCESS;
}

static const rl_ipc_cmd general_cmds[] = {
    { 1, "GetClientId", get_client_id },
    { 4, "CreateRequest", create_request },
    { 5, "GetCurrentNetworkProfile", no_connection },
    { 12, "GetCurrentIpAddress", no_connection },
    { 15, "GetCurrentIpConfigInfo", no_connection },
    { 17, "IsWirelessCommunicationEnabled", is_wireless_enabled },
    { 18, "GetInternetConnectionStatus", get_internet_connection_status },
    { 20, "IsEthernetCommunicationEnabled", answer_false },
    { 21, "IsAnyInternetRequestAccepted", answer_false },
    { 22, "IsAnyForegroundRequestAccepted", answer_false },
    { 34, "SetBackgroundRequestEnabled", rl_srv_stub_ok },
};

static const rl_ipc_class general_class = { "nn::nifm::detail::IGeneralService", general_cmds,
                                            sizeof general_cmds / sizeof general_cmds[0], 0, NULL };

/** @brief IStaticService 4 and 5: no input, or a PID and a reserved u64; an IGeneralService. */
static rl_result create_general_service(rl_ipc_req *r) { return push(r, &general_class, NULL); }

static const rl_ipc_cmd static_cmds[] = {
    { 4, "CreateGeneralServiceOld", create_general_service },
    { 5, "CreateGeneralService", create_general_service },
};

static const rl_ipc_class static_class = { "nifm:u", static_cmds, sizeof static_cmds / sizeof static_cmds[0], 0, NULL };

static rl_ipc_object *static_factory(void) { return rl_srv_object(&static_class, NULL); }

void rl_srv_install_nifm(void) { rl_ipc_register_service("nifm:u", static_factory); }
