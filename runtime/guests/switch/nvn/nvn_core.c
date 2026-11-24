/**
 * @file nvn_core.c
 * @brief The NVN layer's loader hook, proc slots, nvnBootstrapLoader and
 * nvnDeviceGetProcAddress. The hook serves Switch and Switch 2 programs
 * alike: it answers nvnBootstrapLoader only, so a program that draws through
 * Vulkan never reaches it.
 *
 * Slot i of the thunk page (4 bytes each) is the guest address of one proc
 * name. The implemented procs take the first slots in name order; a name the
 * layer does not implement takes the next free slot the first time the game
 * asks for it, and that slot's function stops the game with the name.
 */
#include "nvn_priv.h"

#include <pthread.h>
#include <stdlib.h>

/**
 * @brief Command buffer procs relift has no function for are recorded under
 * their slot's op (NVN_CMD_COUNT + slot); running such a command reports it.
 */
#define RECORDED_PREFIX "nvnCommandBuffer"

typedef struct nvn_impl {
    const char *name;
    rl_fn fn;
    nvn_stats_kind kind;
} nvn_impl;

#define NVN_ENTRY(name) { #name, nvnh_##name, NVN_STATS_IMPL },
#define NVN_ENTRY_RECORDED(op, name) NVN_ENTRY(name)
#define NVN_ENTRY_ZERO(name) { #name, nvn_zero, NVN_STATS_ZERO },
static const nvn_impl impls[] = { NVN_PROCS(NVN_ENTRY) NVN_RECORDED(NVN_ENTRY_RECORDED) NVN_ZERO_PROCS(NVN_ENTRY_ZERO) };
#undef NVN_ENTRY_ZERO
#undef NVN_ENTRY_RECORDED
#undef NVN_ENTRY
#define N_IMPLS (sizeof impls / sizeof impls[0])

static struct {
    bool up;
    uint64_t base;
    uint64_t driver_loader;
    /** @brief The SDK's NvRmGpu entry points (0 when the program has none) and the device the layer opened. */
    uint64_t gpu_lib_open, gpu_device_open, gpu_choose_kind, gpu_get_info, gpu_lib, gpu_device;
    uint64_t driver_addrs[NVN_DRIVER_PROC_COUNT];
    pthread_mutex_t lock;
    uint32_t used;
    const nvn_impl *sorted[N_IMPLS];
    char *names[RL_NVN_SLOTS];
    bool recorded[RL_NVN_SLOTS];
    rl_fn_entry entries[RL_NVN_SLOTS];
    rl_unit unit;
} nvn = { .lock = PTHREAD_MUTEX_INITIALIZER };

static uint64_t miss(rl_ctx *c, uint8_t *mem, uint32_t slot)
{
    if (slot < RL_NVN_SLOTS && nvn.recorded[slot])
        return nvn_cmd_record(c, mem, NVN_CMD_COUNT + slot);
    const char *name = slot < RL_NVN_SLOTS && nvn.names[slot] ? nvn.names[slot] : "(unnamed)";
    const char *impure = nvn_driver_index(name) >= 0 ? nvn_impure(name) : NULL;
    if (impure)
        return (uint64_t)rl_guest_fatal(c, "nvn",
                                        "%s is not implemented, and the %s driver's own code for it is not pure: it "
                                        "%s (called from 0x%llx)",
                                        name, nvn_version_name(), impure, (unsigned long long)c->x[30]);
    return (uint64_t)rl_guest_fatal(c, "nvn", "%s is not implemented for %s (called from 0x%llx)", name,
                                    nvn_version_name(), (unsigned long long)c->x[30]);
}

/*
 * One function per slot: miss_<g><a><b><c><d> is slot (g - 1) * 256 + the
 * base-4 number abcd, so the list below names them in slot order.
 */
#define MISS_SLOT(t)                                                                                                   \
    (((t) / 10000 - 1) * 256 + ((t) / 1000 % 10) * 64 + ((t) / 100 % 10) * 16 + ((t) / 10 % 10) * 4 + (t) % 10)
