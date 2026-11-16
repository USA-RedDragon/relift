/**
 * @file vi.h
 * @brief The vi display services, the hosbinder driver and the buffer queue
 * behind every layer, with a small host side consumer interface.
 *
 * Behavior follows Ryujinx (MIT, Ryujinx.HLE/HOS/Services/Vi and
 * SurfaceFlinger) and Android's BufferQueue, which the Switch's nvnflinger
 * is derived from. Layers are the buffer queues a guest renders into; a host
 * presentation layer takes the queued frames with rl_vi_acquire_frame and
 * hands the buffers back with rl_vi_release_frame. Pixels are never touched
 * here: a frame names its nvmap buffer and describes it.
 */
#ifndef RELIFT_VI_H
#define RELIFT_VI_H

#include "services.h"

#include <pthread.h>
#include <string.h>

/** @brief vi results (module 114). */
#define VI_RESULT_INVALID_ARGUMENTS RL_MAKE_RESULT(114, 1)
#define VI_RESULT_INVALID_LAYER_SIZE RL_MAKE_RESULT(114, 4)
#define VI_RESULT_PERMISSION_DENIED RL_MAKE_RESULT(114, 5)
#define VI_RESULT_INVALID_SCALING_MODE RL_MAKE_RESULT(114, 6)
#define VI_RESULT_INVALID_VALUE RL_MAKE_RESULT(114, 7)
#define VI_RESULT_ALREADY_OPENED RL_MAKE_RESULT(114, 9)
/** @brief Returned by rl_vi_acquire_frame when no frame is ready. */
#define VI_RESULT_NO_FRAME RL_MAKE_RESULT(114, 100)

/** @brief One frame a guest queued on a layer, as the host side sees it. */
typedef struct rl_vi_frame {
    /** @brief Buffer queue slot; pass it to rl_vi_release_frame. */
    uint32_t slot;
    /** @brief nvmap handle of the buffer (the surface's handle, else the graphic buffer's nvmap id). */
    uint32_t nvmap_handle;
    /** @brief nvmap id stored in the graphic buffer. */
    uint32_t nvmap_id;
    /** @brief Byte offset of the pixels inside the nvmap buffer. */
    uint32_t offset;
    /** @brief Buffer width in pixels. */
    uint32_t width;
    /** @brief Buffer height in pixels. */
    uint32_t height;
    /** @brief Android pixel format (1 RGBA8888, 2 RGBX8888, 5 BGRA8888, ...). */
    uint32_t format;
    /** @brief Stride in pixels. */
    uint32_t stride;
    /** @brief Gralloc usage bits. */
    uint32_t usage;
    /** @brief Pitch of the first surface in bytes. */
    uint32_t pitch;
    /** @brief Layout of the first surface (0 pitch linear, 1 tiled, 2 block linear). */
    uint32_t layout;
    /** @brief Kind of the first surface. */
    uint32_t kind;
    /** @brief Block height (log2 of gobs) of a block linear surface. */
    uint32_t block_height_log2;
    /** @brief Nintendo color format of the first surface. */
    uint64_t color_format;
    /** @brief Size in bytes of the first surface. */
    uint64_t size;
    /** @brief Crop rectangle: left, top, right, bottom. */
    int32_t crop[4];
    /** @brief Transform flags (1 flip x, 2 flip y, 4 rotate 90, 8 inverse display). */
    uint32_t transform;
    /** @brief Scaling mode. */
    uint32_t scaling_mode;
    /** @brief Frames the guest wants this one shown for (vsync periods). */
    uint32_t swap_interval;
    /** @brief True when the guest attached a fence the presentation must wait for. */
    bool fence_present;
    /** @brief Number of fences attached (0 to 4). */
    uint32_t fence_count;
    /** @brief Sync point id and value of each fence. */
    uint32_t fence_id[4], fence_value[4];
    /** @brief Frame counter of the layer. */
    uint64_t frame_number;
    /** @brief Guest timestamp of the frame. */
    int64_t timestamp;
} rl_vi_frame;

