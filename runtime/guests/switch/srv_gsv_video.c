/**
 * @file srv_gsv_video.c
 * @brief The Switch 2 video decoder sessions the graphics supervisor hands out (gsv 902, 905, 906 and 1002) and
 * the decoder service each creates (nn::codec::video::detail::ipc).
 *
 * Command ids and layouts follow the SDK's lifted nn::codec video code and Eglantine's description of the
 * service (values only).
 */
#include "nvn/nvn_priv.h"
#include "services.h"
#include "vi.h"

#include <relift/video.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** @brief Decoder output images the supervisor tracks (ids are handed out from 1, the lowest free run first). */
#define IMAGES_MAX 256u
#define ROW_ALIGN 64u
#define PLANE_ROWS 128u

typedef struct image {
    bool used;
    uint32_t pref[16];
    uint32_t memory;
    uint64_t offset;
    /** @brief The client reads the image on the GPU (queried with 428 or 426): it is written as block-linear RGBA. */
    bool gpu_read;
    uint32_t gpu_maps;
} image;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static image g_images[IMAGES_MAX];

typedef struct layout {
    uint32_t code, width, height;
    bool nv12;
    uint64_t pitch, chroma_offset, size;
} layout;

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

/**
 * @brief The layout the supervisor gives a decoder output image for a preference (MakeDecodeImagePreference):
 * word 1 is the output layout code (1 and 4 NV12, 2 and 5 BGRA, the rest RGBA) and word 2 holds (width - 1) |
 * (height - 1) << 16. Pitch-linear rows of 64-byte multiples, NV12's half-height chroma plane below the luma
 * plane, each plane padded to 128 rows (inferred: Eglantine; the console's layout was never captured).
 */
static layout image_layout(const uint32_t *pref)
{
    layout l = { 0 };
    l.code = pref[1];
    l.width = (pref[2] & 0xffff) + 1;
    l.height = (pref[2] >> 16) + 1;
    l.nv12 = l.code == 1 || l.code == 4;
    l.pitch = align_up((uint64_t)l.width * (l.nv12 ? 1 : 4), ROW_ALIGN);
    l.chroma_offset = l.pitch * align_up(l.height, PLANE_ROWS);
    l.size = l.chroma_offset + (l.nv12 ? l.pitch * align_up((l.height + 1) / 2, PLANE_ROWS) : 0);
    return l;
}

/** @brief UserSession 991 (nn::codec::CalculateDecoderImagePoolBuffer): preference[16] and a u32 count; a u64 size and a u64 alignment. */
rl_result rl_gsv_decoder_pool_size(rl_ipc_req *r)
{
    struct {
        uint32_t pref[16];
        uint32_t count;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    uint64_t *out = rl_ipc_out(r, 16);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    layout l = image_layout(in.pref);
    out[0] = l.size * (in.count ? in.count : 1);
    out[1] = (uint64_t)ROW_ALIGN * PLANE_ROWS;
    return KR_SUCCESS;
}

/**
 * @brief UserSession 911 (nn::codec::InitializeDecoderImagePool): preference[16], a u32 count and a u32 memory id;
 * a u32, the id of the first of count images (image i of the pool is that id plus i), one image size apart in the
 * pool's memory.
 */
rl_result rl_gsv_decoder_pool_init(rl_ipc_req *r)
{
    struct {
        uint32_t pref[16];
        uint32_t count, memory;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    uint32_t count = in.count ? in.count : 1;
    layout l = image_layout(in.pref);
    pthread_mutex_lock(&g_mu);
    uint32_t first = 1, run = 0;
    for (uint32_t i = 1; i < IMAGES_MAX && run < count; i++) {
        if (g_images[i].used)
            run = 0;
        else if (++run == count)
            first = i + 1 - count;
    }
    if (run < count) {
        pthread_mutex_unlock(&g_mu);
        return KR_OUT_OF_RESOURCE;
    }
    for (uint32_t i = 0; i < count; i++) {
        image *m = &g_images[first + i];
        m->used = true;
        memcpy(m->pref, in.pref, sizeof m->pref);
        m->memory = in.memory;
        m->offset = l.size * i;
    }
    pthread_mutex_unlock(&g_mu);
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = first;
    return KR_SUCCESS;
}

/**
 * @brief UserSession 961 (GetCpuUserImageDescriptor): a u32 image id; the 128-byte nn::gsv::CpuUserImageDescriptor
 * nn::gs::sdk::ReadUserImage* copy an image out with: u8 kind at 0 (8 RGBA, 9 BGRA, 10 NV12), the plane's offset in
 * the pool memory (u32 at 8) and row pitch (u64 at 0x10), NV12's chroma plane at 0x18 and 0x20.
 */
rl_result rl_gsv_decoder_image_descriptor(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, 4))
        return SF_RESULT_INVALID_HEADER;
    uint8_t *out = rl_ipc_out(r, 128);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    image m = id < IMAGES_MAX ? g_images[id] : (image){ 0 };
    pthread_mutex_unlock(&g_mu);
    if (!m.used)
        return SF_RESULT_INVALID_HEADER;
    layout l = image_layout(m.pref);
    out[0] = l.nv12 ? 10 : l.code == 2 || l.code == 5 ? 9 : 8;
    uint32_t off = (uint32_t)m.offset;
    memcpy(out + 8, &off, 4);
    memcpy(out + 0x10, &l.pitch, 8);
    if (l.nv12) {
        uint64_t c = m.offset + l.chroma_offset;
        memcpy(out + 0x18, &c, 8);
        memcpy(out + 0x20, &l.pitch, 8);
    }
    return KR_SUCCESS;
}


