/**
 * @file tc.h
 * @brief A test client for services: builds CMIF and TIPC requests in the
 * TLS message area, sends them with svcSendSyncRequest and parses the
 * response, the way a libnx program does. Objects returned by a service are
 * move handles, or domain ids once the session is converted to a domain.
 */
#ifndef RELIFT_TC_H
#define RELIFT_TC_H

#include "t.h"

/** @brief A request being built. */
typedef struct tc_req {
    uint32_t type;
    uint32_t object_id;
    uint32_t cmd;
    bool send_pid;
    bool tipc;
    uint8_t data[160];
    uint32_t data_size;
    uint64_t x_addr[4], x_size[4];
    uint32_t nx;
    uint64_t a_addr[4], a_size[4];
    uint32_t na;
    uint64_t b_addr[4], b_size[4];
    uint32_t nb;
    uint64_t c_addr[4], c_size[4];
    uint32_t nc;
    uint32_t copy[4];
    uint32_t ncopy;
    uint32_t in_objects[4];
    uint32_t n_in_objects;
    uint8_t domain_cmd;
    bool raw_magic_bad;
} tc_req;

/** @brief A parsed response. */
typedef struct tc_res {
    uint32_t svc_rc;
    uint32_t result;
    uint8_t data[160];
    uint32_t copy[8], move[8], objects[8];
    uint32_t ncopy, nmove, nobjects;
} tc_res;


/** @brief One service object seen from the client: a session handle, plus a domain object id when the session is a domain. */
typedef struct tc_obj {
    /** @brief Session handle. */
    uint32_t handle;
    /** @brief Domain object id, 0 when the session is not a domain. */
    uint32_t id;
    /** @brief True after ConvertCurrentObjectToDomain. */
    bool domain;
} tc_obj;

/** @brief A request for command cmd with no data. */
tc_req tc_new(uint32_t cmd);
/** @brief Appends size bytes of raw data to q. */
void tc_put(tc_req *q, const void *p, uint32_t n);
/** @brief Appends a 32-bit value to q's raw data. */
void tc_put32(tc_req *q, uint32_t v);
/** @brief Appends a 64-bit value to q's raw data. */
void tc_put64(tc_req *q, uint64_t v);
/** @brief The 32-bit word at byte offset off of r's raw output. */
uint32_t tc_u32(const tc_res *r, uint32_t off);
/** @brief The 64-bit value at byte offset off of r's raw output. */
uint64_t tc_u64(const tc_res *r, uint32_t off);
/** @brief Sends q on the raw session handle h. */
tc_res tc_call(rl_ctx *c, uint32_t h, const tc_req *q, bool domain);
/** @brief Sends q to o (as a domain object when o is in a domain). */
tc_res tc_send(rl_ctx *c, tc_obj o, tc_req q);
/** @brief Sends command cmd with no data to o and returns its result code (the svc code when the call itself failed). */
uint32_t tc_cmd(rl_ctx *c, tc_obj o, uint32_t cmd);
/** @brief Connects to sm: and registers the client; returns the session handle or 0. scratch is a guest address with room for 8 bytes. */
uint32_t tc_connect_sm(rl_ctx *c, uint64_t scratch);
/** @brief sm:GetServiceHandle for name; the result is in the response. */
tc_res tc_sm_get(rl_ctx *c, uint32_t sm, const char *name, bool tipc);
/** @brief sm:RegisterClient with a pid. */
uint32_t tc_sm_register(rl_ctx *c, uint32_t sm, bool tipc);
/** @brief Opens service name through sm: and returns its object (handle 0 when it failed); scratch is a guest address. */
tc_obj tc_open_service(rl_ctx *c, uint64_t scratch, const char *name);
/** @brief Returns the result code of sm:GetServiceHandle for name, on a fresh sm: session. */
uint32_t tc_open_service_result(rl_ctx *c, uint64_t scratch, const char *name);
/** @brief Converts o's session to a domain; o.id becomes 1. Returns the result code. */
uint32_t tc_to_domain(rl_ctx *c, tc_obj *o);
/** @brief Sends q to o and takes the object it returns (a move handle, or a domain id) into *out. Returns the result code. */
uint32_t tc_sub(rl_ctx *c, tc_obj o, tc_req q, tc_obj *out);
/** @brief Closes a session handle with the kernel. */
void tc_close(rl_ctx *c, uint32_t handle);
/**
 * @brief The runner's hang report (core/tests/runner.c): each guest thread's id, state, priority, cores and what it
 * waits on, and each core's selected and holding thread. Read without the kernel lock, which a hung thread may hold.
 */
void tc_hang_report(void);

#endif
