/**
 * @file test_nvn_trace.c
 * @brief The NVN frame trace (RELIFT_NVN_TRACE): its setting, and one traced command that leaves its line and the
 * bytes of the buffer it changed, before and after, in the folder.
 */
#include "nvn/nvn_vk.h"
#include "tc.h"

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TEST(nvn_trace_setting_names_a_folder_a_present_and_a_frame_count)
{
    char dir[64];
    uint64_t from = 0, frames = 0;
    CHECK(nvn_trace_parse("out:120", dir, sizeof dir, &from, &frames));
    CHECK(!strcmp(dir, "out"));
    CHECK_EQ(from, 120);
    CHECK_EQ(frames, 1);
    CHECK(nvn_trace_parse("/a/b:7:3", dir, sizeof dir, &from, &frames));
    CHECK(!strcmp(dir, "/a/b"));
    CHECK_EQ(from, 7);
    CHECK_EQ(frames, 3);
    CHECK(!nvn_trace_parse("out", dir, sizeof dir, &from, &frames));
    CHECK(!nvn_trace_parse("out:x", dir, sizeof dir, &from, &frames));
    CHECK(!nvn_trace_parse(":5", dir, sizeof dir, &from, &frames));
    CHECK(!nvn_trace_parse("out:5:0", dir, sizeof dir, &from, &frames));
    CHECK(!nvn_trace_parse(NULL, dir, sizeof dir, &from, &frames));
}

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(1 << 20);
    *n = buf ? fread(buf, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (buf)
        buf[*n] = 0;
    return buf;
}

static const char *field(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= cap)
        return NULL;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return out;
}

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

TEST(nvn_trace_writes_the_buffers_a_command_changed_before_and_after)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000), buf = heap + 0x1000;
    char dir[PATH_MAX];
    const char *base = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/relift-trace-XXXXXX", base && *base ? base : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    char spec[PATH_MAX + 16];
    snprintf(spec, sizeof spec, "%s:0", dir);
    setenv("RELIFT_NVN_TRACE", spec, 1);
    nvn_trace_present(NULL);
    CHECK(nvn_trace_active);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    st.code[NVN_STAGE_COMPUTE] = 0x1000;
    st.storage[2].addr = buf;
    st.storage[2].size = 32;
    replay r = { .c = c, .mem = t_mem(), .st = &st };
    memset(t_mem() + buf, 0x11, 32);
    nvn_trace_begin(&r, "dispatch");
    memset(t_mem() + buf, 0x22, 32);
    nvn_trace_cmd tc = { .groups = { 4, 2, 1 } };
    tc.keys[NVN_STAGE_COMPUTE] = "feedc0de";
    nvn_trace_end(&r, &tc);
    nvn_trace_present(NULL);
    CHECK(!nvn_trace_active);
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/frame-0/trace.jsonl", dir);
    size_t n = 0;
    char *line = slurp(path, &n);
    CHECK(line != NULL);
    if (!line)
        return;
    CHECK(strstr(line, "\"kind\":\"dispatch\"") != NULL);
    CHECK(strstr(line, "\"groups\":[4,2,1]") != NULL);
    CHECK(strstr(line, "\"compute\":{\"key\":\"feedc0de\"") != NULL);
    CHECK(strstr(line, "\"stage\":\"compute\",\"kind\":\"storage\",\"slot\":2") != NULL);
    char pre[64], post[64];
    CHECK(field(line, "pre", pre, sizeof pre) != NULL);
    CHECK(field(line, "post", post, sizeof post) != NULL);
    CHECK(strcmp(pre, post) != 0);
    uint8_t bytes[32];
    snprintf(path, sizeof path, "%s/frame-0/%s", dir, pre);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK_EQ(fread(bytes, 1, sizeof bytes, f), sizeof bytes);
        CHECK_EQ(bytes[0], 0x11);
        CHECK_EQ(bytes[31], 0x11);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/frame-0/%s", dir, post);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK_EQ(fread(bytes, 1, sizeof bytes, f), sizeof bytes);
        CHECK_EQ(bytes[0], 0x22);
        CHECK_EQ(bytes[31], 0x22);
        fclose(f);
    }
    free(line);
    CHECK_EQ(nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS), 0);
    unsetenv("RELIFT_NVN_TRACE");
}