/** @brief Sets the display resolution reported to guests (default 1280x720). */
RL_RUNTIME_API void rl_vi_set_resolution(uint32_t width, uint32_t height);
/** @brief Number of layers that exist. */
RL_RUNTIME_API uint32_t rl_vi_layer_count(void);
/** @brief Id of the index-th layer (in creation order); false when index is out of range. */
RL_RUNTIME_API bool rl_vi_layer_id(uint32_t index, uint64_t *id);
/** @brief Id of the layer the guest opened or created last (the one to present); false when there is none. */
RL_RUNTIME_API bool rl_vi_render_layer(uint64_t *id);
/** @brief Size of a layer: the one SetLayerSize gave, else the display resolution; false for an unknown layer. */
RL_RUNTIME_API bool rl_vi_layer_size(uint64_t id, uint32_t *width, uint32_t *height);
/**
 * @brief Takes the next queued frame of layer: the slot becomes owned by the
 * host until rl_vi_release_frame. Returns KR_SUCCESS, VI_RESULT_INVALID_VALUE
 * for an unknown layer or VI_RESULT_NO_FRAME when nothing is queued (or a
 * frame is still held).
 */
RL_RUNTIME_API rl_result rl_vi_acquire_frame(uint64_t layer, rl_vi_frame *out);
/** @brief Gives the buffer in slot (acquired earlier) back to the guest and signals the producer's event. */
RL_RUNTIME_API rl_result rl_vi_release_frame(uint64_t layer, uint32_t slot);
/** @brief Sets a function called (on the guest thread, without locks held) each time a frame is queued; NULL removes it. */
RL_RUNTIME_API void rl_vi_set_frame_callback(void (*fn)(uint64_t layer, void *user), void *user);
/** @brief Signals the vsync event once now (tests). */
RL_RUNTIME_API void rl_vi_vsync_now(void);
/** @brief Stops the vsync thread and wakes producers blocked in DequeueBuffer. Safe to call twice; shutdown calls it. */
RL_RUNTIME_API void rl_vi_stop(void);

/* Internal to the vi service files below. */

/** @brief Android status codes the producer returns. */
enum {
    VI_S_OK = 0,
    VI_S_WOULD_BLOCK = -11,
    VI_S_NO_MEMORY = -12,
    VI_S_BUSY = -16,
    VI_S_NO_INIT = -19,
    VI_S_BAD_VALUE = -22,
    VI_S_NOT_ENOUGH_DATA = -61,
    VI_S_INVALID_OPERATION = -37,
    VI_S_UNKNOWN_TRANSACTION = -74,
    VI_S_BUFFER_NEEDS_REALLOCATION = 1,
    VI_S_RELEASE_ALL_BUFFERS = 2,
    VI_S_STALE_BUFFER_SLOT = 1,
    VI_S_NO_BUFFER_AVAILABLE = 2
};

/** @brief Buffer slot states. */
enum { VI_FREE = 0, VI_DEQUEUED = 1, VI_QUEUED = 2, VI_ACQUIRED = 3 };
/** @brief Layer states. */
enum { VI_LAYER_MANAGED_CLOSED = 1, VI_LAYER_MANAGED_OPENED = 2, VI_LAYER_STRAY = 3 };
/** @brief Native window apis a producer connects with. */
enum { VI_API_NONE = 0, VI_API_NVN = 1, VI_API_CPU = 2, VI_API_MEDIA = 3, VI_API_CAMERA = 4 };

/** @brief Buffer slots per queue. */
#define VI_SLOTS 64
/** @brief Fewest bytes of a parcel buffer the binder takes or fills. */
#define VI_PARCEL_MAX 0x1000
/** @brief Bytes of a flattened graphic buffer: 0x28 header and 0x144 of nvmap description. */
#define VI_GFX_SIZE 0x16C
/** @brief Bytes of an android fence: a count and four sync point (id, value) pairs. */
#define VI_FENCE_SIZE 0x24
/** @brief Ints in the nvmap part of a graphic buffer. */
#define VI_GFX_INTS 0x51
/** @brief Layers that can exist at once. */
#define VI_MAX_LAYERS 64

