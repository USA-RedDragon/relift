/**
 * @file vkf.h
 * @brief Internals of the Vulkan forwarding layer shared by the generated
 * thunks and converters (gen/) and the hand-written parts.
 */
#ifndef RELIFT_SWITCH_VKF_H
#define RELIFT_SWITCH_VKF_H

#include "core.h"
#include "vk.h"

#include <relift/a64/abi.h>

#include "vkg.h"

#include <pthread.h>
#include <string.h>

_Static_assert(sizeof(size_t) == 8 && sizeof(void *) == 8, "guest size_t and pointers are 8 bytes; hosts must match");

/** @brief A Vulkan API version number (variant 0). */
#define VKF_API_VERSION(major, minor) (((uint32_t)(major) << 22) | ((uint32_t)(minor) << 12))

/** @brief A host function table indexed by enum vkg_cmd. */
typedef struct vkf_fns {
    /** @brief Host entry points; NULL where the host has none. */
    void (*f[VKG_COUNT])(void);
} vkf_fns;

typedef struct vkf_dev vkf_dev;
typedef struct vkf_inst vkf_inst;

/** @brief A guest-visible dispatchable handle. */
typedef struct vkf_obj {
    /** @brief VKG_KIND_*. */
    uint32_t kind;
    /** @brief Slot in the handle table. */
    uint32_t slot;
    /** @brief Host handle. */
    void *host;
    /** @brief Owner: the instance of a physical device or device, the device of a queue or command buffer. */
    struct vkf_obj *parent;
    /** @brief Function table for calls on this handle. */
    vkf_fns *fns;
    /** @brief Command buffers: the VkCommandPool they came from. */
    uint64_t pool;
    /** @brief Command buffers: allocated at the secondary level. */
    bool secondary;
    /** @brief Secondary command buffers: flags of the last vkBeginCommandBuffer. */
    uint32_t begin_flags;
    /** @brief Secondary command buffers: a number unique to the last vkBeginCommandBuffer. */
    uint64_t gen;
    /** @brief Instances: their state. */
    vkf_inst *inst;
    /** @brief Devices, queues and command buffers: the device state. */
    vkf_dev *dev;
} vkf_obj;

/** @brief A mark in the per-thread arena. */
typedef struct vkf_mark {
    /** @brief Chunk in use. */
    void *chunk;
    /** @brief Bytes used in it. */
    size_t used;
} vkf_mark;

/** @brief One thunk call in progress. */
struct vkf_call {
    /** @brief Guest context. */
    rl_ctx *c;
    /** @brief Guest memory base. */
    uint8_t *mem;
    /** @brief Function table of the dispatchable handle the call is on. */
    const vkf_fns *fns;
    /** @brief That handle. */
    vkf_obj *obj;
    /** @brief Arena state at entry, restored at the end. */
    vkf_mark mark;
    /** @brief Return code for the lifted caller: RL_PENDING when a guest callback ended the thread. */
    int rc;
    /** @brief The call this one runs inside (a guest callback that called Vulkan again). */
    struct vkf_call *outer;
    /** @brief The calling thread gave its core up for a host wait (vkf_wait_enter). */
    bool left;
};

/** @brief Per-instance state. */
struct vkf_inst {
    /** @brief Host instance functions. */
    vkf_fns fns;
    /** @brief Host API version the instance was created with. */
    uint32_t api_version;
    /** @brief The guest enabled VK_NN_vi_surface. */
    bool vi_surface;
    /** @brief The host physical device the guest sees, chosen on first enumeration. */
    void *physical;
};

