/**
 * @file nvn_window.c
 * @brief NVN2 windows: the game's set of textures it renders into and
 * presents, handed out in turn by AcquireTexture.
 */
#include "nvn_priv.h"
#include "nvn_async.h"

#include <stdlib.h>
#include <string.h>

/** @brief Most textures an NVN window holds. */
#define WINDOW_TEXTURES 4u

typedef struct nvn_window {
    nvn_obj o;
    uint64_t textures[WINDOW_TEXTURES];
    uint32_t n, active, next, interval;
    int32_t crop[4];
    uint64_t presents;
    /** @brief The builder's native window (nvnWindowBuilderSetNativeWindow). */
    uint64_t native;
} nvn_window;

/**
 * @brief nvnWindowInitialize(NVNwindow *, const NVNwindowBuilder *): NVNboolean. A driver without
 * nvnWindowBuilderGetNumActiveTextures (older NVN1 SDKs) has every texture active.
 */
uint64_t nvnh_nvnWindowInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t win = c->x[0], builder = c->x[1], n = 0, active = 0, interval = 0, native = 0;
    int rc = nvn_driver_call(c, mem, "nvnWindowBuilderGetNumTextures", &builder, 1, &n);
    uint64_t active_fn = 0;
    if (rc == RL_OK)
        rc = nvn_driver_lookup(c, mem, "nvnWindowBuilderGetNumActiveTextures", &active_fn);
    if (rc == RL_OK && active_fn)
        rc = nvn_guest_call(c, mem, active_fn, &builder, 1, &active);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnWindowBuilderGetPresentInterval", &builder, 1, &interval);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnWindowBuilderGetNativeWindow", &builder, 1, &native);
    if (rc != RL_OK)
        return (uint64_t)rc;
    if (n == 0 || n > WINDOW_TEXTURES)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnWindowInitialize with %llu textures", (unsigned long long)n);
    uint64_t textures[WINDOW_TEXTURES];
    for (uint64_t i = 0; i < n; i++) {
        uint64_t args[2] = { builder, i };
        rc = nvn_driver_call(c, mem, "nvnWindowBuilderGetTexture", args, 2, &textures[i]);
        if (rc != RL_OK)
            return (uint64_t)rc;
    }
    nvn_window *w = (nvn_window *)nvn_obj_new(win, NVN_WINDOW, sizeof *w);
    if (w) {
        memcpy(w->textures, textures, sizeof textures);
        w->n = (uint32_t)n;
        w->active = active && active <= n ? (uint32_t)active : (uint32_t)n;
        w->interval = (uint32_t)interval;
        w->native = native;
    }
    w = (nvn_window *)nvn_obj_publish((nvn_obj *)w);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "nvnWindowInitialize(0x%llx): %llu textures, %llu active, interval %llu",
               (unsigned long long)win, (unsigned long long)n, (unsigned long long)active,
               (unsigned long long)interval);
    c->x[0] = w != NULL;
    return nvn_ret(c);
}

/** @brief nvnWindowFinalize(NVNwindow *). */
uint64_t nvnh_nvnWindowFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_WINDOW));
    return nvn_ret(c);
}

/** @brief The acquire's wait for a free texture: on the game's thread, or queued behind the earlier items of the replay worker, where it holds the later sets back as the sync's queue wait does on the console. */
static void acquire_wait_run(void *arg)
{
    nvn_pace_acquire((uint32_t)(uintptr_t)arg);
}

static void acquire_wait(uint32_t textures)
{
    if (nvn_async_enabled() && nvn_async_post(acquire_wait_run, (void *)(uintptr_t)textures))
        return;
    nvn_async_drain();
    nvn_pace_acquire(textures);
}

static nvn_window *win_of(rl_ctx *c, uint64_t a, const char *proc)
{
    nvn_window *w = (nvn_window *)nvn_obj_get(a, NVN_WINDOW);
    if (!w)
        rl_guest_fatal(c, "nvn", "%s on 0x%llx, not a window", proc, (unsigned long long)a);
    return w;
}

/** @brief NVNwindowAcquireTextureResult values. */
enum { ACQUIRE_SUCCESS = 0, ACQUIRE_NATIVE_ERROR = 1 };

