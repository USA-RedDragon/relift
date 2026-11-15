/**
 * @file vk_core.c
 * @brief The forwarding layer's plumbing: the per-thread arena, the handle
 * table, host function loading, the thunk unit, vkGetInstanceProcAddr and
 * the hand-written instance and device commands.
 */
#include "vkf.h"
#include "hostlib.h"

#include <stdlib.h>
#include <string.h>

vkf_state vkf = { .lock = PTHREAD_MUTEX_INITIALIZER };

static rl_fn_entry g_entries[VKG_COUNT];
static rl_unit g_unit;
static bool g_ready;


typedef struct chunk {
    struct chunk *next;
    size_t size;
    size_t used;
    _Alignas(16) uint8_t data[];
} chunk;

typedef struct arena {
    chunk *first;
    chunk *cur;
} arena;

static _Thread_local arena t_arena;
static _Thread_local vkf_call *t_call;

static chunk *chunk_new(size_t n)
{
    size_t size = n > 65536 ? n : 65536;
    chunk *c = malloc(sizeof *c + size);
    if (!c)
        abort();
    c->next = NULL;
    c->size = size;
    c->used = 0;
    return c;
}

void *vkf_alloc(vkf_call *k, size_t n)
{
    (void)k;
    arena *a = &t_arena;
    n = (n + 15) & ~(size_t)15;
    if (!a->cur) {
        a->first = a->cur = chunk_new(n);
    }
    while (a->cur->used + n > a->cur->size) {
        if (!a->cur->next || a->cur->next->size < n) {
            chunk *c = chunk_new(n);
            c->next = a->cur->next;
            a->cur->next = c;
        }
        a->cur = a->cur->next;
        a->cur->used = 0;
    }
    void *p = a->cur->data + a->cur->used;
    a->cur->used += n;
    return p;
}

void vkf_begin(vkf_call *k, rl_ctx *c, uint8_t *mem)
{
    k->c = c;
    k->mem = mem;
    k->fns = &vkf.global;
    k->obj = NULL;
    k->rc = RL_OK;
    k->mark.chunk = t_arena.cur;
    k->mark.used = t_arena.cur ? t_arena.cur->used : 0;
    k->outer = t_call;
    k->left = false;
    t_call = k;
}

int vkf_end(vkf_call *k)
{
    if (k->mark.chunk) {
        t_arena.cur = k->mark.chunk;
        t_arena.cur->used = k->mark.used;
    } else if (t_arena.first) {
        t_arena.cur = t_arena.first;
        t_arena.cur->used = 0;
    }
    t_call = k->outer;
    return k->rc;
}

vkf_call *vkf_current(void) { return t_call; }

void vkf_global(vkf_call *k) { k->fns = &vkf.global; }


struct vkf_map {
    uint64_t *keys;
    void **vals;
    uint32_t cap;
    uint32_t count;
};

vkf_map *vkf_map_new(void)
{
    vkf_map *m = calloc(1, sizeof *m);
    if (!m)
        abort();
    return m;
}

void vkf_map_free(vkf_map *m)
{
    if (!m)
        return;
    free(m->keys);
    free(m->vals);
    free(m);
}

static uint32_t slot_of(const vkf_map *m, uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 32) & (m->cap - 1);
    while (m->keys[i] && m->keys[i] != key)
        i = (i + 1) & (m->cap - 1);
    return i;
}

void *vkf_map_get(const vkf_map *m, uint64_t key)
{
    if (!m || !m->cap || !key)
        return NULL;
    uint32_t i = slot_of(m, key);
    return m->keys[i] ? m->vals[i] : NULL;
}

static void map_grow(vkf_map *m)
{
    vkf_map old = *m;
    m->cap = old.cap ? old.cap * 2 : 64;
    m->keys = calloc(m->cap, sizeof *m->keys);
    m->vals = calloc(m->cap, sizeof *m->vals);
    if (!m->keys || !m->vals)
        abort();
    m->count = 0;
    for (uint32_t i = 0; i < old.cap; i++)
        if (old.keys[i]) {
            uint32_t j = slot_of(m, old.keys[i]);
            m->keys[j] = old.keys[i];
            m->vals[j] = old.vals[i];
            m->count++;
        }
    free(old.keys);
    free(old.vals);
}