#define EACH4(M, i) M(i##0) M(i##1) M(i##2) M(i##3)
#define EACH16(M, i) EACH4(M, i##0) EACH4(M, i##1) EACH4(M, i##2) EACH4(M, i##3)
#define EACH64(M, i) EACH16(M, i##0) EACH16(M, i##1) EACH16(M, i##2) EACH16(M, i##3)
#define EACH256(M, i) EACH64(M, i##0) EACH64(M, i##1) EACH64(M, i##2) EACH64(M, i##3)
#define EACH_SLOT(M) EACH256(M, 1) EACH256(M, 2) EACH256(M, 3) EACH256(M, 4)

#define MISS1(i)                                                                                                       \
    static uint64_t miss_##i(rl_ctx *c, uint8_t *mem) { return miss(c, mem, MISS_SLOT(i)); }
EACH_SLOT(MISS1)

/** @brief Each slot's own function, which the counting functions of RELIFT_NVN_STATS call after counting. */
static rl_fn slot_fns[RL_NVN_SLOTS];

#define COUNT1(i)                                                                                                      \
    static uint64_t count_##i(rl_ctx *c, uint8_t *mem)                                                                 \
    {                                                                                                                  \
        nvn_stats_call(MISS_SLOT(i));                                                                                  \
        return slot_fns[MISS_SLOT(i)](c, mem);                                                                         \
    }
EACH_SLOT(COUNT1)

#define MISS_REF(i) miss_##i,
#define COUNT_REF(i) count_##i,
static rl_fn const misses[RL_NVN_SLOTS] = { EACH_SLOT(MISS_REF) };
static rl_fn const counts[RL_NVN_SLOTS] = { EACH_SLOT(COUNT_REF) };

static int by_name(const void *a, const void *b)
{
    return strcmp((*(const nvn_impl *const *)a)->name, (*(const nvn_impl *const *)b)->name);
}