/** @brief nvnWindowAcquireTexture(NVNwindow *, NVNsync *, int *index): NVNwindowAcquireTextureResult. */
uint64_t nvnh_nvnWindowAcquireTexture(rl_ctx *c, uint8_t *mem)
{
    nvn_window *w = win_of(c, c->x[0], "nvnWindowAcquireTexture");
    if (!w)
        return RL_PENDING;
    nvn_evlog("AcquireTexture", c->x[1]);
    acquire_wait(w->active);
    nvn_evlog("AcquireTexture returned", c->x[1]);
    nvn_sync_signal(c->x[1]);
    rl_st32(mem, c->x[2], w->next);
    w->next = (w->next + 1) % w->active;
    c->x[0] = ACQUIRE_SUCCESS;
    return nvn_ret(c);
}

/**
 * @brief nvnQueueAcquireTexture(NVNqueue *, NVNwindow *, int *index):
 * NVNqueueAcquireTextureResult, the NVN1 form of nvnWindowAcquireTexture
 * whose wait for the texture goes into the queue; the texture is free at
 * once here, as for nvnWindowAcquireTexture.
 */
uint64_t nvnh_nvnQueueAcquireTexture(rl_ctx *c, uint8_t *mem)
{
    nvn_window *w = win_of(c, c->x[1], "nvnQueueAcquireTexture");
    if (!w)
        return RL_PENDING;
    acquire_wait(w->active);
    rl_st32(mem, c->x[2], w->next);
    w->next = (w->next + 1) % w->active;
    c->x[0] = ACQUIRE_SUCCESS;
    return nvn_ret(c);
}

/** @brief nvnWindowSetPresentInterval(NVNwindow *, int). */
uint64_t nvnh_nvnWindowSetPresentInterval(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_window *w = win_of(c, c->x[0], "nvnWindowSetPresentInterval");
    if (!w)
        return RL_PENDING;
    if (w->interval != (uint32_t)c->x[1])
        rl_log(RL_LOG_INFO, "nvn", "window 0x%llx present interval %u -> %u at present %llu",
               (unsigned long long)w->o.addr, w->interval, (uint32_t)c->x[1], (unsigned long long)w->presents);
    w->interval = (uint32_t)c->x[1];
    return nvn_ret(c);
}

/** @brief nvnWindowGetPresentInterval(const NVNwindow *): int. */
uint64_t nvnh_nvnWindowGetPresentInterval(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_window *w = win_of(c, c->x[0], "nvnWindowGetPresentInterval");
    if (!w)
        return RL_PENDING;
    c->x[0] = w->interval;
    return nvn_ret(c);
}

/**
 * @brief nvnWindowGetNativeWindow(const NVNwindow *): the native window of the builder the window was initialized
 * from. The driver returns the word its own nvnWindowInitialize copies from the builder (a one-load getter in every
 * NVN1 and NVN2 driver read); the layer's initialization does not write the driver's window object, so it keeps the
 * value in its record of the window.
 */
uint64_t nvnh_nvnWindowGetNativeWindow(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_window *w = win_of(c, c->x[0], "nvnWindowGetNativeWindow");
    if (!w)
        return RL_PENDING;
    c->x[0] = w->native;
    return nvn_ret(c);
}

/** @brief nvnWindowGetNumTextures(const NVNwindow *): int. */
uint64_t nvnh_nvnWindowGetNumTextures(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_window *w = win_of(c, c->x[0], "nvnWindowGetNumTextures");
    if (!w)
        return RL_PENDING;
    c->x[0] = w->n;
    return nvn_ret(c);
}

/** @brief nvnWindowGetNumActiveTextures(const NVNwindow *): int. */
uint64_t nvnh_nvnWindowGetNumActiveTextures(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_window *w = win_of(c, c->x[0], "nvnWindowGetNumActiveTextures");
    if (!w)
        return RL_PENDING;
    c->x[0] = w->active;
    return nvn_ret(c);
}

/** @brief nvnWindowSetNumActiveTextures(NVNwindow *, int). */
uint64_t nvnh_nvnWindowSetNumActiveTextures(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_window *w = win_of(c, c->x[0], "nvnWindowSetNumActiveTextures");
    if (!w)
        return RL_PENDING;
    uint32_t n = (uint32_t)c->x[1];
    w->active = n && n <= w->n ? n : w->n;
    w->next %= w->active;
    return nvn_ret(c);
}