/** @brief Per-device state. */
struct vkf_dev {
    /** @brief Host device functions. */
    vkf_fns fns;
    /** @brief enum rl_vk_map_mode in use (never AUTO). */
    int map_mode;
    /** @brief Placed mapping alignment. */
    uint64_t placed_align;
    /** @brief Host-pointer import alignment. */
    uint64_t import_align;
    /** @brief Property flags of each memory type. */
    uint32_t type_flags[32];
    /** @brief Number of memory types. */
    uint32_t type_count;
    /** @brief Memory records (vkf_mem *), keyed by VkDeviceMemory. */
    struct vkf_map *mems;
    /** @brief Protects mems and the shadow state. */
    pthread_mutex_t lock;
    /** @brief Queues of the Switch family the guest created, and the host family 0 queues behind them. */
    uint32_t guest_queues, host_queues;
    /** @brief Held by every queue operation while guest queues share host queues. */
    pthread_mutex_t queue_lock;
    /** @brief Queue families the device was created with, and the queue count of each. */
    uint32_t queue_families[16], queue_counts[16];
    /** @brief Number of entries in queue_families. */
    uint32_t nqueue_families;
    /** @brief The host physical device the device was made on. */
    VkPhysicalDevice physical;
    /** @brief Its limits, which the use-site checks compare against. */
    VkPhysicalDeviceLimits host_limits;
    /** @brief Its name, for log lines. */
    char host_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    /** @brief The host memory type behind each Switch memory type. */
    uint8_t host_type[VK_MAX_MEMORY_TYPES];
    /** @brief Descriptor pool records (vk_desc.c), keyed by VkDescriptorPool. */
    struct vkf_map *dpools;
    /** @brief Protects dpools. */
    pthread_mutex_t dpool_lock;
    /** @brief Live secondary command buffers. */
    uint32_t secondaries;
    /** @brief Pool of the primaries that run submitted secondaries (vk_submit.c), created on first use. */
    VkCommandPool wrap_pool;
    /** @brief Those primaries, keyed by the secondary's host handle. */
    struct vkf_map *wraps;
    /** @brief Protects wrap_pool and wraps. */
    pthread_mutex_t wrap_lock;
    /** @brief Attachment DONT_CARE load and store ops reach the host as LOAD and STORE (vk_pass.c). */
    bool keep_attachments;
};

/** @brief Choices of the vk.keep_attachments setting, as rl_setting_enum returns them. */
enum { RL_VK_KEEP_AUTO, RL_VK_KEEP_ON, RL_VK_KEEP_OFF };

/** @brief Whether a device of this PCI vendor gets DONT_CARE attachment ops as LOAD and STORE (vk.keep_attachments). */
bool vkf_keep_attachments(uint32_t vendor);

/** @brief A small hash map from nonzero 64-bit keys to pointers. */
typedef struct vkf_map vkf_map;
/** @brief Creates an empty map. */
vkf_map *vkf_map_new(void);
/** @brief Frees a map (not the values). */
void vkf_map_free(vkf_map *m);
/** @brief Value for key, NULL when absent. */
void *vkf_map_get(const vkf_map *m, uint64_t key);
/** @brief Sets key to value (NULL removes it). */
void vkf_map_put(vkf_map *m, uint64_t key, void *value);
/** @brief Calls fn on every entry; fn may not change the map. */
void vkf_map_each(const vkf_map *m, void (*fn)(uint64_t key, void *value, void *user), void *user);

/** @brief The layer's settings and global state. */
typedef struct vkf_state {
    /** @brief The configuration given to rl_vk_init. */
    rl_vk_config cfg;
    /** @brief Host vkGetInstanceProcAddr. */
    void *(*gipa)(void *instance, const char *name);
    /** @brief Host global functions (level 0). */
    vkf_fns global;
    /** @brief Map mode forced by RELIFT_VK_MAP or the configuration, AUTO for none. */
    int forced_mode;
    /** @brief Guards the handle table and the guest address allocator. */
    pthread_mutex_t lock;
} vkf_state;

/** @brief The layer state. */
extern vkf_state vkf;

/** @brief Starts a thunk call. */
void vkf_begin(vkf_call *k, rl_ctx *c, uint8_t *mem);
/** @brief Ends a thunk call: releases the arena and returns the code for the lifted caller. */
int vkf_end(vkf_call *k);
/** @brief The innermost thunk call running on this host thread, or NULL. */
vkf_call *vkf_current(void);
/** @brief Allocates n bytes, 16-byte aligned, that live until the call ends. */
void *vkf_alloc(vkf_call *k, size_t n);
/** @brief Sets the call to the host global functions. */
void vkf_global(vkf_call *k);

