/**
 * @file srv_lm.c
 * @brief The log manager service lm: the SDK opens a logger at start-up and
 * sends its log packets there. The runtime prints each packet's message.
 *
 * Command ids and the packet layout follow Ryujinx (MIT) Services/Lm: a
 * 0x18-byte header (pid, thread id, flags, severity, verbosity, payload
 * size), then fields of a one-byte key, a ULEB128 size and the bytes.
 */
#include "services.h"

#include <string.h>

#define LM_HEADER_SIZE 0x18u
#define LM_KEY_MESSAGE 2u
#define LM_KEY_FILE 4u
#define LM_KEY_FUNCTION 5u
#define LM_KEY_MODULE 6u
#define LM_MAX_PACKET 0x4000u

/** @brief The text of field key in the payload p of n bytes, as a length and a pointer; 0 when absent. */
static uint32_t field(const uint8_t *p, uint32_t n, uint8_t key, const uint8_t **at)
{
    uint32_t i = 0;
    while (i < n) {
        uint8_t k = p[i++];
        uint32_t size = 0, shift = 0;
        while (i < n && shift < 28) {
            uint8_t b = p[i++];
            size |= (uint32_t)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        if (size > n - i)
            return 0;
        if (k == key) {
            *at = p + i;
            return size;
        }
        i += size;
    }
    return 0;
}

/** @brief Prints one log packet: module, function and message when present. */
static void print_packet(const uint8_t *pkt, uint32_t n)
{
    if (n < LM_HEADER_SIZE)
        return;
    uint32_t payload;
    memcpy(&payload, pkt + 0x14, 4);
    const uint8_t *p = pkt + LM_HEADER_SIZE;
    uint32_t len = n - LM_HEADER_SIZE < payload ? n - LM_HEADER_SIZE : payload;
    const uint8_t *msg = NULL, *mod = NULL, *fn = NULL;
    uint32_t nm = field(p, len, LM_KEY_MESSAGE, &msg), nmod = field(p, len, LM_KEY_MODULE, &mod),
             nfn = field(p, len, LM_KEY_FUNCTION, &fn);
    while (nm && (msg[nm - 1] == '\n' || msg[nm - 1] == 0))
        nm--;
    if (!nm)
        return;
    rl_log(RL_LOG_INFO, "lm", "[%.*s%s%.*s] %.*s", (int)nmod, nmod ? (const char *)mod : "", nfn ? " " : "", (int)nfn,
           nfn ? (const char *)fn : "", (int)nm, (const char *)msg);
}

static rl_result logger_log(rl_ipc_req *r)
{
    static uint8_t pkt[LM_MAX_PACKET];
    rl_ipc_buf in = rl_ipc_auto_in(r, 0);
    uint64_t n = in.size < sizeof pkt ? in.size : sizeof pkt;
    if (n && rl_ipc_buf_read(&in, pkt, n))
        print_packet(pkt, (uint32_t)n);
    return KR_SUCCESS;
}

static const rl_ipc_cmd logger_cmds[] = {
    { 0, "Log", logger_log },
    RL_SRV_STUB(1, "SetDestination"),
};

static const rl_ipc_class logger_class = { "lm::ILogger", logger_cmds, sizeof logger_cmds / sizeof logger_cmds[0], 0,
                                           NULL };

static rl_result open_logger(rl_ipc_req *r) { return rl_ipc_push_object(r, rl_srv_object(&logger_class, NULL)); }

static const rl_ipc_cmd service_cmds[] = {
    { 0, "OpenLogger", open_logger },
};

static const rl_ipc_class service_class = { "lm", service_cmds, sizeof service_cmds / sizeof service_cmds[0], 0, NULL };

static rl_ipc_object *service_factory(void) { return rl_srv_object(&service_class, NULL); }

void rl_srv_install_lm(void) { rl_ipc_register_service("lm", service_factory); }
