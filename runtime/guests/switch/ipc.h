/**
 * @file ipc.h
 * @brief HIPC message parsing and building, CMIF (with domains) and TIPC,
 * sessions, the service registry and the stub service base.
 *
 * Services run in the runtime's process, so buffers are read and written in
 * place in guest memory instead of being copied by a kernel. The message
 * layout follows switchbrew.org and libnx (ISC) sf/hipc.h, cmif.h and tipc.h.
 */
#ifndef RELIFT_IPC_H
#define RELIFT_IPC_H

#include "hos.h"

/** @brief Service framework results (module 10). */
#define SF_RESULT_NOT_SUPPORTED RL_MAKE_RESULT(10, 1)
#define SF_RESULT_INVALID_HEADER RL_MAKE_RESULT(10, 202)
#define SF_RESULT_UNKNOWN_COMMAND RL_MAKE_RESULT(10, 221)
#define SF_RESULT_INVALID_IN_OBJECT RL_MAKE_RESULT(10, 230)
#define SF_RESULT_TARGET_NOT_FOUND RL_MAKE_RESULT(10, 261)
#define SF_RESULT_OUT_OF_DOMAIN_ENTRIES RL_MAKE_RESULT(10, 301)
/** @brief Service manager results (module 21). */
#define SM_RESULT_INVALID_CLIENT RL_MAKE_RESULT(21, 2)
#define SM_RESULT_INVALID_SERVICE_NAME RL_MAKE_RESULT(21, 6)
#define SM_RESULT_NOT_REGISTERED RL_MAKE_RESULT(21, 7)

/** @brief "SFCI": CMIF request header magic. */
#define CMIF_IN_MAGIC 0x49434653u
/** @brief "SFCO": CMIF response header magic. */
#define CMIF_OUT_MAGIC 0x4F434653u

/** @brief HIPC command types. */
enum {
    HIPC_CLOSE = 2,
    HIPC_REQUEST = 4,
    HIPC_CONTROL = 5,
    HIPC_REQUEST_CTX = 6,
    HIPC_CONTROL_CTX = 7,
    TIPC_CLOSE = 15,
    TIPC_BASE = 16
};

/** @brief Most descriptors of one kind, handles and objects in one message. */
#define RL_IPC_MAX 16
/**
 * @brief Pointer buffer size of a service that does not set one. The SDK
 * sends X buffers through the server's pointer buffer and aborts with hipc
 * PointerBufferTooSmall when QueryPointerBufferSize answers less than it
 * needs, so no service answers 0.
 */
#define RL_IPC_POINTER_BUFFER_DEFAULT 0x1000u
/** @brief Size of the IPC message area at the start of the TLS slot. */
#define RL_IPC_MSG 0x100u

typedef struct rl_ipc_req rl_ipc_req;
typedef struct rl_ipc_object rl_ipc_object;

/** @brief A command handler. Returns the result put in the response. */
typedef rl_result rl_ipc_handler(rl_ipc_req *r);

/** @brief One command of an interface. */
typedef struct rl_ipc_cmd {
    /** @brief Command id. */
    uint32_t id;
    /** @brief Name for traces. */
    const char *name;
    /** @brief Handler. */
    rl_ipc_handler *fn;
} rl_ipc_cmd;

/** @brief An interface: name, commands and pointer buffer size. */
typedef struct rl_ipc_class {
    /** @brief Interface or service name for traces and reports. */
    const char *name;
    /** @brief Commands, any order. */
    const rl_ipc_cmd *cmds;
    /** @brief Number of commands. */
    uint32_t ncmds;
    /** @brief Pointer buffer size reported by QueryPointerBufferSize on a session to this class; 0 selects RL_IPC_POINTER_BUFFER_DEFAULT. */
    uint16_t pointer_buffer_size;
    /** @brief Frees state when the last reference goes; may be NULL. */
    void (*destroy)(void *state);
} rl_ipc_class;

