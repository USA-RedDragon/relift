/**
 * @file svc.c
 * @brief rl_svc, the supervisor call dispatcher, svcGetInfo, debug output,
 * svcBreak, resource limits and process start-up.
 *
 * Register use per svc follows the AArch64 ABI of switchbrew.org (SVC) and
 * Ryujinx (MIT) Syscall and SyscallHandler: arguments in x0..x7, result in
 * w0, outputs from x1.
 */
#include "ipc.h"
#include "hid.h"
#include "services.h"
#include "vi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const svc_names[0x80] = {
    [0x01] = "SetHeapSize",
    [0x02] = "SetMemoryPermission",
    [0x03] = "SetMemoryAttribute",
    [0x04] = "MapMemory",
    [0x05] = "UnmapMemory",
    [0x06] = "QueryMemory",
    [0x07] = "ExitProcess",
    [0x08] = "CreateThread",
    [0x09] = "StartThread",
    [0x0A] = "ExitThread",
    [0x0B] = "SleepThread",
    [0x0C] = "GetThreadPriority",
    [0x0D] = "SetThreadPriority",
    [0x0E] = "GetThreadCoreMask",
    [0x0F] = "SetThreadCoreMask",
    [0x10] = "GetCurrentProcessorNumber",
    [0x11] = "SignalEvent",
    [0x12] = "ClearEvent",
    [0x13] = "MapSharedMemory",
    [0x14] = "UnmapSharedMemory",
    [0x15] = "CreateTransferMemory",
    [0x16] = "CloseHandle",
    [0x17] = "ResetSignal",
    [0x18] = "WaitSynchronization",
    [0x19] = "CancelSynchronization",
    [0x1A] = "ArbitrateLock",
    [0x1B] = "ArbitrateUnlock",
    [0x1C] = "WaitProcessWideKeyAtomic",
    [0x1D] = "SignalProcessWideKey",
    [0x1E] = "GetSystemTick",
    [0x1F] = "ConnectToNamedPort",
    [0x21] = "SendSyncRequest",
    [0x22] = "SendSyncRequestWithUserBuffer",
    [0x24] = "GetProcessId",
    [0x25] = "GetThreadId",
    [0x26] = "Break",
    [0x27] = "OutputDebugString",
    [0x29] = "GetInfo",
    [0x30] = "GetResourceLimitLimitValue",
    [0x31] = "GetResourceLimitCurrentValue",
    [0x34] = "WaitForAddress",
    [0x35] = "SignalToAddress",
    [0x36] = "SynchronizePreemptionState",
    [0x37] = "GetResourceLimitPeakValue",
    [0x45] = "CreateEvent",
};

/** @brief What the Switch guest tells the core: 4 KiB pages, at most 39 address bits, the 19.2 MHz counter. */
static const rl_guest_desc hos_guest = {
    .name = "switch",
    .page_size = HOS_PAGE,
    .address_bits = 39,
    .tick_hz = HOS_TICK_HZ,
};

static void reslimit_destroy(hos_kobj *o) { free(o); }

static int64_t res_limit(uint32_t which)
{
    switch (which) {
    case RES_MEMORY:
        return (int64_t)hos.cfg.total_memory;
    case RES_THREADS:
        return hos.cfg.max_threads;
    case RES_EVENTS:
        return 700;
    case RES_TRANSFER_MEMORY:
        return 200;
    default:
        return 900;
    }
}

static int64_t res_current(uint32_t which)
{
    switch (which) {
    case RES_MEMORY:
        return (int64_t)hos_used_memory();
    case RES_THREADS:
        return hos.live_threads;
    default:
        return 0;
    }
}

static rl_result reslimit_value(uint32_t h, uint32_t which, bool current, uint64_t *out)
{
    if (which >= RES_COUNT)
        return KR_INVALID_ENUM_VALUE;
    hos_lock();
    hos_kobj *o = hos_handle_get(h, KOBJ_RESOURCE_LIMIT);
    if (o)
        *out = (uint64_t)(current ? res_current(which) : res_limit(which));
    hos_unlock();
    if (!o)
        return KR_INVALID_HANDLE;
    hos_kobj_unref(o);
    return KR_SUCCESS;
}

