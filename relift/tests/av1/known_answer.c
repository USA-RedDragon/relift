/*
 * Decodes the temporal units of an AV1 stream through the runtime's AV1 backend and prints the FNV-1a 64 hash of
 * each shown frame (luma, then both chroma planes, tightly packed).
 *
 * known_answer <stream> <unit start offset>...
 */
#include <relift/video.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size)
        return 2;
    fclose(f);
    const rl_av1_stream_backend *be = rl_av1_stream_backend_current();
    void *d = be->open();
    if (!d)
        return 3;
    for (int i = 2; i < argc; i++) {
        size_t start = (size_t)atol(argv[i]);
        size_t end = i + 1 < argc ? (size_t)atol(argv[i + 1]) : (size_t)size;
        rl_video_image im;
        int rc = be->decode(d, data + start, end - start, &im);
        if (rc < 0)
            return 4;
        if (rc == 0)
            continue;
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t y = 0; y < im.height; y++)
            h = fnv(h, im.y + (size_t)y * im.y_stride, im.width);
        for (uint32_t y = 0; y < (im.height + 1) / 2; y++)
            h = fnv(h, im.u + (size_t)y * im.uv_stride, (im.width + 1) / 2);
        for (uint32_t y = 0; y < (im.height + 1) / 2; y++)
            h = fnv(h, im.v + (size_t)y * im.uv_stride, (im.width + 1) / 2);
        printf("%" PRIx64 "\n", h);
    }
    be->close(d);
    return 0;
}
