/**
 * @file nv.h
 * @brief The nvdrv service group: the host API for the presentation layer
 * and the internal pieces shared by the device files.
 *
 * Command ids, ioctl numbers, structure layouts, result codes and the
 * behavior of every device follow Ryujinx (MIT, Copyright (c) Ryujinx Team
 * and Contributors, Ryujinx.HLE/HOS/Services/Nv) and the Linux ioctl
 * encoding used by libnx (ISC, Copyright (c) libnx Authors). Nothing
 * executes on a GPU: work submitted to a channel completes when it is
 * submitted (VIC and NVDEC work runs on the CPU at that point, see
 * host1x.h), address spaces only keep the bookkeeping of virtual ranges, and
 * nvmap keeps the guest addresses the buffers were allocated at.
 */
#ifndef RELIFT_NV_H
#define RELIFT_NV_H

#include "services.h"

/** @brief What nvmap knows about one handle. */
typedef struct rl_nv_map_info {
    /** @brief Handle id (the same value as the id GetId returns). */
    uint32_t handle_id;
    /** @brief Size in bytes, page aligned. */
    uint32_t size;
    /** @brief Alignment given at Alloc, at least one page. */
    uint32_t alignment;
    /** @brief Alloc flags. */
    uint32_t flags;
    /** @brief Memory kind given at Alloc. */
    uint32_t kind;
    /** @brief Heap mask given at Alloc. */
    uint32_t heap_id;
    /** @brief Guest address given at Alloc, 0 before Alloc. */
    uint64_t addr;
    /** @brief True once Alloc succeeded. */
    bool allocated;
} rl_nv_map_info;

/** @brief Looks up an nvmap handle by id; false when there is no such handle. */
RL_RUNTIME_API bool rl_nv_map_lookup(uint32_t handle_id, rl_nv_map_info *out);
/** @brief Forgets every nvmap handle, syncpoint and host1x mapping and turns debug mode off (tests). Open sessions keep their files. */
RL_RUNTIME_API void rl_nv_reset(void);
/** @brief Turns debug mode on or off; /dev/nvhost-dbg-gpu and /dev/nvhost-prof-gpu only open while it is on. */
RL_RUNTIME_API void rl_nv_set_debug_mode(bool on);

/** @brief Result codes of the nvdrv commands (Ryujinx NvResult). */
enum {
    NVR_SUCCESS = 0,
    NVR_NOT_IMPLEMENTED = 1,
    NVR_NOT_SUPPORTED = 2,
    NVR_NOT_INITIALIZED = 3,
    NVR_INVALID_PARAMETER = 4,
    NVR_TIMEOUT = 5,
    NVR_INSUFFICIENT_MEMORY = 6,
    NVR_READ_ONLY_ATTRIBUTE = 7,
    NVR_INVALID_STATE = 8,
    NVR_INVALID_ADDRESS = 9,
    NVR_INVALID_SIZE = 10,
    NVR_INVALID_VALUE = 11,
    NVR_ALREADY_ALLOCATED = 13,
    NVR_BUSY = 14,
    NVR_RESOURCE_ERROR = 15,
    NVR_COUNT_MISMATCH = 16,
    NVR_SHARED_MEMORY_TOO_SMALL = 0x1000,
    NVR_FILE_OPERATION_FAILED = 0x30003,
    NVR_DIRECTORY_OPERATION_FAILED = 0x30004,
    NVR_NOT_AVAILABLE_IN_PRODUCTION = 0x30006,
    NVR_IOCTL_FAILED = 0x3000F,
    NVR_ACCESS_DENIED = 0x30010,
    NVR_FILE_NOT_FOUND = 0x30013,
    NVR_MODULE_NOT_PRESENT = 0xA000E
};