/** @brief Native memory blocks (gsv 111, 112, 114): the process memory each is bound to. */
#define MEMORIES_MAX 64u

typedef struct memory {
    bool used;
    uint64_t size, addr;
} memory;

static memory g_memories[MEMORIES_MAX];

/** @brief UserSession 111 (CreateNativeMemory): a u32 mode and a u64 size; a u32 memory id. */
rl_result rl_gsv_native_memory_create(rl_ipc_req *r)
{
    struct __attribute__((packed)) {
        uint32_t mode;
        uint64_t size;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    uint32_t id = 0;
    for (uint32_t i = 1; i < MEMORIES_MAX && !id; i++)
        if (!g_memories[i].used)
            id = i;
    if (id) {
        g_memories[id].used = true;
        g_memories[id].size = in.size;
        g_memories[id].addr = 0;
    }
    pthread_mutex_unlock(&g_mu);
    uint32_t *out = rl_ipc_out(r, 4);
    if (!id || !out)
        return id ? SF_RESULT_INVALID_HEADER : KR_OUT_OF_RESOURCE;
    *out = id;
    return KR_SUCCESS;
}

/** @brief UserSession 112 (DestroyNativeMemory): a u32 memory id. */
rl_result rl_gsv_native_memory_destroy(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, 4) || id >= MEMORIES_MAX)
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    memset(&g_memories[id], 0, sizeof g_memories[id]);
    pthread_mutex_unlock(&g_mu);
    return KR_SUCCESS;
}

/** @brief UserSession 114 (BindNativeMemory): a u32 memory id, then u64 address, size, flags and offset: backs the memory with process memory. */
rl_result rl_gsv_native_memory_bind(rl_ipc_req *r)
{
    struct __attribute__((packed)) {
        uint32_t id;
        uint64_t addr, size, flags, offset;
    } in;
    if (!rl_srv_in(r, &in, sizeof in) || in.id >= MEMORIES_MAX)
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    g_memories[in.id].addr = in.addr;
    pthread_mutex_unlock(&g_mu);
    return KR_SUCCESS;
}

/** @brief A frame decoded but not yet in an output image (NV12 planes, tightly packed). */
typedef struct frame {
    uint32_t width, height;
    uint64_t pts;
    bool end;
    uint8_t *data;
} frame;

#define RING 16u

typedef struct handed {
    uint32_t image;
    uint64_t value;
} handed;

/** @brief A submitted frame's bitstream, not decoded yet. */
typedef struct input {
    uint8_t *data;
    size_t size;
    uint64_t pts;
    bool end;
} input;

typedef struct finished {
    uint32_t image, width, height;
    uint64_t pts;
    bool end;
} finished;

/** @brief State of a decoder session: its configuration and, once configured, the stream. */
typedef struct video_session {
    pthread_mutex_t mu;
    uint32_t width, height, rate;
    char codec[32];
    hos_revent *input_event;
    const rl_vp9_stream_backend *vp9;
    const rl_av1_stream_backend *av1;
    void *dec;
    uint32_t counter;
    uint64_t handed_total;
    uint64_t submitted;
    handed free[RING];
    uint32_t nfree;
    /** @brief Decoded frames not yet in an image; the extra slot keeps the end-of-stream marker when the ring is full. */
    frame pending[RING + 1];
    uint32_t npending;
    finished done[RING];
    uint32_t ndone;
    /**
     * @brief Submitted frames not decoded yet, oldest first (inputs[head] to inputs[ninputs - 1]): the decoder takes
     * the next only while the decoded ring has room, as the console's decoder waits for an output image, so no
     * decoded frame is ever dropped however far ahead the game submits.
     */
    input *inputs;
    size_t head, ninputs, inputs_cap;
    /** @brief The block-linear frame put_frame converts into, kept between frames; its padding stays zero while the frame size does. */
    uint8_t *scratch;
    size_t scratch_cap;
    uint32_t scratch_w, scratch_h;
} video_session;

