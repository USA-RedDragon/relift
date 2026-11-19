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

static int stub_stream_decode(void *d, const uint8_t *data, size_t size, rl_vp9_image *out)
{
    (void)d;
    (void)data;
    (void)size;
    (void)out;
    return -1;
}

static const rl_vp9_stream_backend g_stub_stream = {
    .name = "none",
    .open = stub_open,
    .close = stub_close,
    .decode = stub_stream_decode,
};

const rl_vp9_stream_backend *rl_vp9_stream_backend_current(void)
{
#ifdef RL_HOST_VP9
    return rl_host_vp9_stream_backend();
#else
    return &g_stub_stream;
#endif
}

static int stub_av1_decode(void *d, const uint8_t *data, size_t size, rl_video_image *out)
{
    (void)d;
    (void)data;
    (void)size;
    (void)out;
    return -1;
}

static const rl_av1_stream_backend g_stub_av1 = {
    .name = "none",
    .open = stub_open,
    .close = stub_close,
    .decode = stub_av1_decode,
};

const rl_av1_stream_backend *rl_av1_stream_backend_current(void)
{
#ifdef RL_HOST_AV1
    return rl_host_av1_stream_backend();
#else
    return &g_stub_av1;
#endif
}
