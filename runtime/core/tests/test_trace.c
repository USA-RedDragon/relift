/**
 * @file test_trace.c
 * @brief The frame trace: stats.trace with a kinds filter writes only the
 * kinds it lists, each line naming the guest thread the hook gives.
 */
#include "runner.h"

#include <relift/settings.h>
#include <relift/trace.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint64_t thread_seven(void) { return 7; }

TEST(trace_kinds_filter_lines_and_name_the_guest_thread)
{
    CHECK(rl_trace_kind_listed("", 'V'));
    CHECK(rl_trace_kind_listed("VP", 'P'));
    CHECK(!rl_trace_kind_listed("VP", 'A'));
    CHECK(!rl_trace_kind_listed("VP", 0));

    static char path[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/rl_trace_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    char spec[600];
    snprintf(spec, sizeof spec, "%s|VA", path);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("stats.trace", spec), RL_SETTING_OK);
    rl_trace_set_thread_id(thread_seven);
    CHECK(rl_trace_on());
    CHECK(rl_trace_now() > 0);
    rl_trace('A', 0x10, 0x20, 0x30);
    rl_trace('P', 1, 2, 3);
    rl_trace('V', 0xab, 0, 0);

    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    char kind[2][2];
    long long t[2];
    unsigned long long id[2], a[2], b[2], c[2];
    for (int i = 0; i < 2; i++)
        CHECK_EQ(fscanf(f, "%1s %lld %llu %llx %llx %llx", kind[i], &t[i], &id[i], &a[i], &b[i], &c[i]), 6);
    char more[8];
    CHECK_EQ(fscanf(f, "%7s", more), EOF);
    fclose(f);
    unlink(path);
    CHECK(kind[0][0] == 'A' && a[0] == 0x10 && b[0] == 0x20 && c[0] == 0x30);
    CHECK(kind[1][0] == 'V' && a[1] == 0xab);
    CHECK(id[0] == 7 && id[1] == 7);
    CHECK(t[1] >= t[0]);
}