/** @brief Reads a 32-bit guest value. */
static inline uint32_t vkf_g32(const vkf_call *k, uint64_t a) { return rl_ld32(k->mem, a); }
/** @brief Reads a 64-bit guest value. */
static inline uint64_t vkf_g64(const vkf_call *k, uint64_t a) { return rl_ld64(k->mem, a); }
/** @brief Writes a 64-bit guest value. */
static inline void vkf_s64(vkf_call *k, uint64_t a, uint64_t v) { rl_st64(k->mem, a, v); }
/** @brief Host pointer to guest address a, NULL for 0. */
static inline void *vkf_ptr(const vkf_call *k, uint64_t a) { return a ? k->mem + a : NULL; }
/** @brief The n-th floating-point argument (s0 to s7). */
static inline float vkf_s(const vkf_call *k, unsigned n)
{
    float f;
    memcpy(&f, &k->c->v[n].s[0], 4);
    return f;
}
/** @brief The n-th 8-byte stack argument. */
static inline uint64_t vkf_stack(const vkf_call *k, unsigned n) { return rl_ld64(k->mem, k->c->sp + 8u * n); }
/** @brief A float in the n-th stack slot. */
static inline float vkf_fstack(const vkf_call *k, unsigned n)
{
    float f;
    uint32_t v = rl_ld32(k->mem, k->c->sp + 8u * n);
    memcpy(&f, &v, 4);
    return f;
}

/** @brief Guest handle value of a table slot and kind. */
#define VKF_ID(slot, kind) (0x5600000000000000ull | ((uint64_t)(slot) << 4) | (uint64_t)(kind))

/** @brief The object behind a guest handle of the given kind, NULL when it is not one. */
vkf_obj *vkf_obj_of(uint64_t g, uint32_t kind);
/** @brief Guest handle of an object. */
static inline uint64_t vkf_id(const vkf_obj *o) { return o ? VKF_ID(o->slot, o->kind) : 0; }
/** @brief Adds an object to the handle table; returns it with its slot set, or NULL. */
vkf_obj *vkf_obj_add(uint32_t kind, void *host, vkf_obj *parent, vkf_fns *fns);
/** @brief Removes an object from the table and frees it. */
void vkf_obj_remove(vkf_obj *o);
/** @brief Removes every object whose parent chain reaches owner (not owner itself). */
void vkf_obj_remove_children(vkf_obj *owner);
/** @brief The object of a host handle, NULL when unknown. */
vkf_obj *vkf_obj_find_host(uint32_t kind, const void *host);

/** @brief Unwraps the call's first dispatchable handle and takes its function table. */
void *vkf_first(vkf_call *k, uint32_t kind, uint64_t g);
/** @brief Unwraps a guest dispatchable handle; NULL for 0 or an unknown handle. */
void *vkf_unwrap(vkf_call *k, uint32_t kind, uint64_t g);
/** @brief Unwraps n guest handles at guest address g into an arena array. */
void **vkf_unwrap_n(vkf_call *k, uint32_t kind, uint64_t g, uint64_t n);
/** @brief Guest handle for a host dispatchable handle, made on first sight under the call's object. */
uint64_t vkf_wrap(vkf_call *k, uint32_t kind, void *host);
/** @brief Host array of n string pointers from the guest array at g. */
const char *const *vkf_strv(vkf_call *k, uint64_t g, uint64_t n);

/** @brief Reports a pNext struct the layer does not know (it is dropped from the chain). */
void vkf_unknown_stype(vkf_call *k, uint32_t stype);
/** @brief Ends a call on a handle the guest never got from the layer: VK_ERROR_UNKNOWN. */
int vkf_bad_handle(vkf_call *k, int cmd, uint64_t g);
/** @brief Ends a call to a command the host lacks: VK_ERROR_FEATURE_NOT_PRESENT. */
int vkf_no_fn(vkf_call *k, int cmd);