static int find_impl(const char *name)
{
    uint32_t lo = 0, hi = N_IMPLS;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(nvn.sorted[mid]->name, name);
        if (!c)
            return (int)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

/** @brief The slot of name, taking a free one for a name relift does not implement; 0 when none is left. */
static uint64_t slot_of(const char *name)
{
    int k = find_impl(name);
    if (k >= 0)
        return nvn.base + 4u * (uint64_t)k;
    uint64_t addr = 0;
    pthread_mutex_lock(&nvn.lock);
    for (uint32_t i = N_IMPLS; i < nvn.used; i++)
        if (!strcmp(nvn.names[i], name)) {
            addr = nvn.base + 4u * (uint64_t)i;
            break;
        }
    if (!addr && nvn.used < RL_NVN_SLOTS) {
        char *copy = strdup(name);
        if (copy) {
            nvn.names[nvn.used] = copy;
            nvn.recorded[nvn.used] = !strncmp(name, RECORDED_PREFIX, sizeof RECORDED_PREFIX - 1);
            addr = nvn.base + 4u * (uint64_t)nvn.used++;
        }
    }
    pthread_mutex_unlock(&nvn.lock);
    if (!addr)
        rl_log(RL_LOG_ERROR, "nvn", "no slot left for %s", name);
    return addr;
}

uint64_t rl_nvn_proc(const char *name)
{
    if (!nvn.up || !name)
        return 0;
    int k = find_impl(name);
    if (k >= 0)
        nvn_stats_lookup(nvn.sorted[k]->name, nvn.sorted[k]->kind, (uint32_t)k);
    return k >= 0 ? nvn.base + 4u * (uint64_t)k : 0;
}

int nvn_guest_call(rl_ctx *c, uint8_t *mem, uint64_t fn, const uint64_t *args, unsigned n, uint64_t *ret)
{
    uint64_t lr = c->x[30], pc = c->pc;
    for (unsigned i = 0; i < n && i < 8; i++)
        c->x[i] = args[i];
    c->x[30] = nvn.base;
    int rc = rl_call_guest(c, mem, fn);
    if (rc != RL_OK)
        return rc;
    c->x[30] = lr;
    c->pc = pc;
    if (ret)
        *ret = c->x[0];
    return RL_OK;
}

/** @brief The driver's own function for proc k of nvn_driver.c (0 when it has none), asking its loader once. */
static int driver_addr(rl_ctx *c, uint8_t *mem, int k, const char *name, uint64_t *addr)
{
    uint64_t a = __atomic_load_n(&nvn.driver_addrs[k], __ATOMIC_ACQUIRE);
    if (a || !nvn.driver_loader) {
        *addr = a;
        return RL_OK;
    }
    size_t len = strlen(name) + 1;
    uint64_t sp = c->sp;
    c->sp = (sp - len) & ~15ull;
    memcpy(mem + c->sp, name, len);
    uint64_t arg = c->sp;
    int rc = nvn_guest_call(c, mem, nvn.driver_loader, &arg, 1, &a);
    c->sp = sp;
    if (rc != RL_OK)
        return rc;
    __atomic_store_n(&nvn.driver_addrs[k], a, __ATOMIC_RELEASE);
    *addr = a;
    return RL_OK;
}

int nvn_driver_lookup(rl_ctx *c, uint8_t *mem, const char *name, uint64_t *addr)
{
    *addr = 0;
    if (!nvn.driver_loader)
        return RL_OK;
    size_t len = strlen(name) + 1;
    uint64_t sp = c->sp;
    c->sp = (sp - len) & ~15ull;
    memcpy(mem + c->sp, name, len);
    uint64_t arg = c->sp;
    int rc = nvn_guest_call(c, mem, nvn.driver_loader, &arg, 1, addr);
    c->sp = sp;
    return rc;
}

/** @brief The guest address the game gets for name: the driver's own function for a kept proc, else a slot. */
static int proc_addr(rl_ctx *c, uint8_t *mem, const char *name, uint64_t *addr)
{
    *addr = 0;
    if (!name)
        return RL_OK;
    int k = nvn_driver_index(name);
    if (k >= 0 && !nvn_impure(name) && !(nvn_generation() == 1 && nvn_driver_reads_texture(name))) {
        int rc = driver_addr(c, mem, k, name, addr);
        if (rc == RL_OK && *addr)
            nvn_stats_lookup(name, NVN_STATS_DRIVER, UINT32_MAX);
        if (rc != RL_OK || *addr)
            return rc;
    }
    *addr = slot_of(name);
    if (*addr && nvn_stats_on()) {
        uint32_t slot = (uint32_t)((*addr - nvn.base) / 4u);
        nvn_stats_kind kind = slot < N_IMPLS ? nvn.sorted[slot]->kind
                              : nvn.recorded[slot] ? NVN_STATS_RECORDED
                                                   : NVN_STATS_MISSING;
        nvn_stats_lookup(slot < N_IMPLS ? nvn.sorted[slot]->name : nvn.names[slot], kind, slot);
    }
    return RL_OK;
}

/**
 * @brief What nvn_driver_index and nvn_impure answer for one name, kept by the name's address: callers pass string
 * literals, so a replayed getter finds its proc without string compares. Entries are made once and never change.
 */
typedef struct proc_memo {
    const char *name;
    const void *driver;
    int k;
    const char *impure;
} proc_memo;

/** @brief Open-addressed table of proc_memo, filled by compare-and-swap; a full table leaves names unremembered. */
static proc_memo *proc_memos[512];

static void proc_lookup(const char *name, int *k, const char **impure)
{
    const void *drv = nvn_driver_record();
    uint32_t h = (uint32_t)(((uintptr_t)name >> 3) * 0x9E3779B1u) % 512u;
    for (uint32_t i = 0; i < 16; i++) {
        proc_memo **slot = &proc_memos[(h + i) % 512u];
        proc_memo *m = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (m && m->name == name && m->driver == drv) {
            *k = m->k;
            *impure = m->impure;
            return;
        }
        if (m)
            continue;
        *k = nvn_driver_index(name);
        *impure = nvn_impure(name);
        proc_memo *fresh = malloc(sizeof *fresh);
        if (!fresh)
            return;
        *fresh = (proc_memo){ name, drv, *k, *impure };
        proc_memo *none = NULL;
        if (!__atomic_compare_exchange_n(slot, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            free(fresh);
        return;
    }
    *k = nvn_driver_index(name);
    *impure = nvn_impure(name);
}

int nvn_driver_call(rl_ctx *c, uint8_t *mem, const char *name, const uint64_t *args, unsigned n, uint64_t *ret)
{
    int k;
    const char *impure;
    proc_lookup(name, &k, &impure);
    if (impure)
        return rl_guest_fatal(c, "nvn", "the %s driver's own %s is not pure: it %s", nvn_version_name(), name, impure);
    uint64_t a = 0;
    int rc = k >= 0 ? driver_addr(c, mem, k, name, &a) : RL_OK;
    if (rc != RL_OK)
        return rc;
    if (!a)
        return rl_guest_fatal(c, "nvn", "the driver has no %s", name);
    return nvn_guest_call(c, mem, a, args, n, ret);
}

int nvn_gpu_open(rl_ctx *c, uint8_t *mem)
{
    pthread_mutex_lock(&nvn.lock);
    bool done = nvn.gpu_device != 0;
    pthread_mutex_unlock(&nvn.lock);
    if (done)
        return RL_OK;
    if (!nvn.gpu_lib_open || !nvn.gpu_device_open)
        return rl_guest_fatal(c, "nvn", "the program has no NvRmGpuLibOpen or NvRmGpuDeviceOpen");
    uint64_t zero = 0, lib = 0;
    int rc = nvn_guest_call(c, mem, nvn.gpu_lib_open, &zero, 1, &lib);
    if (rc != RL_OK)
        return rc;
    if (!lib)
        return rl_guest_fatal(c, "nvn", "NvRmGpuLibOpen failed");
    uint64_t sp = c->sp;
    c->sp = (sp - 32) & ~15ull;
    uint64_t attr = c->sp, out = c->sp + 16;
    memset(mem + c->sp, 0, 32);
    uint64_t args[4] = { lib, 0xffffffffull, attr, out }, err = 0;
    rc = nvn_guest_call(c, mem, nvn.gpu_device_open, args, 4, &err);
    uint64_t dev = rl_ld64(mem, out);
    c->sp = sp;
    if (rc != RL_OK)
        return rc;
    if ((uint32_t)err || !dev)
        return rl_guest_fatal(c, "nvn", "NvRmGpuDeviceOpen failed: NvError 0x%x", (unsigned)err);
    pthread_mutex_lock(&nvn.lock);
    nvn.gpu_lib = lib;
    nvn.gpu_device = dev;
    pthread_mutex_unlock(&nvn.lock);
    rl_log(RL_LOG_INFO, "nvn", "NvRmGpuDevice 0x%llx open (library 0x%llx)", (unsigned long long)dev,
           (unsigned long long)lib);
    return RL_OK;
}

int nvn_gpu_info_u32(rl_ctx *c, uint8_t *mem, int64_t off, uint32_t *out)
{
    int rc = nvn_gpu_open(c, mem);
    if (rc != RL_OK)
        return rc;
    if (!nvn.gpu_get_info)
        return rl_guest_fatal(c, "nvn", "the program has no NvRmGpuDeviceGetInfo");
    uint64_t dev = nvn_gpu_device(), info = 0;
    rc = nvn_guest_call(c, mem, nvn.gpu_get_info, &dev, 1, &info);
    if (rc != RL_OK)
        return rc;
    if (!info)
        return rl_guest_fatal(c, "nvn", "NvRmGpuDeviceGetInfo returned no information");
    *out = rl_ld32(mem, info + (uint64_t)off);
    return RL_OK;
}

uint64_t nvn_gpu_device(void)
{
    pthread_mutex_lock(&nvn.lock);
    uint64_t d = nvn.gpu_device;
    pthread_mutex_unlock(&nvn.lock);
    return d;
}

/** @brief nvnBootstrapLoader(const char *name): the address of any proc, as nvnDeviceGetProcAddress gives it. */
uint64_t nvnh_nvnBootstrapLoader(rl_ctx *c, uint8_t *mem)
{
    const char *name = nvn_str(mem, c->x[0], NVN_NAME_MAX);
    uint64_t a;
    int rc = proc_addr(c, mem, name, &a);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = a;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "nvnBootstrapLoader(%s) = 0x%llx", name ? name : "(null)", (unsigned long long)c->x[0]);
    return nvn_ret(c);
}

/** @brief nvnDeviceGetProcAddress(const NVNdevice *device, const char *name): the same for every device. */
uint64_t nvnh_nvnDeviceGetProcAddress(rl_ctx *c, uint8_t *mem)
{
    uint64_t a;
    int rc = proc_addr(c, mem, nvn_str(mem, c->x[1], NVN_NAME_MAX), &a);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = a;
    return nvn_ret(c);
}

static int hook_start(void *user, uint64_t *code_free)
{
    (void)user;
    uint64_t bytes = (4u * RL_NVN_SLOTS + HOS_PAGE - 1) & ~(HOS_PAGE - 1);
    if (rl_hos_reserve_at(*code_free, bytes) != KR_SUCCESS) {
        rl_log(RL_LOG_ERROR, "nvn", "no guest address space for the NVN layer");
        return -1;
    }
    nvn.base = *code_free;
    *code_free += bytes;
    for (uint32_t i = 0; i < N_IMPLS; i++)
        nvn.sorted[i] = &impls[i];
    qsort(nvn.sorted, N_IMPLS, sizeof nvn.sorted[0], by_name);
    bool count = nvn_stats_on();
    for (uint32_t i = 0; i < RL_NVN_SLOTS; i++) {
        slot_fns[i] = i < N_IMPLS ? nvn.sorted[i]->fn : misses[i];
        nvn.entries[i] = (rl_fn_entry){ nvn.base + 4u * (uint64_t)i, 4, RL_FN_OVERRIDE, count ? counts[i] : slot_fns[i] };
    }
    nvn.used = N_IMPLS;
    nvn.unit = (rl_unit){
        .abi_version = RL_ABI_VERSION,
        .ctx_size = sizeof(rl_ctx),
        .fn_count = RL_NVN_SLOTS,
        .fns = nvn.entries,
        .name = "nvn",
    };
    const rl_unit *u = &nvn.unit;
    if (rl_units_register(&u, 1) != 0)
        return -1;
    nvn.up = true;
    rl_log(RL_LOG_INFO, "nvn", "NVN layer up for %s: %u procs implemented, thunks at 0x%llx", nvn_version_name(),
           (unsigned)N_IMPLS, (unsigned long long)nvn.base);
    return 0;
}

static uint64_t hook_import(void *user, const char *symbol)
{
    (void)user;
    return rl_nvn_proc(symbol);
}

static rl_fn hook_replace(void *user, const char *symbol)
{
    (void)user;
    if (!nvn.up)
        return NULL;
    if (!strcmp(symbol, "nvnBootstrapLoader"))
        return nvnh_nvnBootstrapLoader;
    if (!strcmp(symbol, "NvRmGpuMappingModify"))
        return nvnh_NvRmGpuMappingModify;
    return !strcmp(symbol, "NvRmMemHandleAllocNativeImage") ? nvnh_NvRmMemHandleAllocNativeImage : NULL;
}

static void hook_defined(void *user, const char *symbol, uint64_t addr)
{
    (void)user;
    if (!strcmp(symbol, "nvnBootstrapLoaderInternal"))
        nvn.driver_loader = addr;
    else if (!strcmp(symbol, "NvRmGpuLibOpen"))
        nvn.gpu_lib_open = addr;
    else if (!strcmp(symbol, "NvRmGpuDeviceOpen"))
        nvn.gpu_device_open = addr;
    else if (!strcmp(symbol, "NvRmGpuDeviceChooseKind"))
        nvn.gpu_choose_kind = addr;
    else if (!strcmp(symbol, "NvRmGpuDeviceGetInfo"))
        nvn.gpu_get_info = addr;
}

static int hook_wants(void *user, const char *symbol)
{
    (void)user;
    return !strcmp(symbol, "nvnBootstrapLoader");
}

static const rl_switch_hook g_hook = {
    .name = "nvn",
    .start = hook_start,
    .import = hook_import,
    .replace = hook_replace,
    .defined = hook_defined,
    .wants = hook_wants,
    .consoles = 1u << RL_CONSOLE_SWITCH | 1u << RL_CONSOLE_SWITCH2,
};

const rl_switch_hook *rl_nvn_switch_hook(void) { return &g_hook; }

__attribute__((constructor)) static void register_hook(void) { (void)rl_switch_register_auto_hook(&g_hook); }

const char *nvn_slot_name(uint32_t slot)
{
    if (slot >= RL_NVN_SLOTS)
        return NULL;
    pthread_mutex_lock(&nvn.lock);
    const char *n = slot < nvn.used ? nvn.names[slot] : NULL;
    pthread_mutex_unlock(&nvn.lock);
    return n;
}