static void frame_free(frame *f)
{
    free(f->data);
    f->data = NULL;
}

static void video_destroy(void *state)
{
    video_session *v = state;
    if (v->input_event)
        hos_kobj_unref(&v->input_event->obj);
    for (uint32_t i = 0; i < v->npending; i++)
        frame_free(&v->pending[i]);
    for (size_t i = v->head; i < v->ninputs; i++)
        free(v->inputs[i].data);
    free(v->inputs);
    free(v->scratch);
    if (v->dec)
        v->vp9 ? v->vp9->close(v->dec) : v->av1->close(v->dec);
    rl_gsv_counter_free(v->counter);
    pthread_mutex_destroy(&v->mu);
    free(v);
}

static const rl_ipc_class video_class;

static video_session *video_new(void)
{
    video_session *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    pthread_mutex_init(&v->mu, NULL);
    v->counter = rl_gsv_counter_new();
    return v;
}

static rl_result video_push_session(rl_ipc_req *r)
{
    video_session *v = video_new();
    if (!v)
        return KR_OUT_OF_RESOURCE;
    rl_ipc_object *o = rl_srv_object(&video_class, v);
    if (!o) {
        video_destroy(v);
        return KR_OUT_OF_RESOURCE;
    }
    uint32_t h;
    rl_result rc = rl_ipc_session_open(o, &h);
    rl_ipc_object_unref(o);
    if (rc == KR_SUCCESS)
        r->out_move[r->n_out_move++] = h;
    return rc;
}

/** @brief Command 1 (CreateVideoDecoderService): no input; one moved handle, another session of this class. */
static rl_result video_create_service(rl_ipc_req *r) { return video_push_session(r); }

/** @brief Commands that take their input and answer with the result only (initialize, start, finalize). */
static rl_result video_accept(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

/**
 * @brief Command 101 (Configure): a buffer with the codec name ("video/vp9"), then u32 width, height and rate.
 * Opens the stream decoder for the codec.
 */
static rl_result video_configure(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    struct {
        uint32_t width, height, rate;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    const rl_ipc_buf *b = r->na ? &r->a[0] : r->nx ? &r->x[0] : NULL;
    if (!b)
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&v->mu);
    memset(v->codec, 0, sizeof v->codec);
    rl_ipc_buf_read(b, v->codec, b->size < sizeof v->codec - 1 ? b->size : sizeof v->codec - 1);
    v->width = in.width;
    v->height = in.height;
    v->rate = in.rate;
    rl_result rc = KR_SUCCESS;
    bool vp9 = !strcmp(v->codec, "video/vp9"), av1 = !strcmp(v->codec, "video/av1");
    if (vp9 || av1) {
        if (!v->dec) {
            if (vp9) {
                v->vp9 = rl_vp9_stream_backend_current();
                v->dec = v->vp9->open();
            } else {
                v->av1 = rl_av1_stream_backend_current();
                v->dec = v->av1->open();
            }
            if (!v->dec) {
                rl_ipc_stop(r, "gsv", "video decoder Configure for %s %ux%u: the build has no %s decoder (needs %s)",
                            v->codec, in.width, in.height, vp9 ? "VP9" : "AV1", vp9 ? "libvpx" : "dav1d");
                rc = KR_NOT_IMPLEMENTED;
            }
        }
    } else {
        rl_ipc_stop(r, "gsv", "video decoder Configure for codec \"%s\" is not implemented (video/vp9 and video/av1)",
                    v->codec);
        rc = KR_NOT_IMPLEMENTED;
    }
    pthread_mutex_unlock(&v->mu);
    return rc;
}

/** @brief Command 3 (GetInputEvent): a copied handle; the decoder always has room for input, so the event is signaled. */
static rl_result video_input_event(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    pthread_mutex_lock(&v->mu);
    if (!v->input_event)
        v->input_event = rl_hos_event_create();
    hos_revent *e = v->input_event;
    pthread_mutex_unlock(&v->mu);
    if (!e)
        return KR_OUT_OF_RESOURCE;
    rl_hos_event_signal(e);
    return rl_ipc_push_copy(r, &e->obj);
}

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

void rl_gsv_yuv_to_rgba(uint8_t y, uint8_t u, uint8_t v, bool bgra, uint8_t out[4])
{
    int yy = 1192 * (y < 16 ? 0 : y - 16), cu = u - 128, cv = v - 128;
    uint8_t r = clamp8((yy + 1836 * cv) >> 10), g = clamp8((yy - 218 * cu - 546 * cv) >> 10),
            b = clamp8((yy + 2163 * cu) >> 10);
    out[0] = bgra ? b : r;
    out[1] = g;
    out[2] = bgra ? r : b;
    out[3] = 255;
}

/**
 * @brief Converts the NV12 frame to 8-bit RGBA, or BGRA when bgra, with the BT.709 limited-range matrix (inferred:
 * the movie's colorimetry is not in the stream the service sees).
 */
static void nv12_to_rgba(const frame *f, uint32_t w, uint32_t h, uint8_t *out, uint64_t pitch, bool bgra)
{
    const uint8_t *luma = f->data, *chroma = f->data + (uint64_t)f->width * f->height;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *c = chroma + (uint64_t)(y / 2) * f->width + (x / 2) * 2;
            rl_gsv_yuv_to_rgba(luma[(uint64_t)y * f->width + x], c[0], c[1], bgra, out + (uint64_t)y * pitch + (uint64_t)x * 4);
        }
}