/** @brief Loads fns from the host for an instance (level 1 and 2 through vkGetInstanceProcAddr). */
void vkf_load_instance(vkf_fns *fns, void *host_instance);
/** @brief Loads fns for a device: the instance's, with level 2 replaced through vkGetDeviceProcAddr. */
void vkf_load_device(vkf_fns *fns, const vkf_fns *inst, void *host_device);
/** @brief The host function of cmd in a table, typed. */
#define VKF_FN(fns, name) ((PFN_##name)(fns)->f[VKG_##name])

/** @brief The one queue family the Switch's driver always reports. */
#define VKF_SWITCH_QUEUE_FLAGS                                                                                         \
    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT)
#define VKF_SWITCH_QUEUE_COUNT 16u
#define VKF_SWITCH_TIMESTAMP_BITS 64u

/** @brief A pNext struct the Switch's driver fills: its sType, size and contents (sType set, pNext NULL). */
typedef struct vkf_switch_struct {
    VkStructureType stype;
    uint32_t size;
    const void *data;
} vkf_switch_struct;

/** @brief The features of one format on the Switch. */
typedef struct vkf_switch_format {
    VkFormat format;
    VkFormatProperties props;
} vkf_switch_format;

/** @brief What the Switch's own Vulkan driver reports (vk_switch.c). */
typedef struct vkf_switch_profile {
    /** @brief vkEnumerateInstanceVersion. */
    uint32_t instance_version;
    /** @brief vkGetPhysicalDeviceProperties; pipelineCacheUUID and limits.timestampPeriod are the host's. */
    VkPhysicalDeviceProperties props;
    /** @brief vkGetPhysicalDeviceFeatures. */
    VkPhysicalDeviceFeatures features;
    /** @brief vkGetPhysicalDeviceMemoryProperties. */
    VkPhysicalDeviceMemoryProperties memory;
    /** @brief Property structs the driver fills in a vkGetPhysicalDeviceProperties2 chain. */
    const vkf_switch_struct *props_ext;
    uint32_t props_ext_count;
    /** @brief Feature structs the driver fills in a vkGetPhysicalDeviceFeatures2 chain. */
    const vkf_switch_struct *features_ext;
    uint32_t features_ext_count;
    /** @brief Instance extensions of the loader, its layers and the driver. */
    const VkExtensionProperties *instance_exts;
    uint32_t instance_ext_count;
    /** @brief Device extensions of the driver and the swapchain layer. */
    const VkExtensionProperties *device_exts;
    uint32_t device_ext_count;
    /** @brief Formats with any feature, in increasing format order. */
    const vkf_switch_format *formats;
    uint32_t format_count;
} vkf_switch_profile;

/** @brief The Switch's values. */
extern const vkf_switch_profile vkf_switch;

/** @brief The Switch's properties of a format; all zero for one it does not list. */
VkFormatProperties vkf_switch_format_props(VkFormat format);
/** @brief The format the host driver uses for format f: on MoltenVK without 24-bit depth, the D32 format it makes D24 images as (vk_format.c). */
VkFormat vkf_host_format(const vkf_fns *fns, VkPhysicalDevice pd, VkFormat f);
/** @brief The Switch's extension entry of that name, NULL when it has none. */
const VkExtensionProperties *vkf_switch_ext(const VkExtensionProperties *list, uint32_t n, const char *name);

/** @brief Checks that the host can stand in for the Switch on a device the guest creates; logs and fails otherwise. */
VkResult vkf_props_device_check(vkf_call *k, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, vkf_dev *d);

/** @brief Picks the host memory type behind each of the Switch's for a new device. */
void vkf_mem_types_setup(vkf_call *k, vkf_dev *d, VkPhysicalDevice pd);
/** @brief The Switch memory types whose host types are in host_bits. */
uint32_t vkf_guest_type_bits(const vkf_dev *d, uint32_t host_bits);