/** @brief A flattened android graphic buffer. */
typedef struct vi_gfx {
    uint8_t raw[VI_GFX_SIZE];
} vi_gfx;

enum {
    VI_GFX_WIDTH = 0x04,
    VI_GFX_HEIGHT = 0x08,
    VI_GFX_STRIDE = 0x0C,
    VI_GFX_FORMAT = 0x10,
    VI_GFX_USAGE = 0x14,
    VI_GFX_INTS_COUNT = 0x24,
    VI_GFX_NVMAP_ID = 0x2C,
    VI_GFX_NV_USAGE = 0x40,
    VI_GFX_SURF0 = 0x5C,
    VI_GFX_SURF_COLOR = VI_GFX_SURF0 + 0x08,
    VI_GFX_SURF_LAYOUT = VI_GFX_SURF0 + 0x10,
    VI_GFX_SURF_PITCH = VI_GFX_SURF0 + 0x14,
    VI_GFX_SURF_HANDLE = VI_GFX_SURF0 + 0x18,
    VI_GFX_SURF_OFFSET = VI_GFX_SURF0 + 0x1C,
    VI_GFX_SURF_KIND = VI_GFX_SURF0 + 0x20,
    VI_GFX_SURF_BLOCK = VI_GFX_SURF0 + 0x24,
    VI_GFX_SURF_SIZE = VI_GFX_SURF0 + 0x38
};

static inline uint32_t vi_gfx_u32(const vi_gfx *g, uint32_t off)
{
    uint32_t v;
    memcpy(&v, g->raw + off, 4);
    return v;
}

static inline uint64_t vi_gfx_u64(const vi_gfx *g, uint32_t off)
{
    uint64_t v;
    memcpy(&v, g->raw + off, 8);
    return v;
}

static inline void vi_gfx_set_u32(vi_gfx *g, uint32_t off, uint32_t v) { memcpy(g->raw + off, &v, 4); }

/** @brief The nvmap handle of a graphic buffer: its first surface's handle, else its nvmap id. */
static inline uint32_t vi_gfx_handle(const vi_gfx *g)
{
    uint32_t h = vi_gfx_u32(g, VI_GFX_SURF_HANDLE);
    return h ? h : vi_gfx_u32(g, VI_GFX_NVMAP_ID);
}

/** @brief Sets f to "no fence". */
void vi_fence_none(uint8_t f[VI_FENCE_SIZE]);

/** @brief One buffer slot. */
typedef struct vi_slot {
    bool has_buf;
    vi_gfx buf;
    int state;
    bool request_called;
    uint64_t frame_number;
    uint8_t fence[VI_FENCE_SIZE];
    bool acquire_called;
    bool needs_cleanup;
    bool attached_by_consumer;
    int64_t queue_time, present_time;
    bool prealloc;
} vi_slot;

/** @brief A queued frame. */
typedef struct vi_item {
    vi_gfx buf;
    uint8_t fence[VI_FENCE_SIZE];
    int32_t crop[4];
    uint32_t transform, scaling;
    int64_t timestamp;
    bool auto_timestamp;
    int32_t swap_interval;
    uint64_t frame_number;
    int slot;
    bool droppable;
    bool acquire_called;
} vi_item;

/** @brief The producer's QueueBuffer input. */
typedef struct vi_queue_in {
    int64_t timestamp;
    int32_t auto_timestamp;
    int32_t crop[4];
    uint32_t scaling, transform, sticky;
    int32_t async, swap_interval;
    uint8_t fence[VI_FENCE_SIZE];
} vi_queue_in;

/** @brief The producer's Connect and QueueBuffer output. */
typedef struct vi_queue_out {
    uint32_t width, height, transform_hint, pending;
    uint64_t frame_number;
} vi_queue_out;

/** @brief One entry of the buffer history (28 bytes in the parcel). */
typedef struct vi_hist {
    uint64_t frame_number;
    int64_t queue_time, present_time;
    int32_t state;
} vi_hist;