void vkf_map_put(vkf_map *m, uint64_t key, void *value)
{
    if (!key)
        return;
    if (!value) {
        if (!m->cap)
            return;
        uint32_t i = slot_of(m, key);
        if (!m->keys[i])
            return;
        m->keys[i] = 0;
        m->vals[i] = NULL;
        m->count--;
        for (uint32_t j = (i + 1) & (m->cap - 1); m->keys[j]; j = (j + 1) & (m->cap - 1)) {
            uint64_t k = m->keys[j];
            void *v = m->vals[j];
            m->keys[j] = 0;
            m->vals[j] = NULL;
            m->count--;
            uint32_t n = slot_of(m, k);
            m->keys[n] = k;
            m->vals[n] = v;
            m->count++;
        }
        return;
    }
    if ((m->count + 1) * 2 > m->cap)
        map_grow(m);
    uint32_t i = slot_of(m, key);
    if (!m->keys[i])
        m->count++;
    m->keys[i] = key;
    m->vals[i] = value;
}

void vkf_map_each(const vkf_map *m, void (*fn)(uint64_t key, void *value, void *user), void *user)
{
    for (uint32_t i = 0; m && i < m->cap; i++)
        if (m->keys[i])
            fn(m->keys[i], m->vals[i], user);
}


#define CHUNK_SLOTS 1024u
#define MAX_CHUNKS 1024u

static vkf_obj **g_slots[MAX_CHUNKS];
static uint32_t g_next_slot;
static uint32_t g_live;
static uint32_t *g_free;
static uint32_t g_free_n, g_free_cap;
static vkf_map *g_by_host[VKG_KIND_COUNT];

vkf_obj *vkf_obj_of(uint64_t g, uint32_t kind)
{
    if ((g & 0xFF00000000000000ull) != 0x5600000000000000ull || (g & 15) != kind)
        return NULL;
    uint64_t slot = (g & 0x00FFFFFFFFFFFFFFull) >> 4;
    if (slot >= (uint64_t)CHUNK_SLOTS * MAX_CHUNKS)
        return NULL;
    vkf_obj **c = __atomic_load_n(&g_slots[slot / CHUNK_SLOTS], __ATOMIC_ACQUIRE);
    if (!c)
        return NULL;
    vkf_obj *o = __atomic_load_n(&c[slot % CHUNK_SLOTS], __ATOMIC_ACQUIRE);
    return o && o->kind == kind ? o : NULL;
}

vkf_obj *vkf_obj_add(uint32_t kind, void *host, vkf_obj *parent, vkf_fns *fns)
{
    vkf_obj *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->kind = kind;
    o->host = host;
    o->parent = parent;
    o->fns = fns;
    if (parent) {
        o->inst = parent->inst;
        o->dev = parent->dev;
    }
    pthread_mutex_lock(&vkf.lock);
    uint32_t slot;
    if (g_free_n)
        slot = g_free[--g_free_n];
    else
        slot = g_next_slot++;
    if (slot >= CHUNK_SLOTS * MAX_CHUNKS) {
        pthread_mutex_unlock(&vkf.lock);
        free(o);
        return NULL;
    }
    if (!g_slots[slot / CHUNK_SLOTS]) {
        vkf_obj **c = calloc(CHUNK_SLOTS, sizeof *c);
        if (!c)
            abort();
        __atomic_store_n(&g_slots[slot / CHUNK_SLOTS], c, __ATOMIC_RELEASE);
    }
    o->slot = slot;
    __atomic_store_n(&g_slots[slot / CHUNK_SLOTS][slot % CHUNK_SLOTS], o, __ATOMIC_RELEASE);
    if (!g_by_host[kind])
        g_by_host[kind] = vkf_map_new();
    vkf_map_put(g_by_host[kind], (uint64_t)(uintptr_t)host, o);
    g_live++;
    pthread_mutex_unlock(&vkf.lock);
    return o;
}

static void remove_locked(vkf_obj *o)
{
    __atomic_store_n(&g_slots[o->slot / CHUNK_SLOTS][o->slot % CHUNK_SLOTS], NULL, __ATOMIC_RELEASE);
    if (g_by_host[o->kind] && vkf_map_get(g_by_host[o->kind], (uint64_t)(uintptr_t)o->host) == o)
        vkf_map_put(g_by_host[o->kind], (uint64_t)(uintptr_t)o->host, NULL);
    if (g_free_n == g_free_cap) {
        g_free_cap = g_free_cap ? g_free_cap * 2 : 256;
        g_free = realloc(g_free, g_free_cap * sizeof *g_free);
        if (!g_free)
            abort();
    }
    g_free[g_free_n++] = o->slot;
    g_live--;
    free(o);
}

void vkf_obj_remove(vkf_obj *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&vkf.lock);
    remove_locked(o);
    pthread_mutex_unlock(&vkf.lock);
}