/** @brief Results of the device files (Ryujinx NvInternalResult, Linux errno style). */
enum {
    NVE_OK = 0,
    NVE_NOT_PERMITTED = -1,
    NVE_NO_ENTRY = -2,
    NVE_INTERRUPTED = -4,
    NVE_IO_ERROR = -5,
    NVE_DEVICE_NOT_FOUND = -6,
    NVE_BAD_FILE_NUMBER = -9,
    NVE_TRY_AGAIN = -11,
    NVE_OUT_OF_MEMORY = -12,
    NVE_ACCESS_DENIED = -13,
    NVE_BAD_ADDRESS = -14,
    NVE_BUSY = -16,
    NVE_NOT_A_DIRECTORY = -20,
    NVE_INVALID_INPUT = -22,
    NVE_FILE_TABLE_OVERFLOW = -23,
    NVE_UNKNOWN_0x18 = -24,
    NVE_NOT_SUPPORTED = -25,
    NVE_FILE_TOO_BIG = -27,
    NVE_NO_SPACE_LEFT = -28,
    NVE_READ_ONLY_ATTRIBUTE = -30,
    NVE_NOT_IMPLEMENTED = -38,
    NVE_INVALID_STATE = -40,
    NVE_RESTART = -85,
    NVE_INVALID_ADDRESS = -99,
    NVE_TIMED_OUT = -110,
    NVE_UNKNOWN_0x72 = -114
};

/** @brief Ioctl magic numbers (the type field of the command). */
enum {
    NV_MAGIC_HOST_CUSTOM = 0x00,
    NV_MAGIC_MAP_CUSTOM = 0x01,
    NV_MAGIC_GPU_AS = 0x41,
    NV_MAGIC_GPU = 0x47,
    NV_MAGIC_HOST = 0x48
};

/** @brief Which request kinds an ioctl entry answers. */
enum {
    NV_V_IOCTL = 1,
    NV_V_IOCTL2 = 2,
    NV_V_IOCTL3 = 4
};

/** @brief Page size of nvmap and the address spaces. */
#define NV_PAGE 0x1000ull
/** @brief Most syncpoints. */
#define NV_SYNCPOINTS 192u
/** @brief Most event slots of an nvhost-ctrl file. */
#define NV_EVENTS 64u

/** @brief A fence: a syncpoint id and the value to reach. */
typedef struct nv_fence {
    uint32_t id;
    uint32_t value;
} nv_fence;
_Static_assert(sizeof(nv_fence) == 8, "fence layout");

typedef struct nv_session nv_session;
typedef struct nv_file nv_file;
typedef struct nv_dev_class nv_dev_class;

/** @brief The arguments of one ioctl: the buffer is a private copy, written back to the guest afterwards. */
typedef struct nv_args {
    /** @brief Argument buffer, the size of the larger of the guest's input and output buffers. */
    uint8_t *data;
    /** @brief Bytes of data. */
    uint32_t size;
    /** @brief Inline buffer: the extra input of Ioctl2, the extra output of Ioctl3; NULL for Ioctl. */
    uint8_t *inl;
    /** @brief Bytes of inl. */
    uint32_t inl_size;
    /** @brief The request kind (NV_V_*). */
    uint32_t variant;
    /** @brief The ioctl command word. */
    uint32_t cmd;
    /** @brief Set by a handler for a request relift cannot serve: the guest stops with this summary. */
    const char *stop;
} nv_args;

/** @brief An ioctl handler; returns an NVE_* result. */
typedef int32_t nv_ioctl_fn(nv_file *f, nv_args *a);

/**
 * @brief One ioctl of a device: dispatched by (type, nr). A stub succeeds
 * with fixed plausible values (or without doing anything) because nothing
 * executes on a GPU.
 */
typedef struct nv_ioctl_entry {
    uint8_t type;
    uint8_t nr;
    /** @brief NV_V_* bits of the request kinds it answers. */
    uint8_t variants;
    /** @brief True when the entry only returns fixed values. */
    bool stub;
    /** @brief Fewest bytes the argument buffer must have. */
    uint16_t min_size;
    const char *name;
    nv_ioctl_fn *fn;
} nv_ioctl_entry;

/** @brief A device file kind. */
struct nv_dev_class {
    /** @brief Device path. */
    const char *path;
    /** @brief True when the device only opens in debug mode. */
    bool debug_only;
    /** @brief Ioctls. */
    const nv_ioctl_entry *ioctls;
    /** @brief Number of ioctls. */
    uint32_t nioctls;
    /** @brief Sets f->priv up; returns NVE_*. May be NULL. */
    int32_t (*open)(nv_file *f);
    /** @brief Releases f->priv; may be NULL. */
    void (*close)(nv_file *f);
    /** @brief Finds the event for QueryEvent (no new reference); NULL when the device has no events. */
    int32_t (*query_event)(nv_file *f, uint32_t id, hos_revent **out);
};