/** @brief The rows of one band of an nv12_to_rgba_tiled conversion, for one thread. */
typedef struct tiled_band {
    const frame *f;
    uint32_t w, y0, y1;
    uint8_t *tiled;
    uint64_t pitch, gobs_wide, block_gobs;
    const uint32_t *xpart, *ypart;
} tiled_band;

static void *convert_band(void *arg)
{
    const tiled_band *t = arg;
    const frame *f = t->f;
    const uint8_t *luma = f->data, *chroma = f->data + (uint64_t)f->width * f->height;
    for (uint32_t y = t->y0; y < t->y1; y++) {
        uint64_t block_row = y / (8 * t->block_gobs), gob_in_block = (y / 8) % t->block_gobs;
        uint8_t *gob_row = t->tiled + (block_row * t->gobs_wide * t->block_gobs + gob_in_block) * 512 + t->ypart[y % 8];
        const uint8_t *yrow = luma + (uint64_t)y * f->width, *crow = chroma + (uint64_t)(y / 2) * f->width;
        for (uint32_t x = 0; x < t->w; x += 4) {
            uint8_t px[16];
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t xx = x + k;
                if (xx < t->w)
                    rl_gsv_yuv_to_rgba(yrow[xx], crow[(xx / 2) * 2], crow[(xx / 2) * 2 + 1], false, px + 4 * k);
                else
                    memset(px + 4 * k, 0, 4);
            }
            uint64_t bx = (uint64_t)x * 4;
            memcpy(gob_row + (bx / 64) * t->block_gobs * 512 + t->xpart[(bx % 64) / 16], px,
                   bx + 16 <= t->pitch ? 16 : t->pitch - bx);
        }
    }
    return NULL;
}

/** @brief Most threads one frame's conversion is split across. */
#define CONVERT_THREADS 8u

/**
 * @brief nv12_to_rgba for an image the GPU reads, written straight into its block-linear layout (blocks of block_gobs
 * GOBs, gobs_wide GOBs across): each row converts four pixels at a time, the 16 bytes the layout keeps together, with
 * the byte offset inside a GOB taken as a column part plus a row part (both GOB layouts are such a sum), and bands of
 * rows convert on several threads at once. Bytes the conversion does not reach (the image's padding) are left as they
 * are.
 */
static void nv12_to_rgba_tiled(const frame *f, uint32_t w, uint32_t h, uint8_t *tiled, uint64_t pitch,
                               uint64_t gobs_wide, uint64_t block_gobs)
{
    uint32_t (*gob_offset)(uint32_t, uint32_t) = nvn_gob_offset_fn();
    uint32_t xpart[4], ypart[8];
    for (uint32_t i = 0; i < 4; i++)
        xpart[i] = gob_offset(16 * i, 0);
    for (uint32_t i = 0; i < 8; i++)
        ypart[i] = gob_offset(0, i);
    uint32_t cpus = rl_os_cpu_count(), n = cpus / 2 < CONVERT_THREADS ? cpus / 2 : CONVERT_THREADS;
    if (!n)
        n = 1;
    if (n > h / 8)
        n = h / 8 ? h / 8 : 1;
    tiled_band bands[CONVERT_THREADS];
    pthread_t threads[CONVERT_THREADS];
    bool started[CONVERT_THREADS] = { false };
    for (uint32_t i = 0; i < n; i++) {
        bands[i] = (tiled_band){ f, w, h * i / n, h * (i + 1) / n, tiled, pitch, gobs_wide, block_gobs, xpart, ypart };
        started[i] = i > 0 && pthread_create(&threads[i], NULL, convert_band, &bands[i]) == 0;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!started[i])
            convert_band(&bands[i]);
    for (uint32_t i = 1; i < n; i++)
        if (started[i])
            pthread_join(threads[i], NULL);
}