/** @brief A buffer queue: state shared by the producer and the consumer. */
typedef struct vi_core {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    vi_slot slots[VI_SLOTS];
    int override_max;
    bool use_async;
    bool dequeue_cannot_block;
    uint32_t default_format;
    int default_width, default_height;
    int default_max;
    int max_acquired;
    bool has_been_queued;
    uint64_t frame_counter;
    uint32_t transform_hint;
    bool abandoned;
    int connected_api;
    bool consumer_connected;
    bool consumer_controlled;
    uint32_t consumer_usage;
    vi_item queue[VI_SLOTS];
    uint32_t nqueue;
    vi_hist history[8];
    uint32_t hist_pos;
    int max_cached;
    bool active;
    hos_revent *free_ev;
    hos_revent *frame_ev;
    uint64_t layer_id;
} vi_core;

/** @brief A layer: its state, binder id and buffer queue. */
typedef struct vi_layer {
    uint32_t refs;
    uint64_t id;
    int state;
    int32_t binder_id;
    uint64_t owner;
    uint32_t width, height;
    vi_core core;
} vi_layer;

/** @brief Display resolution. */
void vi_get_resolution(uint32_t *w, uint32_t *h);
/** @brief Forgets every layer and stops the vsync thread (service installation). */
void vi_layers_reset(void);
/** @brief Creates a layer in state (VI_LAYER_*) with the next free id. */
rl_result vi_layer_create(uint64_t owner, int state, uint64_t *id);
/** @brief Finds a layer by id and adds a reference; NULL if none. */
vi_layer *vi_layer_get(uint64_t id);
/** @brief Finds a layer by binder id and adds a reference; NULL if none. */
vi_layer *vi_layer_by_binder(int32_t binder_id);
/** @brief Drops a layer reference. */
void vi_layer_unref(vi_layer *l);
/** @brief OpenLayer: ManagedClosed becomes ManagedOpened; *binder receives the producer binder id. */
rl_result vi_layer_open(uint64_t id, int32_t *binder);
/** @brief CloseLayer. */
rl_result vi_layer_close(uint64_t id);
/** @brief DestroyManagedLayer. */
rl_result vi_layer_destroy_managed(uint64_t id);
/** @brief DestroyStrayLayer. */
rl_result vi_layer_destroy_stray(uint64_t id);
/** @brief SetLayerSize: records the size a host window for the layer starts with. */
rl_result vi_layer_set_size(uint64_t id, uint64_t width, uint64_t height);
/** @brief Makes id the layer to present. */
void vi_layer_set_render(uint64_t id);
/** @brief Binder id of a layer's producer, or 0. */
int32_t vi_layer_binder(uint64_t id);
/** @brief Wakes every producer blocked on a queue and stops queues from blocking. */
void vi_layers_deactivate(void);

/** @brief Producer side of the buffer queue (BufferQueueProducer); every function returns an Android status. */
int vi_bq_request_buffer(vi_core *c, int slot, vi_gfx *gb, bool *has);
int vi_bq_set_buffer_count(vi_core *c, int count);
int vi_bq_dequeue(vi_core *c, int *slot, uint8_t fence[VI_FENCE_SIZE], bool async, uint32_t w, uint32_t h, uint32_t fmt,
                  uint32_t usage);