/** @brief Points ci at host family 0 queues for the guest's Switch family ones and records the counts in d. */
VkResult vkf_queue_create_infos(vkf_call *k, VkPhysicalDevice pd, VkDeviceCreateInfo *ci, vkf_dev *d);

/** @brief Takes the device's queue lock when guest queues share host queues. */
void vkf_queue_lock(vkf_call *k);

/** @brief Releases what vkf_queue_lock took. */
void vkf_queue_unlock(vkf_call *k);

/** @brief vkf_queue_lock, then vkf_shadow_flush: before a submit. */
void vkf_queue_enter(vkf_call *k);

/** @brief vkf_shadow_pull, then vkf_queue_unlock: after a queue wait. */
void vkf_queue_pull_leave(vkf_call *k);
/** @brief Gives the calling guest thread's core up for a host wait with a timeout other than 0. */
void vkf_wait_enter(vkf_call *k, uint64_t timeout);
/** @brief Ends vkf_wait_enter: waits for the core again. Call with no host lock held. */
void vkf_wait_return(vkf_call *k);

/** @brief Copies bytes to a fresh area below the guest stack pointer *sp; returns its guest address. */
uint64_t vkf_push(vkf_call *k, uint64_t *sp, const void *src, size_t n);
/** @brief Calls a guest function with up to eight integer arguments on the guest stack below sp; returns x0. */
uint64_t vkf_guest_call(vkf_call *k, uint64_t fn, const uint64_t *args, unsigned n, uint64_t sp);

/** @brief Allocates a guest range of size bytes aligned to align in the map window; 0 on failure. */
uint64_t vkf_va_alloc(uint64_t size, uint64_t align);
/** @brief Frees a range from vkf_va_alloc. */
void vkf_va_free(uint64_t addr);
/** @brief Changes the guest map through the configuration's hook. */
int vkf_vm_set(uint64_t addr, uint64_t size, uint32_t perm, int content);

/** @brief Chooses a map mode for a physical device and fills the device state; returns the extensions and features to enable. */
void vkf_mem_device_setup(vkf_call *k, vkf_dev *d, void *phys, const char ***exts, uint32_t *n_exts, void **pnext);
/** @brief Releases every memory record of a device (after vkDestroyDevice). */
void vkf_mem_device_teardown(vkf_dev *d);
/** @brief Frees the primary that runs a secondary command buffer the guest frees. */
void vkf_wrap_drop(const vkf_fns *fns, void *dev, vkf_dev *d, void *secondary);
/** @brief Destroys the primaries that run submitted secondaries. */
void vkf_wrap_device_teardown(const vkf_fns *fns, void *dev, vkf_dev *d);
/** @brief Destroys the host descriptor pools added behind guest pools and frees their records. */
void vkf_desc_device_teardown(vkf_call *k, vkf_dev *d, void *dev);
/** @brief Copies every shadow-mapped range of the call's device from guest to host. */
void vkf_shadow_flush(vkf_call *k);
/** @brief Copies host-cached shadow-mapped ranges of the call's device from host to guest. */
void vkf_shadow_pull(vkf_call *k);

/** @brief Guest extent (the layer's size), host drawable extent and its generation for a surface, from the provider. */
void vkf_surface_extent(uint64_t surface, uint32_t guest[2], uint32_t host[2], uint64_t *generation);
/** @brief Rewrites host surface capabilities into the ones a guest swapchain on the surface gets. */
void vkf_vi_caps(uint64_t surface, VkSurfaceCapabilitiesKHR *caps);
/** @brief Destroys every guest swapchain of a device (before the host device goes). */
void vkf_swap_device_teardown(const vkf_obj *dobj);
/** @brief True for commands on swapchains the layer does not forward: their lookups return NULL. */
bool vkf_swap_unsupported(const char *name);

/** @brief Frees callback records owned by a host handle (a messenger or callback); 0 frees the unowned ones. */
void vkf_cb_release(uint64_t owner);
/** @brief Gives the unowned callback records made during this call to owner. */
void vkf_cb_adopt(uint64_t owner);

#endif
