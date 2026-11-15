/**
 * @file test_datacheck.c
 * @brief The game data check: SHA-256 vectors, a matching folder, a missing, a short and a changed file.
 */
#include "runner.h"

#include <relift/datacheck.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

TEST(sha256_matches_the_fips_vectors)
{
    char h[65];
    rl_data_sha256("", 0, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    rl_data_sha256("abc", 3, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    rl_data_sha256(two, strlen(two), h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

TEST(data_check_names_the_first_bad_file)
{
    char root[256];
    snprintf(root, sizeof root, "%s/relift-datacheck-%d", getenv("TMPDIR") ? getenv("TMPDIR") : ".", (int)getpid());
    char romfs[300], p[400];
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    mkdir(root, 0700);
    mkdir(romfs, 0700);
    snprintf(p, sizeof p, "%s/a", romfs);
    put(p, "abc");
    const char *m = "# comment\n"
                    "3 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad romfs/a\n";
    rl_data_report r;
    CHECK_EQ(rl_data_check(m, strlen(m), romfs, NULL, true, &r), 0);
    CHECK_EQ(r.files, 1);
    CHECK_EQ(r.bytes, 3);
    put(p, "abd");
    CHECK_EQ(rl_data_check(m, strlen(m), romfs, NULL, false, &r), 0);
    CHECK_EQ(rl_data_check(m, strlen(m), romfs, NULL, true, &r), 1);
    CHECK(!strcmp(r.why, "contents differ"));
    put(p, "ab");
    CHECK_EQ(rl_data_check(m, strlen(m), romfs, NULL, false, &r), 1);
    CHECK(!strcmp(r.first, "romfs/a"));
    CHECK(!strcmp(r.why, "is 2 bytes, expected 3"));
    unlink(p);
    const char *two = "3 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad romfs/a\n"
                      "1 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad dlc/x\n";
    CHECK_EQ(rl_data_check(two, strlen(two), romfs, NULL, false, &r), 1);
    CHECK_EQ(r.bad, 2);
    CHECK(!strcmp(r.first, "romfs/a"));
    CHECK(!strcmp(r.why, "missing"));
    CHECK_EQ(rl_data_check("3 short romfs/a\n", 16, romfs, NULL, false, &r), -1);
    rmdir(romfs);
    rmdir(root);
}