int vi_bq_detach(vi_core *c, int slot);
int vi_bq_detach_next(vi_core *c, vi_gfx *gb, uint8_t fence[VI_FENCE_SIZE]);
int vi_bq_attach(vi_core *c, int *slot, const vi_gfx *gb);
int vi_bq_queue(vi_core *c, int slot, const vi_queue_in *in, vi_queue_out *out);
void vi_bq_cancel(vi_core *c, int slot, const uint8_t fence[VI_FENCE_SIZE]);
int vi_bq_query(vi_core *c, uint32_t what, int *out);
int vi_bq_connect(vi_core *c, int api, bool producer_controlled, vi_queue_out *out);
int vi_bq_disconnect(vi_core *c, int api);
int vi_bq_set_preallocated(vi_core *c, int slot, const vi_gfx *gb);
int vi_bq_history(vi_core *c, int count, vi_hist *out, uint32_t *n);
/** @brief Consumer side: takes the next queued frame (BufferQueueConsumer::AcquireBuffer); *gb is the buffer. */
int vi_bq_acquire(vi_core *c, vi_item *item);
/** @brief Consumer side: gives slot back (ReleaseBuffer); frame_number UINT64_MAX accepts the slot's current frame. */
int vi_bq_release(vi_core *c, int slot, uint64_t frame_number);

/** @brief Notifies the host frame callback. */
void vi_frame_queued(vi_core *c);
/**
 * @brief Counts one guest frame, from a buffer queued to vi or a Vulkan
 * present: advances the input script and runs the per-frame cheats.
 */
void vi_guest_frame(void);
/** @brief The number of guest frames vi_guest_frame counted. */
uint64_t vi_guest_frames(void);

/** @brief Binder driver (IHOSBinderDriver) object. */
rl_ipc_object *vi_binder_new(void);
/** @brief Writes the "dispdrv" native window parcel for binder id into out; *len receives its size. */
rl_result vi_native_window(int32_t binder_id, rl_ipc_buf out, uint64_t *len);

/** @brief The vsync event (created on demand) and a start of its timer thread. */
hos_revent *vi_vsync_event(bool start);

/** @brief Time between two vsyncs of the console's display, in nanoseconds (60 Hz). */
#define VI_VSYNC_NS 16666667ll
/**
 * @brief The first vsync strictly after t (CLOCK_MONOTONIC nanoseconds).
 *
 * The vsync event and the swapchain present pacing share this one timeline,
 * so a frame shown at a vsync and the event the guest waits on line up.
 */
int64_t vi_vsync_after(int64_t t);

/** @brief The vi vsync grid and its lock to the host display (vi_clock.c). */
typedef struct vi_clock {
    /** @brief A vsync of the grid; the others are VI_VSYNC_NS apart. */
    int64_t epoch;
    /** @brief Filtered offset of host flips from the grid (ns). */
    int64_t phase;
    /** @brief Flips seen, flips in this window, flips in it far off the filtered phase. */
    uint32_t flips, seen, wild;
    /** @brief Steering stays off until flips reaches this (the host followed the grid). */
    uint32_t free_until;
    /** @brief How far the grid moved in this window, and the phase when it began. */
    int64_t window_moved, window_phase;
    bool have_phase;
    /** @brief The last window of flips lined up, so the grid is steered. */
    bool steady;
    /** @brief Steer the grid toward the host's flips; false only measures. */
    bool lock;
} vi_clock;

/** @brief A grid through epoch, steered when lock is set. */
void vi_clock_init(vi_clock *c, int64_t epoch, bool lock);
/** @brief The first vsync of the grid strictly after t. */
int64_t vi_clock_after(const vi_clock *c, int64_t t);
/** @brief Takes a time the host showed a present; returns how far the grid moved (ns). */
int64_t vi_clock_flip(vi_clock *c, int64_t t);
/** @brief Whether the grid follows the host now. */
bool vi_clock_locked(const vi_clock *c);
/** @brief A time the host display showed a present of the guest (CLOCK_MONOTONIC ns): steers the vi vsync grid. */
void vi_host_flip(int64_t t);

/** @brief Whether stats.trace names a frame trace file (frame_trace.c); read once. */
bool vi_trace_on(void);
/** @brief CLOCK_MONOTONIC nanoseconds while the frame trace is on, else 0. */
int64_t vi_trace_now(void);
/** @brief Writes one frame trace line of this kind when the trace is on. */
void vi_trace(char kind, uint64_t a, uint64_t b, uint64_t c);
/** @brief Writes a frame trace line with a and the calling thread's CPU time. */
void vi_trace_cpu(char kind, uint64_t a);

#endif