/** @brief Block height, in GOBs (log2), of a decoder image the GPU reads (inferred: Eglantine writes 16-GOB blocks). */
#define GPU_READ_BLOCK_LOG2 4u

static bool put_frame(video_session *v, uint32_t image_id, const frame *f)
{
    pthread_mutex_lock(&g_mu);
    image m = image_id < IMAGES_MAX ? g_images[image_id] : (image){ 0 };
    memory mem = m.used && m.memory < MEMORIES_MAX ? g_memories[m.memory] : (memory){ 0 };
    pthread_mutex_unlock(&g_mu);
    if (!m.used || !mem.addr || !f->data)
        return false;
    layout l = image_layout(m.pref);
    uint32_t w = f->width < l.width ? f->width : l.width, h = f->height < l.height ? f->height : l.height;
    uint64_t base = mem.addr + m.offset;
    if (l.nv12 && !m.gpu_read) {
        for (uint32_t y = 0; y < h; y++)
            rl_vm_kernel_write(&hos.vm, base + y * l.pitch, f->data + (uint64_t)y * f->width, w);
        const uint8_t *chroma = f->data + (uint64_t)f->width * f->height;
        for (uint32_t y = 0; y < (h + 1) / 2; y++)
            rl_vm_kernel_write(&hos.vm, base + l.chroma_offset + y * l.pitch, chroma + (uint64_t)y * f->width, w);
        return true;
    }
    uint64_t rows = l.chroma_offset / l.pitch;
    uint64_t gobs_wide = l.pitch / 64, gob_rows = (rows + 7) / 8, block_gobs = 1u << GPU_READ_BLOCK_LOG2;
    uint64_t bytes = (gob_rows + block_gobs - 1) / block_gobs * block_gobs * gobs_wide * 512;
    uint8_t *linear = m.gpu_read ? NULL : calloc(1, l.pitch * rows), *tiled = NULL;
    if (m.gpu_read) {
        if (bytes > v->scratch_cap || w != v->scratch_w || h != v->scratch_h) {
            free(v->scratch);
            v->scratch = calloc(1, bytes);
            v->scratch_cap = v->scratch ? bytes : 0;
            v->scratch_w = w;
            v->scratch_h = h;
        }
        tiled = v->scratch;
    }
    if ((!m.gpu_read && !linear) || (m.gpu_read && !tiled)) {
        free(linear);
        return false;
    }
    /* An image the GPU reads is sampled through the movie player's view of the native image, an RGBA8 texture (NVN format
     * 0x38), so its bytes are R, G, B, A whatever the pool's layout code is. */
    if (m.gpu_read) {
        nv12_to_rgba_tiled(f, w, h, tiled, l.pitch, gobs_wide, block_gobs);
        rl_vm_kernel_write(&hos.vm, base, tiled, bytes);
    } else {
        nv12_to_rgba(f, w, h, linear, l.pitch, l.code == 2 || l.code == 5);
        rl_vm_kernel_write(&hos.vm, base, linear, l.pitch * rows);
    }
    free(linear);
    return true;
}

/**
 * @brief UserSession 428 (nn::gpu::sdk::QueryNativeImage) and 426 (MapNativeImage, with u32 handle, u32, u32 address
 * space and u8 permission) of a decoder output image: the title's movie player blits each image as block-linear
 * RGBA, so the image is marked to be written that way. 148 bytes back: for 426 the image's GPU address and last byte
 * (the GPU address space is the process's: GPU == CPU address), then a 33-word descriptor {1, 0, (width - 1) |
 * (height - 1) << 16, ..., 0x01000001 at word 8, 1 at word 9, the offset in the memory at word 10 (0 once mapped),
 * the size at word 11}. Inferred: Eglantine's reply for the same title; the console's descriptor was not captured.
 */
