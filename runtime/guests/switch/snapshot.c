/**
 * @file snapshot.c
 * @brief The Switch layer's part of the diagnostic snapshot (core/snapshot.h): every guest thread with its state, the
 * lifted function at its saved pc and what it waits on, in the terms of the GDB server's `info threads` but taken
 * without stopping or attaching anything. Only with the debug stub built in.
 */
#include "gdbstub.h"
#include "hos.h"

#if defined(RL_DEBUG_STUB)

#include "crash.h"
#include "snapshot.h"

#include <stdio.h>
#include <string.h>

static const char *wait_name(int k)
{
    switch (k) {
    case HW_SYNC:
        return "svcWaitSynchronization";
    case HW_MUTEX:
        return "mutex (svcArbitrateLock)";
    case HW_CONDVAR:
        return "condition variable (svcWaitProcessWideKeyAtomic)";
    case HW_ARBITER:
        return "address arbiter (svcWaitForAddress)";
    case HW_SLEEP:
        return "sleep (svcSleepThread)";
    default:
        return "";
    }
}

static const char *kobj_name(uint32_t t)
{
    static const char *const names[] = { "?", "thread", "process", "readable event", "writable event",
                                         "session", "shared memory", "transfer memory", "resource limit" };
    return t < sizeof names / sizeof names[0] ? names[t] : "?";
}

/** @brief The lifted function at guest address a: "module+0xoff name", or "-". */
static void fn_at(char *out, size_t n, uint64_t a)
{
    rl_fn_hit h;
    if (!rl_fn_find_guest(a, &h) || !h.fn) {
        snprintf(out, n, "-");
        return;
    }
    const char *nm = h.fn->guest_name ? h.fn->guest_name : h.fn->host_sym ? h.fn->host_sym : "?";
    snprintf(out, n, "%s+0x%llx %s", h.map->module ? h.map->module : "?",
             (unsigned long long)(h.fn->fn_addr - h.map->module_base), nm);
}

static void snap_end(void *user, const char *dir)
{
    (void)user;
    char path[1300];
    snprintf(path, sizeof path, "%s/threads_guest.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    const rl_gdb_cpu *cpu = rl_arm64_gdb();
    fprintf(f, "Guest threads. The pc is the thread's last saved pc: exact for a thread waiting in a system call,\n"
               "the last call's for one that was running (nothing was stopped).\n\n");
    hos_lock();
    uint32_t n = 0;
    for (hos_thread *t = hos.threads; t; t = t->next_all) {
        char fn[300] = "-";
        uint64_t pc = t->rt && t->rt->ctx && cpu->pc ? cpu->pc(t->rt->ctx) : 0;
        if (pc)
            fn_at(fn, sizeof fn, pc);
        const char *st = t->state == HT_CREATED ? "created" : t->state == HT_EXITED ? "exited" : "started";
        fprintf(f, "thread %llu: %s%s, core %d (ideal %d, affinity 0x%llx), priority %d (base %d)\n",
                (unsigned long long)t->id, st, t->attached ? ", host-attached" : "", t->cur_core, t->core,
                (unsigned long long)t->affinity, t->prio, t->base_prio);
        fprintf(f, "    %s; pc 0x%llx in %s\n", t->cur_core >= 0 ? "holds a core, running lifted code" :
                t->wait_kind ? "waiting" : t->runnable ? "runnable, waiting for a core" : "not runnable",
                (unsigned long long)pc, fn);
        fprintf(f, "    entry 0x%llx, stack top 0x%llx\n", (unsigned long long)t->entry, (unsigned long long)t->stack_top);
        if (t->wait_kind) {
            fprintf(f, "    waits on: %s", wait_name(t->wait_kind));
            if (t->wait_kind == HW_MUTEX || t->wait_kind == HW_ARBITER || t->wait_kind == HW_CONDVAR)
                fprintf(f, ", address 0x%llx", (unsigned long long)t->mutex_addr);
            if (t->wait_kind == HW_CONDVAR)
                fprintf(f, ", key 0x%llx", (unsigned long long)t->cv_key);
            if (t->wait_kind == HW_MUTEX && t->mutex_owner)
                fprintf(f, ", held by thread %llu", (unsigned long long)t->mutex_owner->id);
            fputc('\n', f);
            for (int i = 0; t->wait_kind == HW_SYNC && i < t->wait_count && i < HOS_WAIT_MAX; i++)
                fprintf(f, "      object %d: %s%s\n", i, t->wait_objs[i] ? kobj_name(t->wait_objs[i]->type) : "?",
                        t->wait_objs[i] ? "" : " (gone)");
            if (t->deadline)
                fprintf(f, "      times out at guest clock %llu ns\n", (unsigned long long)t->deadline);
        }
        n++;
    }
    hos_unlock();
    fprintf(f, "\n%u threads\n", n);
    fclose(f);
    rl_snapshot_file("threads_guest.txt", "Every guest thread: state, core, priority, the lifted function at its saved pc and what it waits on (svc, address, event), as the GDB server's info threads gives, taken without stopping the guest.");
}

void hos_snapshot_register(void)
{
    rl_snapshot_provider p = { .name = "switch", .end = snap_end };
    rl_snapshot_register(&p);
}

#else

void hos_snapshot_register(void) {}

#endif