static uint64_t thread_cpu_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return rl_ns_to_ticks((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

static rl_result get_info(uint64_t *out, uint32_t id, uint32_t handle, uint64_t sub)
{
    const hos_layout *l = &hos.lay;
    *out = 0;
    if (id <= 7 || (id >= 12 && id <= 18) || (id >= 20 && id <= 24) || id == 28) {
        if (sub != 0)
            return KR_INVALID_COMBINATION;
        if (handle != HOS_CUR_PROCESS)
            return KR_INVALID_HANDLE;
        switch (id) {
        case 0: *out = hos.cfg.core_mask; break;
        case 1: *out = hos.cfg.priority_mask; break;
        case 2: *out = l->alias_start; break;
        case 3: *out = l->alias_end - l->alias_start; break;
        case 4: *out = l->heap_start; break;
        case 5: *out = l->heap_end - l->heap_start; break;
        case 6: case 21: *out = hos.cfg.total_memory; break;
        case 7: case 22: hos_lock(); *out = hos_used_memory(); hos_unlock(); break;
        case 12: *out = l->aslr_start; break;
        case 13: *out = l->aslr_end - l->aslr_start; break;
        case 14: *out = l->stack_start; break;
        case 15: *out = l->stack_end - l->stack_start; break;
        case 16: case 17: *out = 0; break;
        case 18: *out = hos.cfg.program_id; break;
        case 20: *out = hos.user_exception_ctx; break;
        case 23: *out = 1; break;
        case 24: *out = hos.cfg.max_threads - hos.live_threads; break;
        case 28: *out = 0; break;
        default: break;
        }
        return KR_SUCCESS;
    }
    switch (id) {
    case 8:
        if (handle != 0)
            return KR_INVALID_HANDLE;
        if (sub != 0)
            return KR_INVALID_COMBINATION;
        return KR_SUCCESS;
    case 9: {
        if (handle != 0)
            return KR_INVALID_HANDLE;
        if (sub != 0)
            return KR_INVALID_COMBINATION;
        uint32_t h = 0;
        hos_lock();
        rl_result rc = hos_handle_add(&hos.reslimit->obj, &h);
        hos_unlock();
        *out = h;
        return rc;
    }
    case 10:
        if (handle != 0)
            return KR_INVALID_HANDLE;
        if (sub != UINT64_MAX && sub >= 4)
            return KR_INVALID_COMBINATION;
        return KR_SUCCESS;
    case 11:
        if (handle != 0)
            return KR_INVALID_HANDLE;
        if (sub >= 4)
            return KR_INVALID_COMBINATION;
        *out = hos.entropy[sub];
        return KR_SUCCESS;
    case 25: {
        if (sub != UINT64_MAX && sub >= 4)
            return KR_INVALID_COMBINATION;
        hos_lock();
        hos_thread *t = hos_thread_get(handle);
        hos_unlock();
        if (!t)
            return KR_INVALID_HANDLE;
        if (t == hos_current())
            *out = thread_cpu_ticks();
        hos_kobj_unref(&t->obj);
        return KR_SUCCESS;
    }
    case 26:
        *out = 1;
        return KR_SUCCESS;
    default:
        (void)rl_report_unknown(0x474554494E464F00ull ^ id, "svc", "GetInfo: unknown id %u (handle 0x%x, sub 0x%llx)", id,
                                handle, (unsigned long long)sub);
        return KR_INVALID_ENUM_VALUE;
    }
}

static void debug_string(uint64_t addr, uint64_t size)
{
    char buf[1024];
    if (size >= sizeof buf)
        size = sizeof buf - 1;
    hos_lock();
    bool ok = hos_user_range(addr, size, HOS_PERM_R);
    if (ok)
        memcpy(buf, hos.vm.base + addr, size);
    hos_unlock();
    if (!ok)
        return;
    buf[size] = 0;
    while (size && (buf[size - 1] == '\n' || buf[size - 1] == '\r'))
        buf[--size] = 0;
    buf[size] = '\n';
    fwrite(buf, 1, size + 1, stdout);
    fflush(stdout);
}

static rl_result close_handle(uint32_t h)
{
    if (h == HOS_CUR_THREAD || h == HOS_CUR_PROCESS)
        return KR_SUCCESS;
    hos_lock();
    hos_kobj *o = hos_handle_remove(h);
    hos_unlock();
    if (!o)
        return KR_INVALID_HANDLE;
    hos_kobj_unref(o);
    return KR_SUCCESS;
}

static rl_result process_id(uint32_t h, uint64_t *out)
{
    if (h == HOS_CUR_PROCESS) {
        *out = HOS_PID;
        return KR_SUCCESS;
    }
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    hos_unlock();
    if (!t)
        return KR_INVALID_HANDLE;
    hos_kobj_unref(&t->obj);
    *out = HOS_PID;
    return KR_SUCCESS;
}

static int unknown_svc(rl_ctx *c, uint32_t imm)
{
    if (rl_report_unknown(0x5356430000000000ull | imm, "svc", "unimplemented svc 0x%02x%s%s (lr 0x%llx, host return %p)",
                          imm, imm < 0x80 && svc_names[imm] ? " " : "", imm < 0x80 && svc_names[imm] ? svc_names[imm] : "",
                          (unsigned long long)c->x[30], __builtin_return_address(0))) {
        rl_log(RL_LOG_ERROR, "svc", "stopping: --strict");
        return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
    }
    c->x[0] = KR_NOT_IMPLEMENTED;
    return RL_OK;
}

static int dispatch(rl_ctx *c, uint32_t imm)
{
    uint64_t *x = c->x;
    uint32_t w0 = (uint32_t)x[0], w1 = (uint32_t)x[1], w2 = (uint32_t)x[2], w3 = (uint32_t)x[3];
    rl_result rc;
    switch (imm) {
    case 0x01: {
        uint64_t a = 0;
        rc = hos_set_heap_size(x[1], &a);
        x[1] = a;
        break;
    }
    case 0x02: rc = hos_set_memory_permission(x[0], x[1], w2); break;
    case 0x03: rc = hos_set_memory_attribute(x[0], x[1], w2, w3); break;
    case 0x04: rc = hos_map_memory(x[0], x[1], x[2]); break;
    case 0x05: rc = hos_unmap_memory(x[0], x[1], x[2]); break;
    case 0x06: {
        uint32_t pi = 0;
        rc = hos_query_memory(x[0], x[2], &pi);
        x[1] = pi;
        break;
    }
    case 0x07:
        return rl_pend_exit_process(c, 0);
    case 0x08: {
        uint32_t h = 0;
        rc = hos_create_thread(&h, x[1], x[2], x[3], (int32_t)x[4], (int32_t)x[5]);
        x[1] = h;
        break;
    }
    case 0x09: rc = hos_start_thread(w0); break;
    case 0x0A:
        return rl_pend_exit_thread(c);
    case 0x0B:
        hos_sleep_thread((int64_t)x[0]);
        return RL_OK;
    case 0x0C: {
        int32_t p = 0;
        rc = hos_get_thread_priority(w1, &p);
        x[1] = (uint32_t)p;
        break;
    }
    case 0x0D: rc = hos_set_thread_priority(w0, (int32_t)w1); break;
    case 0x0E: {
        int32_t core = 0;
        uint64_t mask = 0;
        rc = hos_get_thread_core_mask(w2, &core, &mask);
        x[1] = (uint32_t)core;
        x[2] = mask;
        break;
    }
    case 0x0F: rc = hos_set_thread_core_mask(w0, (int32_t)w1, x[2]); break;
    case 0x10: {
        hos_thread *t = hos_current();
        hos_lock();
        x[0] = t ? (uint32_t)hos_sched_processor(t) : 0;
        hos_unlock();
        return RL_OK;
    }
    case 0x11: rc = hos_signal_event(w0); break;
    case 0x12: rc = hos_clear_event(w0); break;
    case 0x13: rc = hos_map_shared_memory(w0, x[1], x[2], w3); break;
    case 0x14: rc = hos_unmap_shared_memory(w0, x[1], x[2]); break;
    case 0x15: {
        uint32_t h = 0;
        rc = hos_create_transfer_memory(&h, x[1], x[2], w3);
        x[1] = h;
        break;
    }
    case 0x16: rc = close_handle(w0); break;
    case 0x17: rc = hos_reset_signal(w0); break;
    case 0x18: {
        int32_t idx = 0;
        rc = hos_wait_sync(&idx, x[1], (int32_t)w2, (int64_t)x[3]);
        x[1] = (uint32_t)idx;
        break;
    }
    case 0x19: rc = hos_cancel_sync(w0); break;
    case 0x1A: rc = hos_arbitrate_lock(w0, x[1], w2); break;
    case 0x1B: rc = hos_arbitrate_unlock(x[0]); break;
    case 0x1C: rc = hos_wait_process_wide_key(x[0], x[1], w2, (int64_t)x[3]); break;
    case 0x1D:
        hos_signal_process_wide_key(x[0], (int32_t)w1);
        rc = KR_SUCCESS;
        break;
    case 0x1E:
        x[0] = rl_mrs_cntpct(c);
        return RL_OK;
    case 0x1F: {
        uint32_t h = 0;
        rc = hos_connect_named_port(&h, x[1]);
        x[1] = h;
        break;
    }
    case 0x21:
    case 0x22: {
        bool stop = false;
        hos_thread *t = hos_current();
        hos_sched_leave();
        if (imm == 0x21)
            rc = t ? hos_ipc_request(w0, t->tls, RL_IPC_MSG, &stop) : KR_INVALID_THREAD;
        else
            rc = hos_ipc_request(w2, x[0], x[1], &stop);
        hos_sched_return();
        if (stop) {
            rl_log(RL_LOG_ERROR, "ipc", "stopping: --strict");
            return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
        }
        break;
    }
    case 0x24: {
        uint64_t pid = 0;
        rc = process_id(w1, &pid);
        x[1] = pid;
        break;
    }
    case 0x25: {
        uint64_t id = 0;
        rc = hos_get_thread_id(w1, &id);
        x[1] = id;
        break;
    }
    case 0x26:
        if (w0 & 0x80000000u) {
            rl_log(RL_LOG_INFO, "guest", "svcBreak notification 0x%x (0x%llx, 0x%llx)", w0, (unsigned long long)x[1],
                   (unsigned long long)x[2]);
            rc = KR_SUCCESS;
            break;
        }
        rl_log(RL_LOG_ERROR, "guest", "svcBreak reason 0x%x (0x%llx, 0x%llx), lr 0x%llx", w0, (unsigned long long)x[1],
               (unsigned long long)x[2], (unsigned long long)x[30]);
        return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
    case 0x27:
        debug_string(x[0], x[1]);
        rc = KR_SUCCESS;
        break;
    case 0x29: {
        uint64_t v = 0;
        rc = get_info(&v, w1, w2, x[3]);
        x[1] = v;
        break;
    }
    case 0x30:
    case 0x37: {
        uint64_t v = 0;
        rc = reslimit_value(w1, w2, false, &v);
        x[1] = v;
        break;
    }
    case 0x31: {
        uint64_t v = 0;
        rc = reslimit_value(w1, w2, true, &v);
        x[1] = v;
        break;
    }
    case 0x34: rc = hos_wait_for_address(x[0], w1, (int32_t)w2, (int64_t)x[3]); break;
    case 0x35: rc = hos_signal_to_address(x[0], w1, (int32_t)w2, (int32_t)w3); break;
    case 0x36: rc = KR_SUCCESS; break;
    case 0x45: {
        uint32_t wh = 0, rh = 0;
        rc = hos_create_event(&wh, &rh);
        x[1] = wh;
        x[2] = rh;
        break;
    }
    default:
        return unknown_svc(c, imm);
    }
    x[0] = rc;
    return RL_OK;
}

static int svc_traced(rl_ctx *c, uint32_t imm);

int rl_svc(rl_ctx *c, uint8_t *mem, uint32_t imm)
{
    (void)mem;
    rl_clrex(&c->excl);
    /* The guest's FP flags stay out of the runtime's own FP work. */
    uint32_t flags = rl_a64_flags_enter();
    int r = rl_cfg()->trace_os ? svc_traced(c, imm) : dispatch(c, imm);
    hos_sched_point();
    rl_a64_flags_leave(flags);
    return r;
}

static int svc_traced(rl_ctx *c, uint32_t imm)
{
    uint64_t a[4] = { c->x[0], c->x[1], c->x[2], c->x[3] };
    uint64_t t0 = rl_clock_ns();
    int r = dispatch(c, imm);
    const char *name = imm < 0x80 && svc_names[imm] ? svc_names[imm] : "?";
    rl_log(RL_LOG_INFO, "svc", "0x%02x %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) -> 0x%x%s (%llu us)", imm, name,
           (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3],
           (uint32_t)c->x[0], r == RL_PENDING ? " pending" : "", (unsigned long long)((rl_clock_ns() - t0) / 1000));
    return r;
}

void rl_hos_default_config(rl_hos_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->address_space_type = 3;
    cfg->code_start = 0x8000000;
    cfg->code_size = 0x10000000;
    cfg->program_id = 0x0100000000010000ull;
    cfg->total_memory = 0xCD500000ull;
    cfg->core_mask = 0x7;
    cfg->priority_mask = 0xFFFFFFFFFF000000ull;
    cfg->max_threads = 256;
    cfg->main_prio = 44;
    cfg->main_core = 0;
    cfg->main_stack_size = 0x100000;
}

static void fill_entropy(void)
{
    if (!rl_host_random(hos.entropy, sizeof hos.entropy)) {
        uint64_t s = rl_clock_ns();
        for (int i = 0; i < 4; i++) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            hos.entropy[i] = s;
        }
    }
}

int rl_hos_init(const rl_hos_config *cfg)
{
    pthread_mutexattr_t ma;
    if (hos.up)
        return -1;
    if (!rl_cfg()->loaded)
        rl_config_load();
    if (rl_core_init(&hos_guest, rl_a64_cpu()) != 0)
        return -1;
    memset(&hos, 0, sizeof hos);
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&hos.lock, &ma);
    pthread_mutexattr_destroy(&ma);
    if (cfg)
        hos.cfg = *cfg;
    else
        rl_hos_default_config(&hos.cfg);
    hos.next_thread_id = 1;
    hos_sched_init();
    if (hos_mem_init() != 0)
        return -1;
    hos.reslimit = calloc(1, sizeof *hos.reslimit);
    if (!hos.reslimit)
        return -1;
    hos_kobj_init(&hos.reslimit->obj, KOBJ_RESOURCE_LIMIT, reslimit_destroy);
    fill_entropy();
    hos_lock();
    hos.user_exception_ctx = hos_tls_alloc();
    hos_unlock();
    hos.up = true;
    rl_srv_install_all();
    return 0;
}

void rl_hos_shutdown(void)
{
    if (!hos.up)
        return;
    rl_hid_stop();
    rl_vi_stop();
    hos_kobj *drop[HOS_HANDLES];
    uint32_t n = 0;
    hos_audio_shutdown();
    hos_lock();
    for (uint32_t i = 0; i < HOS_HANDLES; i++)
        if (hos.handles[i]) {
            drop[n++] = hos.handles[i];
            hos.handles[i] = NULL;
        }
    hos_unlock();
    for (uint32_t i = 0; i < n; i++)
        hos_kobj_unref(drop[i]);
    hos_kobj_unref(&hos.reslimit->obj);
    rl_set_mem_base(NULL);
    rl_vm_destroy(&hos.vm);
    pthread_mutex_destroy(&hos.lock);
    hos.up = false;
}