rl_result rl_gsv_native_image_query(rl_ipc_req *r)
{
    uint32_t handle;
    if (!rl_srv_in(r, &handle, 4))
        return SF_RESULT_INVALID_HEADER;
    bool map = r->cmd == 426;
    pthread_mutex_lock(&g_mu);
    image *m = handle < IMAGES_MAX && g_images[handle].used ? &g_images[handle] : NULL;
    memory mem = m && m->memory < MEMORIES_MAX ? g_memories[m->memory] : (memory){ 0 };
    if (m && mem.addr) {
        m->gpu_read = true;
        if (map)
            m->gpu_maps++;
    }
    image copy = m ? *m : (image){ 0 };
    pthread_mutex_unlock(&g_mu);
    if (!m || !mem.addr) {
        rl_ipc_stop(r, "gsv", "UserSession %u of native image %u: only decoder images bound to memory are known",
                    r->cmd, handle);
        return KR_NOT_IMPLEMENTED;
    }
    layout l = image_layout(copy.pref);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "gsv", "native image %u (%s): memory 0x%llx offset 0x%llx size 0x%llx", handle,
               map ? "map" : "query", (unsigned long long)mem.addr, (unsigned long long)copy.offset,
               (unsigned long long)l.size);
    uint32_t *out = rl_ipc_out(r, 148);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    uint64_t address = mem.addr + copy.offset, last = address + l.size - 1;
    if (map) {
        out[0] = (uint32_t)address;
        out[1] = (uint32_t)(address >> 32);
        out[2] = (uint32_t)last;
        out[3] = (uint32_t)(last >> 32);
    }
    uint32_t *d = out + 4;
    d[0] = 1;
    d[2] = copy.pref[2];
    d[8] = 0x01000001;
    d[9] = 1;
    d[10] = map ? 0 : (uint32_t)copy.offset;
    d[11] = (uint32_t)l.size;
    return KR_SUCCESS;
}

bool rl_gsv_native_image_region(uint32_t handle, uint64_t *addr, uint64_t *size)
{
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    image *m = handle < IMAGES_MAX && g_images[handle].used ? &g_images[handle] : NULL;
    if (m && m->memory < MEMORIES_MAX && g_memories[m->memory].addr) {
        m->gpu_read = true;
        *addr = g_memories[m->memory].addr + m->offset;
        *size = image_layout(m->pref).size;
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

/** @brief UserSession 912 (nn::codec FinalizeDecoderImagePool, once per image): a u32 image id; the image is freed, and nothing is written to it again. */
rl_result rl_gsv_decoder_image_free(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, 4))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    if (id < IMAGES_MAX)
        memset(&g_images[id], 0, sizeof g_images[id]);
    pthread_mutex_unlock(&g_mu);
    return KR_SUCCESS;
}

/** @brief UserSession 427 (UnmapNativeImage): a u32 handle, a u32 address space and the mapped range; the map count falls. */
rl_result rl_gsv_native_image_unmap(rl_ipc_req *r)
{
    uint32_t handle;
    if (!rl_srv_in(r, &handle, 4))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    if (handle < IMAGES_MAX && g_images[handle].gpu_maps)
        g_images[handle].gpu_maps--;
    pthread_mutex_unlock(&g_mu);
    return KR_SUCCESS;
}

/** @brief Moves decoded frames into the registered images in order and signals each image's counter value. mu held. */
static void fill_images(video_session *v)
{
    while (v->npending && v->nfree) {
        frame f = v->pending[0];
        memmove(&v->pending[0], &v->pending[1], (v->npending - 1) * sizeof v->pending[0]);
        v->npending--;
        handed h = v->free[0];
        memmove(&v->free[0], &v->free[1], (v->nfree - 1) * sizeof v->free[0]);
        v->nfree--;
        int64_t t0 = vi_trace_now();
        if (!f.end)
            put_frame(v, h.image, &f);
        if (t0)
            vi_trace('W', (uint64_t)(vi_trace_now() - t0), h.image, f.pts);
        frame_free(&f);
        if (v->ndone < RING)
            v->done[v->ndone++] = (finished){ h.image, f.width, f.height, f.pts, f.end };
        rl_gsv_counter_signal(v->counter, h.value);
    }
}

static void queue_frame(video_session *v, const rl_vp9_image *im, uint64_t pts, bool end)
{
    if (v->npending >= RING + (end ? 1u : 0u))
        return;
    frame *f = &v->pending[v->npending];
    memset(f, 0, sizeof *f);
    f->pts = pts;
    f->end = end;
    if (im) {
        f->width = im->width;
        f->height = im->height;
        size_t luma = (size_t)im->width * im->height, cw = (im->width + 1) / 2, ch = (im->height + 1) / 2;
        f->data = malloc(luma + cw * ch * 2);
        if (!f->data)
            return;
        for (uint32_t y = 0; y < im->height; y++)
            memcpy(f->data + (size_t)y * im->width, im->y + (size_t)y * im->y_stride, im->width);
        uint8_t *uv = f->data + luma;
        for (size_t y = 0; y < ch; y++)
            for (size_t x = 0; x < cw; x++) {
                uv[(y * cw + x) * 2] = im->u[y * im->uv_stride + x];
                uv[(y * cw + x) * 2 + 1] = im->v[y * im->uv_stride + x];
            }
    }
    v->npending++;
}

/**
 * @brief Decodes submitted frames while the decoded ring has room and moves decoded frames into free images. False
 * when a frame does not decode, with its size in bad. mu held.
 */