static bool reaches(const vkf_obj *o, const vkf_obj *owner)
{
    for (const vkf_obj *p = o->parent; p; p = p->parent)
        if (p == owner)
            return true;
    return false;
}

void vkf_obj_remove_children(vkf_obj *owner)
{
    pthread_mutex_lock(&vkf.lock);
    for (int pass = 0; pass < 4; pass++)
        for (uint32_t s = 0; s < g_next_slot; s++) {
            vkf_obj *o = g_slots[s / CHUNK_SLOTS] ? g_slots[s / CHUNK_SLOTS][s % CHUNK_SLOTS] : NULL;
            if (o && reaches(o, owner)) {
                bool leaf = true;
                for (uint32_t t = 0; t < g_next_slot && leaf; t++) {
                    vkf_obj *q = g_slots[t / CHUNK_SLOTS] ? g_slots[t / CHUNK_SLOTS][t % CHUNK_SLOTS] : NULL;
                    if (q && q->parent == o)
                        leaf = false;
                }
                if (leaf)
                    remove_locked(o);
            }
        }
    pthread_mutex_unlock(&vkf.lock);
}

vkf_obj *vkf_obj_find_host(uint32_t kind, const void *host)
{
    pthread_mutex_lock(&vkf.lock);
    vkf_obj *o = g_by_host[kind] ? vkf_map_get(g_by_host[kind], (uint64_t)(uintptr_t)host) : NULL;
    pthread_mutex_unlock(&vkf.lock);
    return o;
}

uint32_t rl_vk_live_handles(void) { return g_live; }

void *vkf_first(vkf_call *k, uint32_t kind, uint64_t g)
{
    vkf_obj *o = vkf_obj_of(g, kind);
    if (!o)
        return NULL;
    k->obj = o;
    k->fns = o->fns;
    return o->host;
}

void *vkf_unwrap(vkf_call *k, uint32_t kind, uint64_t g)
{
    (void)k;
    vkf_obj *o = vkf_obj_of(g, kind);
    if (!o && g && rl_once(g ^ 0x766b756e77726170ull))
        rl_log(RL_LOG_WARN, "vk", "unknown guest handle 0x%llx (kind %u)", (unsigned long long)g, kind);
    return o ? o->host : NULL;
}

void **vkf_unwrap_n(vkf_call *k, uint32_t kind, uint64_t g, uint64_t n)
{
    if (!g)
        return NULL;
    void **h = vkf_alloc(k, (n ? n : 1) * sizeof *h);
    for (uint64_t i = 0; i < n; i++)
        h[i] = vkf_unwrap(k, kind, vkf_g64(k, g + 8 * i));
    return h;
}

uint64_t vkf_wrap(vkf_call *k, uint32_t kind, void *host)
{
    if (!host)
        return 0;
    vkf_obj *o = vkf_obj_find_host(kind, host);
    if (o)
        return vkf_id(o);
    vkf_obj *parent = k->obj;
    if (kind == VKG_KIND_VkPhysicalDevice)
        while (parent && parent->kind != VKG_KIND_VkInstance)
            parent = parent->parent;
    else if (kind == VKG_KIND_VkQueue || kind == VKG_KIND_VkCommandBuffer)
        while (parent && parent->kind != VKG_KIND_VkDevice)
            parent = parent->parent;
    if (!parent) {
        rl_log(RL_LOG_ERROR, "vk", "no owner for a new handle of kind %u", kind);
        return 0;
    }
    o = vkf_obj_add(kind, host, parent, parent->fns);
    return vkf_id(o);
}

const char *const *vkf_strv(vkf_call *k, uint64_t g, uint64_t n)
{
    if (!g)
        return NULL;
    const char **h = vkf_alloc(k, (n ? n : 1) * sizeof *h);
    for (uint64_t i = 0; i < n; i++)
        h[i] = vkf_ptr(k, vkf_g64(k, g + 8 * i));
    return h;
}

void vkf_unknown_stype(vkf_call *k, uint32_t stype)
{
    (void)k;
    rl_report_unknown(0x766b0000000000ull | stype, "vk", "pNext struct with sType %u is not forwarded; dropped from the chain", stype);
}

int vkf_bad_handle(vkf_call *k, int cmd, uint64_t g)
{
    rl_log(RL_LOG_ERROR, "vk", "%s: 0x%llx is not a handle the guest got from the forwarding layer", vkg_cmds[cmd].name,
           (unsigned long long)g);
    k->c->x[0] = (uint32_t)VK_ERROR_UNKNOWN;
    return vkf_end(k);
}

