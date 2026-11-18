/**
 * @file test_srv_pl.c
 * @brief pl:u the way nn::pl reaches it: each font's size and offset, the
 * read-only shared memory with the font at its offset behind the .bfttf
 * header, the priority list, and a named refusal for a font relift does not
 * have.
 */
#include "tc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t u32_of(rl_ctx *c, tc_obj o, uint32_t cmd, uint32_t type, uint32_t *rc)
{
    tc_req q = tc_new(cmd);
    tc_put32(&q, type);
    tc_res r = tc_send(c, o, q);
    *rc = r.svc_rc ? r.svc_rc : r.result;
    return tc_u32(&r, 0);
}

TEST(pl_serves_the_imported_fonts_in_shared_memory)
{
    char dir[4096], path[4200];
    snprintf(dir, sizeof dir, "%s/fonts", getenv("RELIFT_SDMC"));
    mkdir(dir, 0755);
    static const char std_font[] = "\x00\x01\x00\x00 standard font";
    static const char ext_font[] = "\x00\x01\x00\x00 extension";
    snprintf(path, sizeof path, "%s/0.ttf", dir);
    FILE *f = fopen(path, "wb");
    fwrite(std_font, 1, sizeof std_font - 1, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/5.ttf", dir);
    f = fopen(path, "wb");
    fwrite(ext_font, 1, sizeof ext_font - 1, f);
    fclose(f);
    setenv("RELIFT_FONTS", dir, 1);

    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj pl = tc_open_service(c, heap, "pl:u");
    CHECK(pl.handle != 0);
    uint32_t rc;
    CHECK_EQ(u32_of(c, pl, 0, 0, &rc), 0);
    CHECK_EQ(rc, 0);
    CHECK_EQ(u32_of(c, pl, 1, 0, &rc), 1);
    CHECK_EQ(u32_of(c, pl, 2, 0, &rc), sizeof std_font - 1);
    uint32_t off0 = u32_of(c, pl, 3, 0, &rc);
    CHECK_EQ(off0, 8);
    uint32_t off5 = u32_of(c, pl, 3, 5, &rc);
    CHECK_EQ(u32_of(c, pl, 2, 5, &rc), sizeof ext_font - 1);
    CHECK_EQ(off5, 8 + sizeof std_font - 1 + 8);

    tc_res r = tc_send(c, pl, tc_new(4));
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);
    uint64_t at = t_free_map_addr();
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, r.copy[0], at, 0x1100000, HOS_PERM_R, 0, 0), 0);
    const uint8_t *shm = t_mem() + at;
    CHECK(memcmp(shm + off0, std_font, sizeof std_font - 1) == 0);
    CHECK(memcmp(shm + off5, ext_font, sizeof ext_font - 1) == 0);
    uint32_t hdr[2];
    memcpy(hdr, shm, 8);
    CHECK_EQ(hdr[0], 0x18029a7fu);
    CHECK_EQ(__builtin_bswap32(hdr[1] ^ 0x06186249u), sizeof std_font - 1);

    tc_req q = tc_new(5);
    tc_put64(&q, 0);
    q.nb = 3;
    for (uint32_t b = 0; b < 3; b++) {
        q.b_addr[b] = heap + 0x1000 + 0x100 * b;
        q.b_size[b] = 0x40;
    }
    r = tc_send(c, pl, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.data[0], 1);
    CHECK_EQ(tc_u32(&r, 4), 2);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x1000), 0);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x1004), 5);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x1104), off5);

    (void)u32_of(c, pl, 2, 4, &rc);
    CHECK_EQ(rc, SF_RESULT_NOT_SUPPORTED);
    unsetenv("RELIFT_FONTS");
}

static void write_font(const char *dir, uint32_t type, const char *text)
{
    char path[4200];
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%u.ttf", dir, type);
    FILE *f = fopen(path, "wb");
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

TEST(pl_takes_the_fallback_only_for_fonts_the_player_lacks)
{
    char own[4096], fallback[4096];
    snprintf(own, sizeof own, "%s/own-fonts", getenv("RELIFT_SDMC"));
    snprintf(fallback, sizeof fallback, "%s/fallback-fonts", getenv("RELIFT_SDMC"));
    write_font(own, 1, "own Chinese Simplified");
    write_font(fallback, 1, "fallback Chinese Simplified");
    write_font(fallback, 4, "fallback Korean");
    setenv("RELIFT_FONTS", own, 1);
    setenv("RELIFT_FONTS_FALLBACK", fallback, 1);

    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj pl = tc_open_service(c, heap, "pl:u");
    uint32_t rc;
    CHECK_EQ(u32_of(c, pl, 2, 1, &rc), strlen("own Chinese Simplified"));
    CHECK_EQ(rc, 0);
    CHECK_EQ(u32_of(c, pl, 2, 4, &rc), strlen("fallback Korean"));
    CHECK_EQ(rc, 0);
    (void)u32_of(c, pl, 2, 0, &rc);
    CHECK_EQ(rc, SF_RESULT_NOT_SUPPORTED);
    unsetenv("RELIFT_FONTS");
    unsetenv("RELIFT_FONTS_FALLBACK");
}