static bool pump(video_session *v, size_t *bad)
{
    for (;;) {
        fill_images(v);
        if (v->head == v->ninputs || v->npending >= RING) {
            if (v->head == v->ninputs)
                v->head = v->ninputs = 0;
            return true;
        }
        input in = v->inputs[v->head++];
        int shown = 0;
        rl_vp9_image im;
        int64_t t0 = vi_trace_now();
        if (in.size)
            shown = v->vp9 ? v->vp9->decode(v->dec, in.data, in.size, &im)
                           : v->av1->decode(v->dec, in.data, in.size, &im);
        if (t0)
            vi_trace('D', (uint64_t)(vi_trace_now() - t0), in.size, (uint64_t)shown);
        free(in.data);
        if (shown < 0) {
            *bad = in.size;
            return false;
        }
        if (shown)
            queue_frame(v, &im, in.pts, false);
        if (in.end)
            queue_frame(v, NULL, in.pts, true);
    }
}

/**
 * @brief Commands 109 and 400 (submit input): the compressed frame in the first send buffer (400 adds a second
 * buffer with a frame information block, which is not needed), a u64 presentation time in microseconds and a u8
 * end-of-stream flag; a u64 submission id. The frame is kept and decoded once the decoded ring has room (pump).
 */
static rl_result video_submit(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    struct __attribute__((packed)) {
        uint64_t pts;
        uint8_t end;
    } in;
    if (!rl_srv_in(r, &in, sizeof in) || !r->na)
        return SF_RESULT_INVALID_HEADER;
    uint64_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&v->mu);
    rl_result rc = KR_SUCCESS;
    if (!v->dec) {
        rc = SF_RESULT_INVALID_HEADER;
    } else {
        uint8_t *data = malloc(r->a[0].size ? r->a[0].size : 1);
        if (v->head && v->ninputs == v->inputs_cap) {
            memmove(v->inputs, v->inputs + v->head, (v->ninputs - v->head) * sizeof *v->inputs);
            v->ninputs -= v->head;
            v->head = 0;
        }
        if (data && v->ninputs == v->inputs_cap) {
            size_t cap = v->inputs_cap ? v->inputs_cap * 2 : 64;
            input *grown = realloc(v->inputs, cap * sizeof *grown);
            if (grown) {
                v->inputs = grown;
                v->inputs_cap = cap;
            }
        }
        size_t bad = 0;
        if (!data || v->ninputs == v->inputs_cap) {
            free(data);
            rc = KR_OUT_OF_RESOURCE;
        } else {
            rl_ipc_buf_read(&r->a[0], data, r->a[0].size);
            v->inputs[v->ninputs++] = (input){ data, r->a[0].size, in.pts, in.end != 0 };
            if (!pump(v, &bad)) {
                rl_ipc_stop(r, "gsv", "video decoder: a %llu byte frame did not decode", (unsigned long long)bad);
                rc = KR_NOT_IMPLEMENTED;
            } else {
                *out = ++v->submitted;
            }
        }
    }
    hos_revent *e = v->input_event;
    pthread_mutex_unlock(&v->mu);
    if (e)
        rl_hos_event_signal(e);
    return rc;
}

/** @brief Hands an output image to the stream: queues it for the next frame, writes its counter value and decodes what now fits; false as pump. mu held. */
static bool hand_out(video_session *v, uint32_t image, uint8_t *out16, size_t *bad)
{
    uint64_t value = ++v->handed_total;
    if (v->nfree < RING)
        v->free[v->nfree++] = (handed){ image, value };
    uint32_t counter = ~v->counter;
    memset(out16, 0, 16);
    memcpy(out16, &counter, 4);
    memcpy(out16 + 8, &value, 8);
    return pump(v, bad);
}

/** @brief Command 311 (register an output image): a u32 image id; its NativeSyncHandle, signaled once a frame is in the image. */
static rl_result video_register(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    uint32_t image;
    if (!rl_srv_in(r, &image, 4))
        return SF_RESULT_INVALID_HEADER;
    uint8_t *out = rl_ipc_out(r, 16);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    size_t bad = 0;
    pthread_mutex_lock(&v->mu);
    bool ok = hand_out(v, image, out, &bad);
    pthread_mutex_unlock(&v->mu);
    if (!ok) {
        rl_ipc_stop(r, "gsv", "video decoder: a %llu byte frame did not decode", (unsigned long long)bad);
        return KR_NOT_IMPLEMENTED;
    }
    return KR_SUCCESS;
}

/**
 * @brief Command 314 (give an output image back): a u32 image id, a u32, and the NativeSyncHandle of the client's
 * reads; the image's next NativeSyncHandle. relift does not wait for the reads before it rewrites the image.
 */