/** @brief An open device file. */
struct nv_file {
    const nv_dev_class *dev;
    nv_session *sess;
    int32_t fd;
    void *priv;
    /** @brief Next open nvhost-ctrl file (only used by that device). */
    nv_file *next_ctrl;
};

/** @brief The file with descriptor fd of session s, or NULL. nv lock held. */
nv_file *nv_file_get(nv_session *s, int32_t fd);

/** @brief A host-side virtual address range allocator (free ranges, sorted). */
typedef struct nv_va {
    struct nv_va_range {
        uint64_t start, end;
    } *free;
    uint32_t n, cap;
    uint64_t lo, hi;
} nv_va;

/** @brief Initializes a with the free range [lo, hi). */
void nv_va_init(nv_va *a, uint64_t lo, uint64_t hi);
/** @brief Frees a's memory. */
void nv_va_destroy(nv_va *a);
/** @brief Finds an aligned free range of size bytes at or above start, then below it; false when there is none. Does not take it. */
bool nv_va_find(const nv_va *a, uint64_t size, uint64_t align, uint64_t start, uint64_t *out);
/** @brief True when [va, va + size) lies inside one free range. */
bool nv_va_is_free(const nv_va *a, uint64_t va, uint64_t size);
/** @brief Takes [va, va + size) out of the free ranges; false when it is not free. */
bool nv_va_take(nv_va *a, uint64_t va, uint64_t size);
/** @brief Returns [va, va + size) to the free ranges, merging neighbors. */
void nv_va_put(nv_va *a, uint64_t va, uint64_t size);

/** @brief An nvmap handle. */
typedef struct nv_map {
    int32_t id;
    uint32_t size;
    int32_t align;
    int32_t kind;
    uint32_t flags;
    uint32_t heap_mask;
    uint64_t addr;
    bool allocated;
    /** @brief Address in the host1x (DMA) space, 0 when not mapped. */
    uint64_t dma_addr;
    int64_t refs;
} nv_map;

/** @brief The nvmap handle id, or NULL. nv lock held. */
nv_map *nv_map_get(int32_t id);
/** @brief The handle whose host1x mapping holds DMA address addr, or NULL. nv lock held. */
nv_map *nv_map_by_dma(uint64_t addr);
/** @brief Frees every nvmap handle. nv lock held. */
void nv_map_reset(void);

/** @brief Syncpoint state (process wide, in memory). nv lock held for all of these. */
bool nv_sync_valid(uint32_t id);
uint32_t nv_sync_alloc(bool client_managed);
void nv_sync_release(uint32_t id);
uint32_t nv_sync_min(uint32_t id);
uint32_t nv_sync_max(uint32_t id);
uint32_t nv_sync_max_add(uint32_t id, uint32_t n);
bool nv_sync_expired(uint32_t id, uint32_t threshold);
/** @brief Marks every submitted increment of id as executed and wakes the events waiting for it. */
void nv_sync_complete(uint32_t id);
void nv_sync_reset(void);

/** @brief Forgets the channel syncpoints and the host1x address space. nv lock held. */
void nv_ch_reset(void);
/** @brief Binds channel file ch to the address space with descriptor as_fd; false when ch is not a channel. */
bool nv_ch_bind(nv_file *ch, int32_t as_fd);
/** @brief Allocates a DMA address for the nvmap handle m in the host1x space; false when the space is full. */
bool nv_host1x_map(nv_map *m, uint64_t *out);

/** @brief Takes and drops the nvdrv lock (protects every table above). */
void nv_lock(void);
void nv_unlock(void);

extern const nv_dev_class nv_dev_nvmap;
extern const nv_dev_class nv_dev_ctrl;
extern const nv_dev_class nv_dev_ctrl_gpu;
extern const nv_dev_class nv_dev_as_gpu;
extern const nv_dev_class nv_dev_gpu;
extern const nv_dev_class nv_dev_nvdec;
extern const nv_dev_class nv_dev_vic;
extern const nv_dev_class nv_dev_nvjpg;
extern const nv_dev_class nv_dev_nvenc;
extern const nv_dev_class nv_dev_dbg_gpu;
extern const nv_dev_class nv_dev_prof_gpu;

#endif