int vkf_no_fn(vkf_call *k, int cmd)
{
    if (rl_once(0x766b6e6f666e0000ull | (uint64_t)cmd))
        rl_log(RL_LOG_ERROR, "vk", "%s: the host has no such command", vkg_cmds[cmd].name);
    k->c->x[0] = (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
    return vkf_end(k);
}


typedef void (*vkf_void_fn)(void);

static vkf_void_fn load_one(void *(*get)(void *, const char *), void *h, int i)
{
    void *p = get(h, vkg_cmds[i].name);
    for (const char *const *a = vkg_cmds[i].aliases; !p && a && *a; a++)
        p = get(h, *a);
    vkf_void_fn f;
    memcpy(&f, &p, sizeof f);
    return f;
}

void vkf_load_instance(vkf_fns *fns, void *host_instance)
{
    for (int i = 0; i < VKG_COUNT; i++)
        fns->f[i] = vkg_cmds[i].level == 0 ? vkf.global.f[i] : load_one(vkf.gipa, host_instance, i);
}

void vkf_load_device(vkf_fns *fns, const vkf_fns *inst, void *host_device)
{
    void *(*gdpa)(void *, const char *);
    void (*raw)(void) = inst->f[VKG_vkGetDeviceProcAddr];
    memcpy(&gdpa, &raw, sizeof gdpa);
    for (int i = 0; i < VKG_COUNT; i++) {
        fns->f[i] = inst->f[i];
        if (vkg_cmds[i].level == 2 && gdpa) {
            vkf_void_fn f = load_one(gdpa, host_device, i);
            if (f)
                fns->f[i] = f;
        }
    }
}


static int map_mode_from_env(int fallback)
{
    const char *e = getenv(RL_VK_MAP_ENV);
    if (!e || !*e)
        return fallback;
    if (!strcmp(e, "placed"))
        return RL_VK_MAP_PLACED;
    if (!strcmp(e, "import"))
        return RL_VK_MAP_IMPORT;
    if (!strcmp(e, "shadow"))
        return RL_VK_MAP_SHADOW;
    if (!strcmp(e, "auto"))
        return RL_VK_MAP_AUTO;
    rl_log(RL_LOG_WARN, "vk", "%s=%s is not placed, import, shadow or auto; ignored", RL_VK_MAP_ENV, e);
    return fallback;
}

int rl_vk_init(const rl_vk_config *cfg)
{
    vkf.cfg = *cfg;
    void *gipa = cfg->gipa ? cfg->gipa : rl_hostlib_vulkan_gipa();
    if (!gipa)
        return -1;
    memcpy(&vkf.gipa, &gipa, sizeof gipa);
    vkf.forced_mode = map_mode_from_env(cfg->map_mode);
    for (int i = 0; i < VKG_COUNT; i++)
        vkf.global.f[i] = vkg_cmds[i].level == 0 ? load_one(vkf.gipa, NULL, i) : NULL;
    if (!vkf.global.f[VKG_vkCreateInstance]) {
        rl_log(RL_LOG_ERROR, "vk", "the host Vulkan loader has no vkCreateInstance");
        return -1;
    }
    uint32_t n = 0;
    for (int i = 0; i < VKG_COUNT; i++)
        if (vkg_thunks[i])
            g_entries[n++] = (rl_fn_entry){ cfg->thunk_base + 4u * (uint64_t)i, 4, RL_FN_OVERRIDE, vkg_thunks[i] };
    g_unit = (rl_unit){
        .abi_version = RL_ABI_VERSION,
        .ctx_size = sizeof(rl_ctx),
        .fn_count = n,
        .fns = g_entries,
        .name = "vulkan-forwarding",
    };
    g_ready = true;
    return 0;
}

void rl_vk_shutdown(void)
{
    pthread_mutex_lock(&vkf.lock);
    for (uint32_t s = 0; s < g_next_slot; s++) {
        vkf_obj *o = g_slots[s / CHUNK_SLOTS] ? g_slots[s / CHUNK_SLOTS][s % CHUNK_SLOTS] : NULL;
        if (o)
            remove_locked(o);
    }
    pthread_mutex_unlock(&vkf.lock);
    vkf_cb_release(0);
    g_ready = false;
}

const rl_unit *rl_vk_unit(void) { return g_ready ? &g_unit : NULL; }

uint64_t rl_vk_thunk_bytes(void) { return 4u * (uint64_t)VKG_COUNT; }

static int cmd_of(const char *name)
{
    uint32_t lo = 0, hi = vkg_name_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(vkg_names[mid].name, name);
        if (!c)
            return vkg_names[mid].cmd;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

rl_fn rl_vk_import(const char *symbol)
{
    int c = cmd_of(symbol);
    return c < 0 ? NULL : vkg_thunks[c];
}

uint64_t rl_vk_guest_addr(const char *name)
{
    int c = cmd_of(name);
    return c < 0 || !vkg_thunks[c] ? 0 : vkf.cfg.thunk_base + 4u * (uint64_t)c;
}

int rl_vk_device_map_mode(uint64_t guest_device)
{
    vkf_obj *o = vkf_obj_of(guest_device, VKG_KIND_VkDevice);
    return o && o->dev ? o->dev->map_mode : -1;
}


static bool emulated(int cmd) { return cmd == VKG_vkCreateViSurfaceNN; }

int vkh_vkGetInstanceProcAddr(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    uint64_t gi = c->x[0];
    const char *name = vkf_ptr(&k, c->x[1]);
    int cmd = name && !vkf_swap_unsupported(name) ? cmd_of(name) : -1;
    uint64_t addr = 0;
    if (cmd >= 0) {
        uint64_t a = vkf.cfg.thunk_base + 4u * (uint64_t)cmd;
        if (cmd == VKG_vkGetInstanceProcAddr)
            addr = a;
        else if (!gi)
            addr = vkg_cmds[cmd].level == 0 && vkf.global.f[cmd] ? a : 0;
        else {
            vkf_obj *o = vkf_obj_of(gi, VKG_KIND_VkInstance);
            if (o && vkg_cmds[cmd].level == 0)
                addr = vkf.global.f[cmd] ? a : 0;
            else if (o && emulated(cmd))
                addr = o->inst->vi_surface ? a : 0;
            else if (o)
                addr = o->fns->f[cmd] ? a : 0;
        }
    }
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "vk", "vkGetInstanceProcAddr(%s) = 0x%llx", name ? name : "(null)", (unsigned long long)addr);
    c->x[0] = addr;
    return vkf_end(&k);
}

int vkh_vkGetDeviceProcAddr(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    vkf_obj *o = vkf_obj_of(c->x[0], VKG_KIND_VkDevice);
    const char *name = vkf_ptr(&k, c->x[1]);
    int cmd = name && !vkf_swap_unsupported(name) ? cmd_of(name) : -1;
    uint64_t addr = 0;
    if (o && cmd >= 0 && (vkg_cmds[cmd].level == 2 || cmd == VKG_vkGetDeviceProcAddr) && o->fns->f[cmd])
        addr = vkf.cfg.thunk_base + 4u * (uint64_t)cmd;
    c->x[0] = addr;
    return vkf_end(&k);
}


static bool in_list(const char *const *list, const char *name)
{
    for (; list && *list; list++)
        if (!strcmp(*list, name))
            return true;
    return false;
}

static bool host_has(const VkExtensionProperties *p, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name))
            return true;
    return false;
}

