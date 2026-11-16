/**
 * @file video.c
 * @brief Selection of the VP9 decoder backend.
 */
#include <relift/video.h>

#include <stddef.h>

static void *stub_open(void) { return NULL; }

static void stub_close(void *d) { (void)d; }

static bool stub_decode(void *d, const rl_vp9_frame *f)
{
    (void)d;
    (void)f;
    return false;
}

static const rl_vp9_backend g_stub_vp9 = {
    .name = "none",
    .open = stub_open,
    .close = stub_close,
    .decode = stub_decode,
};

const rl_vp9_backend *rl_vp9_backend_current(void)
{
#ifdef RL_HOST_VP9
    return rl_host_vp9_backend();
#else
    return &g_stub_vp9;
#endif
}