/** @brief nvnWindowSetCrop(NVNwindow *, int x, int y, int w, int h). */
uint64_t nvnh_nvnWindowSetCrop(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_window *w = win_of(c, c->x[0], "nvnWindowSetCrop");
    if (!w)
        return RL_PENDING;
    for (unsigned i = 0; i < 4; i++)
        w->crop[i] = (int32_t)c->x[1 + i];
    return nvn_ret(c);
}

/** @brief nvnWindowGetCrop(const NVNwindow *, NVNrectangle *). */
uint64_t nvnh_nvnWindowGetCrop(rl_ctx *c, uint8_t *mem)
{
    nvn_window *w = win_of(c, c->x[0], "nvnWindowGetCrop");
    if (!w)
        return RL_PENDING;
    memcpy(mem + c->x[1], w->crop, sizeof w->crop);
    return nvn_ret(c);
}

bool nvn_crop_rect(const int32_t crop[4], uint32_t width, uint32_t height, uint32_t out[4])
{
    if (crop[2] == 0 || crop[3] == 0) {
        out[0] = out[1] = 0;
        out[2] = width;
        out[3] = height;
        return width && height;
    }
    int64_t x0 = crop[0] < 0 ? 0 : crop[0], y0 = crop[1] < 0 ? 0 : crop[1];
    int64_t x1 = (int64_t)crop[0] + crop[2], y1 = (int64_t)crop[1] + crop[3];
    x1 = x1 > (int64_t)width ? (int64_t)width : x1;
    y1 = y1 > (int64_t)height ? (int64_t)height : y1;
    if (x1 <= x0 || y1 <= y0)
        return false;
    out[0] = (uint32_t)x0;
    out[1] = (uint32_t)y0;
    out[2] = (uint32_t)(x1 - x0);
    out[3] = (uint32_t)(y1 - y0);
    return true;
}

/**
 * @brief nvnQueuePresentTexture(NVNqueue *, NVNwindow *, int index): shows
 * the window's crop rectangle of the texture, as the console's display does
 * (a game may render a 1920x1080 picture into larger window textures).
 */
/** @brief A present queued for the replay worker, behind the sets submitted before it. */
typedef struct async_present {
    rl_ctx *c;
    uint8_t *mem;
    uint64_t queue, texture;
    int32_t crop[4];
    uint32_t interval;
} async_present;

static void async_present_run(void *arg)
{
    async_present *a = arg;
    int rc = nvn_present(a->c, a->mem, a->queue, a->texture, a->crop, a->interval);
    if (rc != RL_OK)
        rl_log(RL_LOG_ERROR, "nvn", "a present run on the worker returned %d", rc);
    free(a->c);
    free(a);
}

static bool present_posted(rl_ctx *c, uint8_t *mem, uint64_t queue, uint64_t texture, const nvn_window *w)
{
    if (!nvn_async_enabled())
        return false;
    async_present *a = malloc(sizeof *a);
    if (!a)
        return false;
    a->c = nvn_async_snapshot(c);
    a->mem = mem;
    a->queue = queue;
    a->texture = texture;
    memcpy(a->crop, w->crop, sizeof a->crop);
    a->interval = w->interval;
    if (a->c && nvn_async_post(async_present_run, a))
        return true;
    free(a->c);
    free(a);
    return false;
}

uint64_t nvnh_nvnQueuePresentTexture(rl_ctx *c, uint8_t *mem)
{
    nvn_window *w = win_of(c, c->x[1], "nvnQueuePresentTexture");
    if (!w)
        return RL_PENDING;
    uint32_t idx = (uint32_t)c->x[2];
    if (idx >= w->n)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnQueuePresentTexture of texture %u of %u", idx, w->n);
    nvn_evlog("PresentTexture", c->x[2]);
    int rc = present_posted(c, mem, c->x[0], w->textures[idx], w) ? RL_OK : (nvn_async_drain(), nvn_present(c, mem, c->x[0], w->textures[idx], w->crop, w->interval));
    if (rc != RL_OK)
        return (uint64_t)rc;
    if (w->presents++ == 0)
        rl_log(RL_LOG_INFO, "nvn", "first present: window 0x%llx texture %u (0x%llx), %u textures, %u active, interval %u",
               (unsigned long long)w->o.addr, idx, (unsigned long long)w->textures[idx], w->n, w->active, w->interval);
    return nvn_ret(c);
}