#define PORTABILITY_ENUMERATION "VK_KHR_portability_enumeration"
#define PORTABILITY_SUBSET "VK_KHR_portability_subset"
#define ENUMERATE_PORTABILITY_BIT 0x1u

static bool device_has(vkf_call *k, VkPhysicalDevice pd, const char *name)
{
    PFN_vkEnumerateDeviceExtensionProperties f = VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    if (f(pd, NULL, &n, NULL) < 0)
        return false;
    VkExtensionProperties *p = vkf_alloc(k, (n + 1) * sizeof *p);
    if (f(pd, NULL, &n, p) < 0)
        return false;
    return host_has(p, n, name);
}

static const VkExtensionProperties *host_instance_exts(vkf_call *k, uint32_t *n)
{
    PFN_vkEnumerateInstanceExtensionProperties f = VKF_FN(&vkf.global, vkEnumerateInstanceExtensionProperties);
    *n = 0;
    if (!f || f(NULL, n, NULL) < 0)
        return NULL;
    VkExtensionProperties *p = vkf_alloc(k, (*n + 1) * sizeof *p);
    if (f(NULL, n, p) < 0)
        *n = 0;
    return p;
}

static bool provider_ok(const VkExtensionProperties *host, uint32_t n)
{
    const rl_vk_surface_provider *s = vkf.cfg.surfaces;
    if (!s)
        return false;
    for (const char *const *e = s->instance_extensions; e && *e; e++)
        if (!host_has(host, n, *e))
            return false;
    return true;
}

/** @brief Writes a filtered extension list with the count-and-array protocol. */
static int32_t answer_exts(vkf_call *k, const VkExtensionProperties *list, uint32_t n, uint64_t gcount, uint64_t garray)
{
    if (!gcount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!garray) {
        rl_st32(k->mem, gcount, n);
        return VK_SUCCESS;
    }
    uint32_t cap = vkf_g32(k, gcount);
    uint32_t w = cap < n ? cap : n;
    memcpy(k->mem + garray, list, w * sizeof *list);
    rl_st32(k->mem, gcount, w);
    return w < n ? VK_INCOMPLETE : VK_SUCCESS;
}

