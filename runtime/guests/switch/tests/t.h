/**
 * @file t.h
 * @brief Helpers for the Switch guest tests, over the core test runner.
 *
 * Tests drive the runtime the way lifted code does: registers in an rl_ctx,
 * rl_svc(c, mem, imm), results read back from c->x.
 */
#ifndef RELIFT_T_H
#define RELIFT_T_H

#include "hos.h"
#include "ipc.h"
#include "runner.h"

#include <stdio.h>
#include <string.h>

/** @brief Starts the runtime for a test with cfg (NULL for defaults) and attaches the calling thread; returns its context. */
rl_ctx *t_boot(const rl_hos_config *cfg);
/**
 * @brief Calls svc imm with x0..x5 = a0..a5 on c, as lifted code does, and
 * returns w0. The return code of rl_svc is stored in *t_svc_rc.
 */
uint32_t t_svc(rl_ctx *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
/** @brief rl_svc return code of the last t_svc call. */
extern int t_svc_rc;
/**
 * @brief svcSleepThread(ns) on the calling guest thread, keeping x0..x5 and
 * t_svc_rc: a wait that gives the core to the threads the test started.
 */
void t_sleep(uint64_t ns);
/**
 * @brief Gives the core to the other guest threads until cond holds. There is
 * no bound: the runner's per-test alarm ends a test whose condition never
 * comes, so a loaded host cannot make it fail by being slow.
 */
#define T_WAIT_UNTIL(cond) \
    do { \
        while (!(cond)) \
            t_sleep(1000000); \
    } while (0)
/** @brief Grows the heap to size with svcSetHeapSize and returns its guest address (0 on failure). */
uint64_t t_heap(rl_ctx *c, uint64_t size);
/** @brief Registers lifted-signature host functions at guest addresses (one unit). */
void t_guest_fns(const uint64_t *addrs, rl_fn const *fns, uint32_t n);
/** @brief Guest address of the k-th test function slot in the code region. */
static inline uint64_t t_code(uint32_t k) { return 0x8000000ull + 0x100ull * k; }

/** @brief A free, host-page aligned guest address in the ASLR region outside every other region, for shared memory. */
uint64_t t_free_map_addr(void);

/** @brief svc numbers used by the tests. */
enum {
    SVC_SET_HEAP_SIZE = 0x01,
    SVC_SET_MEMORY_PERMISSION = 0x02,
    SVC_SET_MEMORY_ATTRIBUTE = 0x03,
    SVC_MAP_MEMORY = 0x04,
    SVC_UNMAP_MEMORY = 0x05,
    SVC_QUERY_MEMORY = 0x06,
    SVC_EXIT_PROCESS = 0x07,
    SVC_CREATE_THREAD = 0x08,
    SVC_START_THREAD = 0x09,
    SVC_EXIT_THREAD = 0x0A,
    SVC_SLEEP_THREAD = 0x0B,
    SVC_GET_THREAD_PRIORITY = 0x0C,
    SVC_SET_THREAD_PRIORITY = 0x0D,
    SVC_GET_THREAD_CORE_MASK = 0x0E,
    SVC_SET_THREAD_CORE_MASK = 0x0F,
    SVC_GET_CURRENT_PROCESSOR_NUMBER = 0x10,
    SVC_SIGNAL_EVENT = 0x11,
    SVC_CLEAR_EVENT = 0x12,
    SVC_MAP_SHARED_MEMORY = 0x13,
    SVC_UNMAP_SHARED_MEMORY = 0x14,
    SVC_CREATE_TRANSFER_MEMORY = 0x15,
    SVC_CLOSE_HANDLE = 0x16,
    SVC_RESET_SIGNAL = 0x17,
    SVC_WAIT_SYNCHRONIZATION = 0x18,
    SVC_CANCEL_SYNCHRONIZATION = 0x19,
    SVC_ARBITRATE_LOCK = 0x1A,
    SVC_ARBITRATE_UNLOCK = 0x1B,
    SVC_WAIT_PROCESS_WIDE_KEY_ATOMIC = 0x1C,
    SVC_SIGNAL_PROCESS_WIDE_KEY = 0x1D,
    SVC_GET_SYSTEM_TICK = 0x1E,
    SVC_CONNECT_TO_NAMED_PORT = 0x1F,
    SVC_SEND_SYNC_REQUEST = 0x21,
    SVC_GET_PROCESS_ID = 0x24,
    SVC_GET_THREAD_ID = 0x25,
    SVC_BREAK = 0x26,
    SVC_OUTPUT_DEBUG_STRING = 0x27,
    SVC_GET_INFO = 0x29,
    SVC_MAP_PHYSICAL_MEMORY = 0x2C,
    SVC_UNMAP_PHYSICAL_MEMORY = 0x2D,
    SVC_GET_RESOURCE_LIMIT_LIMIT_VALUE = 0x30,
    SVC_GET_RESOURCE_LIMIT_CURRENT_VALUE = 0x31,
    SVC_WAIT_FOR_ADDRESS = 0x34,
    SVC_SIGNAL_TO_ADDRESS = 0x35,
    SVC_CREATE_EVENT = 0x45
};

#endif