/** @brief An interface instance (a service session's object or a domain object). */
struct rl_ipc_object {
    /** @brief Reference count. */
    uint32_t refs;
    /** @brief Interface. */
    const rl_ipc_class *cls;
    /** @brief Interface state. */
    void *state;
};

/** @brief Creates an object with one reference. */
RL_RUNTIME_API rl_ipc_object *rl_ipc_object_new(const rl_ipc_class *cls, void *state);
/** @brief Adds a reference. */
RL_RUNTIME_API void rl_ipc_object_ref(rl_ipc_object *o);
/** @brief Drops a reference. */
RL_RUNTIME_API void rl_ipc_object_unref(rl_ipc_object *o);

/** @brief Client session kernel object. */
/** @brief A domain's object table, shared by every session of the domain (CloneCurrentObject on a domain session clones the session, not the domain). */
typedef struct hos_domain {
    /** @brief Sessions that use it. Lock taken. */
    uint32_t refs;
    /** @brief Objects by id (index 0 unused). */
    rl_ipc_object *obj[64];
} hos_domain;

typedef struct hos_session {
    /** @brief Base. */
    hos_kobj obj;
    /** @brief Object the session talks to while not a domain. */
    rl_ipc_object *root;
    /** @brief True once converted to a domain. */
    bool domain;
    /** @brief The domain's objects, shared with the sessions cloned from this one (NULL until converted). */
    struct hos_domain *dt;
    /** @brief Set by a Close request. */
    bool closed;
} hos_session;

/** @brief Creates a session to o (adds a reference to o) and puts it in the handle table. Lock taken. */
RL_RUNTIME_API rl_result rl_ipc_session_open(rl_ipc_object *o, uint32_t *handle);

/** @brief A buffer descriptor after parsing. */
typedef struct rl_ipc_buf {
    /** @brief Guest address. */
    uint64_t addr;
    /** @brief Size in bytes. */
    uint64_t size;
    /** @brief Static index (X) or buffer mode (A, B, W). */
    uint32_t attr;
} rl_ipc_buf;

/** @brief A parsed request and the response being built. */
struct rl_ipc_req {
    /** @brief Target object. */
    rl_ipc_object *self;
    /** @brief Session the request came on. */
    hos_session *session;
    /** @brief Command id. */
    uint32_t cmd;
    /** @brief True for TIPC. */
    bool tipc;
    /** @brief True when the client sent its pid. */
    bool has_pid;
    /** @brief Client pid when has_pid. */
    uint64_t pid;
    /** @brief Raw input data after the CMIF header (or the TIPC data words). */
    const uint8_t *in;
    /** @brief Bytes available at in. */
    uint32_t in_size;
    /** @brief Send statics (X), send (A), receive (B), exchange (W) buffers and receive list (C). */
    rl_ipc_buf x[RL_IPC_MAX], a[RL_IPC_MAX], b[RL_IPC_MAX], w[RL_IPC_MAX], c[RL_IPC_MAX];
    /** @brief Counts of the above. */
    uint32_t nx, na, nb, nw, nc;
    /** @brief Copy and move handles from the client. */
    uint32_t in_copy[RL_IPC_MAX], in_move[RL_IPC_MAX];
    /** @brief Counts of the above. */
    uint32_t n_in_copy, n_in_move;
    /** @brief Domain input objects. */
    rl_ipc_object *in_objects[RL_IPC_MAX];
    /** @brief Number of domain input objects. */
    uint32_t n_in_objects;
    /** @brief Output raw data. */
    _Alignas(16) uint8_t out[RL_IPC_MSG];
    /** @brief Bytes of output raw data. */
    uint32_t out_size;
    /** @brief Output copy and move handles. */
    uint32_t out_copy[RL_IPC_MAX], out_move[RL_IPC_MAX];
    /** @brief Counts of the above. */
    uint32_t n_out_copy, n_out_move;
    /** @brief Output objects (each holds a reference). */
    rl_ipc_object *out_objects[RL_IPC_MAX];
    /** @brief Number of output objects. */
    uint32_t n_out_objects;
    /** @brief Set by rl_ipc_stop: the request ends the process with a named report. */
    bool stop;
    /** @brief The report's tag and summary when stop is set. */
    const char *stop_tag;
    char stop_what[256];
};