int vkh_vkEnumerateInstanceExtensionProperties(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    if (c->x[0]) {
        rl_st32(mem, c->x[1], 0);
        c->x[0] = (uint32_t)VK_ERROR_LAYER_NOT_PRESENT;
        return vkf_end(&k);
    }
    uint32_t n;
    const VkExtensionProperties *host = host_instance_exts(&k, &n);
    VkExtensionProperties *out = vkf_alloc(&k, (vkf_switch.instance_ext_count + 1) * sizeof *out);
    uint32_t m = 0;
    for (uint32_t i = 0; i < vkf_switch.instance_ext_count; i++) {
        const VkExtensionProperties *e = &vkf_switch.instance_exts[i];
        bool vi = !strcmp(e->extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME);
        if (vi ? provider_ok(host, n) : in_list(vkg_instance_exts, e->extensionName) && host_has(host, n, e->extensionName))
            out[m++] = *e;
    }
    c->x[0] = (uint32_t)answer_exts(&k, out, m, c->x[1], c->x[2]);
    return vkf_end(&k);
}

int vkh_vkEnumerateDeviceExtensionProperties(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkEnumerateDeviceExtensionProperties, c->x[0]);
    PFN_vkEnumerateDeviceExtensionProperties f = VKF_FN(k.fns, vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    if (c->x[1] || f(pd, NULL, &n, NULL) < 0) {
        rl_st32(mem, c->x[2], 0);
        c->x[0] = c->x[1] ? (uint32_t)VK_ERROR_LAYER_NOT_PRESENT : (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
        return vkf_end(&k);
    }
    VkExtensionProperties *host = vkf_alloc(&k, (n + 1) * sizeof *host);
    if (f(pd, NULL, &n, host) < 0)
        n = 0;
    VkExtensionProperties *out = vkf_alloc(&k, (vkf_switch.device_ext_count + 1) * sizeof *out);
    uint32_t m = 0;
    for (uint32_t i = 0; i < vkf_switch.device_ext_count; i++) {
        const VkExtensionProperties *e = &vkf_switch.device_exts[i];
        if (!in_list(vkg_device_exts, e->extensionName))
            continue;
        if (host_has(host, n, e->extensionName))
            out[m++] = *e;
        else if (rl_once(0x766b657800000000ull | i))
            rl_log(RL_LOG_WARN, "vk", "the host device lacks %s, which the Switch has; the guest does not see it", e->extensionName);
    }
    c->x[0] = (uint32_t)answer_exts(&k, out, m, c->x[2], c->x[3]);
    return vkf_end(&k);
}


int vkh_vkCreateInstance(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstanceCreateInfo *ci = vkg_ain_VkInstanceCreateInfo(&k, c->x[0], 1);
    uint64_t gout = c->x[2];
    if (!ci || !gout) {
        c->x[0] = (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
        return vkf_end(&k);
    }
    uint32_t n_host;
    const VkExtensionProperties *host = host_instance_exts(&k, &n_host);
    const rl_vk_surface_provider *sp = vkf.cfg.surfaces;
    uint32_t n_prov = 0;
    for (const char *const *e = sp ? sp->instance_extensions : NULL; e && *e; e++)
        n_prov++;
    const char **exts = vkf_alloc(&k, (ci->enabledExtensionCount + n_prov + 2) * sizeof *exts);
    uint32_t n = 0;
    bool vi = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, VK_NN_VI_SURFACE_EXTENSION_NAME)) {
            vi = true;
            continue;
        }
        if (!in_list(vkg_instance_exts, e)) {
            rl_log(RL_LOG_WARN, "vk", "vkCreateInstance: extension %s is not forwarded", e);
            c->x[0] = (uint32_t)VK_ERROR_EXTENSION_NOT_PRESENT;
            return vkf_end(&k);
        }
        exts[n++] = e;
    }
    if (vi) {
        if (!provider_ok(host, n_host)) {
            rl_log(RL_LOG_WARN, "vk", "vkCreateInstance: VK_NN_vi_surface needs a surface provider the host supports");
            c->x[0] = (uint32_t)VK_ERROR_EXTENSION_NOT_PRESENT;
            return vkf_end(&k);
        }
        for (const char *const *e = sp->instance_extensions; e && *e; e++) {
            bool have = false;
            for (uint32_t i = 0; i < n; i++)
                have = have || !strcmp(exts[i], *e);
            if (!have)
                exts[n++] = *e;
        }
    }
    if (host_has(host, n_host, PORTABILITY_ENUMERATION)) {
        exts[n++] = PORTABILITY_ENUMERATION;
        ci->flags |= ENUMERATE_PORTABILITY_BIT;
    }
    ci->ppEnabledExtensionNames = exts;
    ci->enabledExtensionCount = n;
    uint32_t host_version = VKF_API_VERSION(1, 0);
    PFN_vkEnumerateInstanceVersion ev = VKF_FN(&vkf.global, vkEnumerateInstanceVersion);
    if (ev)
        ev(&host_version);
    VkApplicationInfo *app = vkf_alloc(&k, sizeof *app);
    if (ci->pApplicationInfo)
        *app = *ci->pApplicationInfo;
    else
        *app = (VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VKF_API_VERSION(1, 0) };
    app->pNext = NULL;
    uint32_t want = host_version >= VKF_API_VERSION(1, 3) ? VKF_API_VERSION(1, 3) : host_version;
    if (app->apiVersion < want)
        app->apiVersion = want;
    ci->pApplicationInfo = app;
    VkInstance inst = NULL;
    VkResult r = VKF_FN(&vkf.global, vkCreateInstance)(ci, NULL, &inst);
    if (r < 0) {
        vkf_cb_release(0);
        c->x[0] = (uint32_t)r;
        return vkf_end(&k);
    }
    vkf_inst *st = calloc(1, sizeof *st);
    if (!st)
        abort();
    st->api_version = app->apiVersion;
    st->vi_surface = vi;
    vkf_load_instance(&st->fns, inst);
    vkf_obj *o = vkf_obj_add(VKG_KIND_VkInstance, inst, NULL, &st->fns);
    o->inst = st;
    vkf_cb_adopt((uint64_t)(uintptr_t)inst);
    vkf_s64(&k, gout, vkf_id(o));
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

