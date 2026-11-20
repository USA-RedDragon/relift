/**
 * @file srv_ssl.c
 * @brief The TLS services ssl and ssl:s (nn::ssl::sf::ISslService and
 * ISslServiceForSystem) of a console that is offline: contexts and connection
 * objects are created, options and settings are kept, and every connection
 * fails for want of a socket (the bsd service has none to give). Command ids
 * and layouts come from the SDK's own client (the proxies behind nn::ssl);
 * error values and the command list follow Eglantine (GPL-3,
 * src/core/hle/service/ssl/ssl.cpp and ssl_backend.h) as values only. The
 * commands a game calls that this does not answer stop with relift's unknown
 * command report.
 */
#include "services.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/** @brief The ssl service's error module. */
#define SSL_MODULE 123
#define SSL_RESULT_NO_SOCKET RL_MAKE_RESULT(SSL_MODULE, 103)
#define SSL_RESULT_INVALID_SOCKET RL_MAKE_RESULT(SSL_MODULE, 106)

/** @brief Number of ISslContext objects alive in the process, as GetContextCount reports. */
static atomic_uint context_count;
/** @brief Source of the ids the pki import commands return. */
static atomic_uint_fast64_t next_pki_id = 1;

/** @brief Reply of a command that has no output. */
static rl_result ok(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

/** @brief Reply of a command whose output is size zero bytes. */
static rl_result zeros(rl_ipc_req *r, uint32_t size) { return rl_ipc_out(r, size) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER; }

static rl_result zero_u32(rl_ipc_req *r) { return zeros(r, 4); }

/** @brief A command that needs a socket, which no connection has: the output is zero and the result says so. */
static rl_result no_socket_u32(rl_ipc_req *r)
{
    zeros(r, 4);
    return SSL_RESULT_NO_SOCKET;
}

static rl_result no_socket_none(rl_ipc_req *r)
{
    (void)r;
    return SSL_RESULT_NO_SOCKET;
}

/** @brief SetSocketDescriptor (0): an i32 descriptor; the bsd service has no open socket, so the descriptor is invalid. */
static rl_result connection_set_socket_descriptor(rl_ipc_req *r)
{
    int32_t fd;
    return rl_srv_in(r, &fd, 4) ? SSL_RESULT_INVALID_SOCKET : SF_RESULT_INVALID_HEADER;
}

/** @brief The state a connection keeps for the Get commands that read back what the client set. */
typedef struct connection {
    char host[0x100];
    uint32_t verify_option;
    uint32_t session_cache_mode;
    uint32_t renegotiation_mode;
} connection;

static connection *conn(rl_ipc_req *r) { return r->self->state; }

/** @brief SetHostName (1): the host name as a send buffer. */
static rl_result connection_set_host_name(rl_ipc_req *r)
{
    connection *c = conn(r);
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    memset(c->host, 0, sizeof c->host);
    uint64_t n = b.size < sizeof c->host - 1 ? b.size : sizeof c->host - 1;
    return rl_ipc_buf_read(&b, c->host, n) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief SetVerifyOption (2): a u32. */
static rl_result connection_set_verify_option(rl_ipc_req *r)
{
    return rl_srv_in(r, &conn(r)->verify_option, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief GetHostName (5): the host name as a receive buffer and a u32 length. */
static rl_result connection_get_host_name(rl_ipc_req *r)
{
    connection *c = conn(r);
    uint32_t n = (uint32_t)strlen(c->host);
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    if (n >= b.size || !rl_ipc_buf_write(&b, c->host, (uint64_t)n + 1))
        return SF_RESULT_INVALID_HEADER;
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = n + 1;
    return KR_SUCCESS;
}

/** @brief GetVerifyOption (6): a u32. */
static rl_result connection_get_verify_option(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = conn(r)->verify_option;
    return KR_SUCCESS;
}

/** @brief GetIoMode (7): a u32, 1 (blocking) until a socket is set. */
static rl_result connection_get_io_mode(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 1;
    return KR_SUCCESS;
}

/** @brief DoHandshakeGetServerCert (9): two u32s (certificate size and count) and a result. */
static rl_result connection_handshake_get_server_cert(rl_ipc_req *r)
{
    zeros(r, 8);
    return SSL_RESULT_NO_SOCKET;
}

/** @brief SetSessionCacheMode (17): a u32. */
static rl_result connection_set_session_cache_mode(rl_ipc_req *r)
{
    return rl_srv_in(r, &conn(r)->session_cache_mode, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief GetSessionCacheMode (18): a u32. */
static rl_result connection_get_session_cache_mode(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = conn(r)->session_cache_mode;
    return KR_SUCCESS;
}

/** @brief SetRenegotiationMode (20): a u32. */
static rl_result connection_set_renegotiation_mode(rl_ipc_req *r)
{
    return rl_srv_in(r, &conn(r)->renegotiation_mode, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief GetRenegotiationMode (21): a u32. */
static rl_result connection_get_renegotiation_mode(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = conn(r)->renegotiation_mode;
    return KR_SUCCESS;
}

/** @brief GetVerifyCertErrors (24): a receive buffer and two u32s, empty. */
static rl_result connection_get_verify_cert_errors(rl_ipc_req *r)
{
    zeros(r, 8);
    return KR_SUCCESS;
}

static const rl_ipc_cmd connection_cmds[] = {
    { 0, "SetSocketDescriptor", connection_set_socket_descriptor },
    { 1, "SetHostName", connection_set_host_name },
    { 2, "SetVerifyOption", connection_set_verify_option },
    { 3, "SetIoMode", no_socket_none },
    { 4, "GetSocketDescriptor", no_socket_u32 },
    { 5, "GetHostName", connection_get_host_name },
    { 6, "GetVerifyOption", connection_get_verify_option },
    { 7, "GetIoMode", connection_get_io_mode },
    { 8, "DoHandshake", no_socket_none },
    { 9, "DoHandshakeGetServerCert", connection_handshake_get_server_cert },
    { 10, "Read", no_socket_u32 },
    { 11, "Write", no_socket_u32 },
    { 12, "Pending", no_socket_u32 },
    { 13, "Peek", no_socket_u32 },
    { 14, "Poll", no_socket_u32 },
    { 15, "GetVerifyCertError", ok },
    { 16, "GetNeededServerCertBufferSize", zero_u32 },
    { 17, "SetSessionCacheMode", connection_set_session_cache_mode },
    { 18, "GetSessionCacheMode", connection_get_session_cache_mode },
    { 19, "FlushSessionCache", ok },
    { 20, "SetRenegotiationMode", connection_set_renegotiation_mode },
    { 21, "GetRenegotiationMode", connection_get_renegotiation_mode },
    { 22, "SetOption", ok },
    { 23, "GetOption", zero_u32 },
    { 24, "GetVerifyCertErrors", connection_get_verify_cert_errors },
};

static void connection_destroy(void *state) { free(state); }

static const rl_ipc_class connection_class = { "nn::ssl::sf::ISslConnection", connection_cmds,
                                               sizeof connection_cmds / sizeof connection_cmds[0], 0, connection_destroy };

/** @brief The state a context keeps for GetOption, and the count of connections it made. */
typedef struct context {
    uint32_t option;
    int32_t option_value;
    atomic_uint connections;
} context;

static void context_destroy(void *state)
{
    free(state);
    atomic_fetch_sub(&context_count, 1);
}

/** @brief SetOption (0): u32 option, i32 value. */
static rl_result context_set_option(rl_ipc_req *r)
{
    struct {
        uint32_t option;
        int32_t value;
    } in;
    if (!rl_srv_in(r, &in, 8))
        return SF_RESULT_INVALID_HEADER;
    context *c = r->self->state;
    c->option = in.option;
    c->option_value = in.value;
    return KR_SUCCESS;
}

/** @brief GetOption (1): u32 option; an i32 value, the last one set for it (zero when none was). */
static rl_result context_get_option(rl_ipc_req *r)
{
    uint32_t option;
    if (!rl_srv_in(r, &option, 4))
        return SF_RESULT_INVALID_HEADER;
    context *c = r->self->state;
    int32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = c->option == option ? c->option_value : 0;
    return KR_SUCCESS;
}

/** @brief CreateConnection (2): an ISslConnection. */
static rl_result context_create_connection(rl_ipc_req *r)
{
    connection *c = calloc(1, sizeof *c);
    if (!c)
        return KR_OUT_OF_RESOURCE;
    rl_ipc_object *o = rl_srv_object(&connection_class, c);
    if (!o) {
        free(c);
        return KR_OUT_OF_RESOURCE;
    }
    atomic_fetch_add(&((context *)r->self->state)->connections, 1);
    return rl_ipc_push_object(r, o);
}

/** @brief GetConnectionCount (3): a u32. */
static rl_result context_get_connection_count(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = atomic_load(&((context *)r->self->state)->connections);
    return KR_SUCCESS;
}

/** @brief ImportServerPki, ImportClientPki, RegisterInternalPki and ImportCrl (4, 5, 8, 10): a u64 id of what was added. */
static rl_result context_import(rl_ipc_req *r)
{
    uint64_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = atomic_fetch_add(&next_pki_id, 1);
    return KR_SUCCESS;
}

static const rl_ipc_cmd context_cmds[] = {
    { 0, "SetOption", context_set_option },
    { 1, "GetOption", context_get_option },
    { 2, "CreateConnection", context_create_connection },
    { 3, "GetConnectionCount", context_get_connection_count },
    { 4, "ImportServerPki", context_import },
    { 5, "ImportClientPki", context_import },
    { 6, "RemoveServerPki", ok },
    { 7, "RemoveClientPki", ok },
    { 8, "RegisterInternalPki", context_import },
    { 9, "AddPolicyOid", ok },
    { 10, "ImportCrl", context_import },
    { 11, "RemoveCrl", ok },
};

static const rl_ipc_class context_class = { "nn::ssl::sf::ISslContext", context_cmds,
                                            sizeof context_cmds / sizeof context_cmds[0], 0, context_destroy };

/** @brief CreateContext (0) and CreateContextForSystem (100): u32 SslVersion, 4 bytes of padding, a u64 pid placeholder and the pid; an ISslContext. */
static rl_result service_create_context(rl_ipc_req *r)
{
    struct {
        uint32_t version;
        uint32_t pad;
        uint64_t pid;
    } in;
    if (!rl_srv_in(r, &in, 16))
        return SF_RESULT_INVALID_HEADER;
    context *c = calloc(1, sizeof *c);
    if (!c)
        return KR_OUT_OF_RESOURCE;
    rl_ipc_object *o = rl_srv_object(&context_class, c);
    if (!o) {
        free(c);
        return KR_OUT_OF_RESOURCE;
    }
    atomic_fetch_add(&context_count, 1);
    return rl_ipc_push_object(r, o);
}

/** @brief GetContextCount (1): a u32. */
static rl_result service_get_context_count(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = atomic_load(&context_count);
    return KR_SUCCESS;
}

/** @brief SetInterfaceVersion (5): a u32 version of the client's interface. */
static rl_result service_set_interface_version(rl_ipc_req *r)
{
    uint32_t version;
    return rl_srv_in(r, &version, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static const rl_ipc_cmd service_cmds[] = {
    { 0, "CreateContext", service_create_context },
    { 1, "GetContextCount", service_get_context_count },
    { 5, "SetInterfaceVersion", service_set_interface_version },
};

static const rl_ipc_cmd system_service_cmds[] = {
    { 0, "CreateContext", service_create_context },
    { 1, "GetContextCount", service_get_context_count },
    { 5, "SetInterfaceVersion", service_set_interface_version },
    { 100, "CreateContextForSystem", service_create_context },
};

static const rl_ipc_class service_class = { "nn::ssl::sf::ISslService", service_cmds,
                                            sizeof service_cmds / sizeof service_cmds[0], 0, NULL };
static const rl_ipc_class system_service_class = { "nn::ssl::sf::ISslServiceForSystem", system_service_cmds,
                                                   sizeof system_service_cmds / sizeof system_service_cmds[0], 0, NULL };

static rl_ipc_object *service_factory(void) { return rl_srv_object(&service_class, NULL); }

static rl_ipc_object *system_service_factory(void) { return rl_srv_object(&system_service_class, NULL); }

void rl_srv_install_ssl(void)
{
    rl_ipc_register_service("ssl", service_factory);
    rl_ipc_register_service("ssl:s", system_service_factory);
}