/**
 * @brief Ends the process after this request with a stop report naming what
 * the service could not answer (tag, then printf-style text); the handler
 * still returns its result.
 */
RL_RUNTIME_API void rl_ipc_stop(rl_ipc_req *r, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/**
 * @brief The stop a request on this host thread asked for, cleared as it is
 * taken: true with *tag and what filled (n bytes), false when none.
 */
bool rl_ipc_take_stop(const char **tag, char *what, size_t n);

/** @brief Returns a zeroed output area of size bytes (the command's raw output). */
RL_RUNTIME_API void *rl_ipc_out(rl_ipc_req *r, uint32_t size);
/** @brief Reads input raw data; returns false when the client sent fewer than size bytes. */
RL_RUNTIME_API bool rl_ipc_in(const rl_ipc_req *r, void *dst, uint32_t size);
/** @brief Returns object o to the client (takes over the caller's reference). */
RL_RUNTIME_API rl_result rl_ipc_push_object(rl_ipc_req *r, rl_ipc_object *o);
/** @brief Returns a copy handle to kernel object k. */
RL_RUNTIME_API rl_result rl_ipc_push_copy(rl_ipc_req *r, hos_kobj *k);
/** @brief Returns a move handle to kernel object k. */
RL_RUNTIME_API rl_result rl_ipc_push_move(rl_ipc_req *r, hos_kobj *k);
/** @brief Input buffer i of an auto-select pair: the A buffer if nonempty, else the X static with index i. */
RL_RUNTIME_API rl_ipc_buf rl_ipc_auto_in(const rl_ipc_req *r, uint32_t i);
/** @brief Output buffer i of an auto-select pair: the B buffer if nonempty, else receive list entry i. */
RL_RUNTIME_API rl_ipc_buf rl_ipc_auto_out(const rl_ipc_req *r, uint32_t i);
/** @brief Copies size bytes from a client buffer; false when it is too small or not readable. */
RL_RUNTIME_API bool rl_ipc_buf_read(const rl_ipc_buf *b, void *dst, uint64_t size);
/** @brief Copies size bytes into a client buffer; false when it is too small or not writable. */
RL_RUNTIME_API bool rl_ipc_buf_write(const rl_ipc_buf *b, const void *src, uint64_t size);

/**
 * @brief Handles svcSendSyncRequest: parses the message at guest msg_addr,
 * runs the handler and writes the response. *stop is set under --strict when
 * an unknown command must end the process.
 */
rl_result hos_ipc_request(uint32_t handle, uint64_t msg_addr, uint64_t msg_size, bool *stop);

/** @brief Factory of a service's session object. */
typedef rl_ipc_object *rl_ipc_factory(void);
/** @brief Registers a service under name (at most 8 characters) for sm:. */
RL_RUNTIME_API void rl_ipc_register_service(const char *name, rl_ipc_factory *factory);
/** @brief Removes every registered service (tests). */
RL_RUNTIME_API void rl_ipc_reset_services(void);
/** @brief Encodes a service name as sm: does (little-endian, zero padded). */
uint64_t rl_ipc_name(const char *name);
/** @brief Finds a registered service's factory by encoded name. */
rl_ipc_factory *hos_ipc_find_service(uint64_t name);
/** @brief Number of registered services. */
uint32_t hos_ipc_service_count(void);
/** @brief Encoded name of registered service i, 0 when out of range. */
uint64_t hos_ipc_service_name(uint32_t i);

/** @brief svcConnectToNamedPort: "sm:" only. */
rl_result hos_connect_named_port(uint32_t *out, uint64_t name_addr);
/** @brief The sm: interface object for a new port connection. */
rl_ipc_object *hos_sm_open(void);

#endif