int vkh_vkDestroyInstance(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    if (!c->x[0])
        return vkf_end(&k);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkDestroyInstance, c->x[0]);
    vkf_obj *o = k.obj;
    VKF_FN(k.fns, vkDestroyInstance)(inst, NULL);
    vkf_cb_release((uint64_t)(uintptr_t)inst);
    vkf_obj_remove_children(o);
    vkf_inst *st = o->inst;
    vkf_obj_remove(o);
    free(st);
    return vkf_end(&k);
}


int vkh_vkCreateDevice(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkCreateDevice, c->x[0]);
    vkf_obj *pobj = k.obj;
    VkDeviceCreateInfo *ci = vkg_ain_VkDeviceCreateInfo(&k, c->x[1], 1);
    uint64_t gout = c->x[3];
    if (!ci || !gout) {
        c->x[0] = (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
        return vkf_end(&k);
    }
    vkf_dev *d = calloc(1, sizeof *d);
    if (!d)
        abort();
    pthread_mutex_init(&d->lock, NULL);
    pthread_mutex_init(&d->queue_lock, NULL);
    d->mems = vkf_map_new();
    pthread_mutex_init(&d->dpool_lock, NULL);
    d->dpools = vkf_map_new();
    const char **extra = NULL;
    uint32_t n_extra = 0;
    void *chain = NULL;
    VkResult r = vkf_props_device_check(&k, pd, ci, d);
    if (r < 0) {
        vkf_map_free(d->mems);
        pthread_mutex_destroy(&d->queue_lock);
        pthread_mutex_destroy(&d->lock);
        free(d);
        c->x[0] = (uint32_t)r;
        return vkf_end(&k);
    }
    vkf_mem_types_setup(&k, d, pd);
    vkf_mem_device_setup(&k, d, pd, &extra, &n_extra, &chain);
    const char **exts = vkf_alloc(&k, (ci->enabledExtensionCount + n_extra + 2) * sizeof *exts);
    uint32_t n = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        exts[n++] = ci->ppEnabledExtensionNames[i];
    if (device_has(&k, pd, PORTABILITY_SUBSET))
        exts[n++] = PORTABILITY_SUBSET;
    for (uint32_t j = 0; j < n_extra; j++) {
        bool have = false;
        for (uint32_t i = 0; i < n; i++)
            have = have || !strcmp(exts[i], extra[j]);
        if (!have)
            exts[n++] = extra[j];
    }
    ci->ppEnabledExtensionNames = exts;
    ci->enabledExtensionCount = n;
    if (chain) {
        VkBaseOutStructure *last = chain;
        while (last->pNext)
            last = last->pNext;
        last->pNext = (VkBaseOutStructure *)ci->pNext;
        ci->pNext = chain;
    }
    VkDevice dev = NULL;
    r = vkf_queue_create_infos(&k, pd, ci, d);
    if (r >= 0)
        r = VKF_FN(k.fns, vkCreateDevice)(pd, ci, NULL, &dev);
    if (r < 0) {
        vkf_map_free(d->dpools);
        pthread_mutex_destroy(&d->dpool_lock);
        vkf_map_free(d->mems);
        pthread_mutex_destroy(&d->queue_lock);
        pthread_mutex_destroy(&d->lock);
        free(d);
        c->x[0] = (uint32_t)r;
        return vkf_end(&k);
    }
    vkf_load_device(&d->fns, k.fns, dev);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount && d->nqueue_families < 16; i++) {
        d->queue_families[d->nqueue_families] = ci->pQueueCreateInfos[i].queueFamilyIndex;
        d->queue_counts[d->nqueue_families++] = ci->pQueueCreateInfos[i].queueCount;
    }
    vkf_obj *o = vkf_obj_add(VKG_KIND_VkDevice, dev, pobj, &d->fns);
    o->dev = d;
    vkf_s64(&k, gout, vkf_id(o));
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