static rl_result video_release(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    struct {
        uint32_t image, unused;
        uint8_t handle[16];
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    uint8_t *out = rl_ipc_out(r, 16);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    size_t bad = 0;
    pthread_mutex_lock(&v->mu);
    bool ok = hand_out(v, in.image, out, &bad);
    pthread_mutex_unlock(&v->mu);
    if (!ok) {
        rl_ipc_stop(r, "gsv", "video decoder: a %llu byte frame did not decode", (unsigned long long)bad);
        return KR_NOT_IMPLEMENTED;
    }
    return KR_SUCCESS;
}

/** @brief Command 212 (unregister an output image): a u32 image id; the image leaves the free list. */
static rl_result video_unregister(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    uint32_t image;
    if (!rl_srv_in(r, &image, 4))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&v->mu);
    for (uint32_t i = 0; i < v->nfree; i++)
        if (v->free[i].image == image) {
            memmove(&v->free[i], &v->free[i + 1], (v->nfree - i - 1) * sizeof v->free[0]);
            v->nfree--;
            break;
        }
    pthread_mutex_unlock(&v->mu);
    return KR_SUCCESS;
}

#define RESULT_NO_FRAME RL_MAKE_RESULT(111, 3604)

/**
 * @brief Command 213 (get the next finished frame): 64 bytes: u32 image id, width, height, width, height, a u32
 * zero, a u64 presentation time in nanoseconds and a u32 end-of-stream flag. With nothing finished, the result the
 * client's own acquire uses for an empty ring (inferred: Eglantine).
 */
static rl_result video_get_frame(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    pthread_mutex_lock(&v->mu);
    if (!v->ndone) {
        pthread_mutex_unlock(&v->mu);
        return RESULT_NO_FRAME;
    }
    finished f = v->done[0];
    memmove(&v->done[0], &v->done[1], (v->ndone - 1) * sizeof v->done[0]);
    v->ndone--;
    pthread_mutex_unlock(&v->mu);
    uint8_t *out = rl_ipc_out(r, 64);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    uint32_t w[6] = { f.image, f.width, f.height, f.width, f.height, 0 };
    uint64_t ns = f.pts * 1000;
    uint32_t end = f.end ? 1 : 0;
    memcpy(out, w, 24);
    memcpy(out + 24, &ns, 8);
    memcpy(out + 32, &end, 4);
    return KR_SUCCESS;
}

/**
 * @brief Commands 8 and 207 (flush, stop): the queued frames, decoded or not, are dropped; images already handed out
 * keep their place.
 */
static rl_result video_flush(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    pthread_mutex_lock(&v->mu);
    for (uint32_t i = 0; i < v->npending; i++)
        frame_free(&v->pending[i]);
    v->npending = 0;
    for (size_t i = v->head; i < v->ninputs; i++)
        free(v->inputs[i].data);
    v->head = v->ninputs = 0;
    pthread_mutex_unlock(&v->mu);
    return KR_SUCCESS;
}

/** @brief Command 115 (finalize): every counter value handed out is reached, so the client's waits end. */
static rl_result video_finalize(rl_ipc_req *r)
{
    video_session *v = r->self->state;
    pthread_mutex_lock(&v->mu);
    rl_gsv_counter_signal(v->counter, v->handed_total);
    pthread_mutex_unlock(&v->mu);
    return KR_SUCCESS;
}

static const rl_ipc_cmd video_cmds[] = {
    { 1, "CreateVideoDecoderService", video_create_service },
    { 3, "GetInputEvent", video_input_event },
    { 6, "Start", video_accept },
    { 8, "Flush", video_flush },
    { 101, "Configure", video_configure },
    { 109, "Decode", video_submit },
    { 115, "Finalize", video_finalize },
    { 200, "InitializeWithTransferMemory", video_accept },
    { 201, "InitializeWithTransferMemoryAndFlag", video_accept },
    { 202, "InitializeWithTransferMemoryFlagAndValue", video_accept },
    { 207, "Stop", video_flush },
    { 210, "InitializeWithRemoteMemory", video_accept },
    { 212, "UnbindFrame", video_unregister },
    { 213, "GetFrame", video_get_frame },
    { 311, "BindFrame", video_register },
    { 314, "ReleaseFrame", video_release },
    { 400, "DecodeWithInformation", video_submit },
};

static const rl_ipc_class video_class = { "nn::gsv::VideoSession", video_cmds,
                                          sizeof video_cmds / sizeof video_cmds[0], 0, video_destroy };

rl_result rl_gsv_open_video_session(rl_ipc_req *r) { return video_push_session(r); }