int vkh_vkDestroyDevice(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    if (!c->x[0])
        return vkf_end(&k);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkDestroyDevice, c->x[0]);
    vkf_obj *o = k.obj;
    vkf_dev *d = o->dev;
    vkf_swap_device_teardown(o);
    vkf_desc_device_teardown(&k, d, dev);
    VKF_FN(k.fns, vkDestroyDevice)(dev, NULL);
    vkf_mem_device_teardown(d);
    vkf_obj_remove_children(o);
    vkf_obj_remove(o);
    vkf_map_free(d->mems);
    pthread_mutex_destroy(&d->dpool_lock);
    pthread_mutex_destroy(&d->queue_lock);
    pthread_mutex_destroy(&d->lock);
    free(d);
    return vkf_end(&k);
}


int vkh_vkAllocateCommandBuffers(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkAllocateCommandBuffers, c->x[0]);
    const VkCommandBufferAllocateInfo *ai = vkg_ain_VkCommandBufferAllocateInfo(&k, c->x[1], 1);
    uint64_t gout = c->x[2];
    uint32_t n = ai ? ai->commandBufferCount : 0;
    VkCommandBuffer *h = vkf_alloc(&k, (n ? n : 1) * sizeof *h);
    VkResult r = VKF_FN(k.fns, vkAllocateCommandBuffers)(dev, ai, h);
    if (r >= 0)
        for (uint32_t i = 0; i < n; i++) {
            vkf_obj *o = vkf_obj_add(VKG_KIND_VkCommandBuffer, h[i], k.obj, k.obj->fns);
            o->pool = (uint64_t)(uintptr_t)ai->commandPool;
            vkf_s64(&k, gout + 8u * i, vkf_id(o));
        }
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

int vkh_vkFreeCommandBuffers(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkFreeCommandBuffers, c->x[0]);
    VkCommandPool pool = (VkCommandPool)(uintptr_t)c->x[1];
    uint32_t n = (uint32_t)c->x[2];
    uint64_t g = c->x[3];
    VkCommandBuffer *h = (VkCommandBuffer *)vkf_unwrap_n(&k, VKG_KIND_VkCommandBuffer, g, n);
    VKF_FN(k.fns, vkFreeCommandBuffers)(dev, pool, n, h);
    for (uint32_t i = 0; g && i < n; i++)
        vkf_obj_remove(vkf_obj_of(vkf_g64(&k, g + 8u * i), VKG_KIND_VkCommandBuffer));
    return vkf_end(&k);
}

int vkh_vkDestroyCommandPool(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkDestroyCommandPool, c->x[0]);
    uint64_t pool = c->x[1];
    VKF_FN(k.fns, vkDestroyCommandPool)(dev, (VkCommandPool)(uintptr_t)pool, NULL);
    if (pool) {
        pthread_mutex_lock(&vkf.lock);
        for (uint32_t s = 0; s < g_next_slot; s++) {
            vkf_obj *o = g_slots[s / CHUNK_SLOTS] ? g_slots[s / CHUNK_SLOTS][s % CHUNK_SLOTS] : NULL;
            if (o && o->kind == VKG_KIND_VkCommandBuffer && o->pool == pool && o->parent == k.obj)
                remove_locked(o);
        }
        pthread_mutex_unlock(&vkf.lock);
    }
    return vkf_end(&k);
}
