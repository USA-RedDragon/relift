/**
 * @file test_runner_xid.c
 * @brief The runner fails a test that faulted the GPU: an NVIDIA Xid in the kernel log laid to the test by its
 * child's pid or its time, with the line decoded.
 */
#include "runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define XID_LINE(t, pid, name)                                                                                        \
    t " archlinux kernel: NVRM: Xid (PCI:0000:01:00): 69, pid=" pid ", name=" name ", Class Error: channel "            \
      "0x0000002e, Class 0000c797, Offset 00000274, Data 00000006, ErrorCode 00000132\n"

TEST(runner_lays_an_xid_to_the_test_by_its_pid_or_its_time)
{
    t_span spans[3] = { { 0, 100, 10.0, 11.0 }, { 1, 200, 11.0, 12.0 }, { 2, 300, 20.0, 21.0 } };
    char why[512];
    const char *next;
    /* By pid, wherever its time. */
    const char *a = "10.5 archlinux kernel: other line\n" XID_LINE("50.0", "200", "elsewhere");
    CHECK_EQ(t_xid_attribute(a, "switch_vk_tests", spans, 3, why, sizeof why, &next), 1);
    CHECK(strstr(why, "Xid 69") && strstr(why, "AMPERE_B") && strstr(why, "DRAW_VERTEX_ARRAY_BEGIN_END_B"));
    CHECK(strstr(why, "error code 0x132") && strstr(why, "data 0x6"));
    /* By time, only from a process of this program's name (a grandchild's pid is not a run's). */
    const char *b = XID_LINE("20.4", "999", "switch_vk_tests");
    CHECK_EQ(t_xid_attribute(b, "switch_vk_tests", spans, 3, why, sizeof why, &next), 2);
    CHECK_EQ(t_xid_attribute(b, "core_tests", spans, 3, why, sizeof why, &next), -1);
    /* Another program's fault outside every run is not a test's. */
    const char *c = XID_LINE("15.0", "999", "vulkan_native");
    CHECK_EQ(t_xid_attribute(c, "vulkan_native", spans, 3, why, sizeof why, &next), -1);
}

/** @brief A test that passes after a short while, for the runner's own run below. */
TEST(runner_xid_probe_passes_slowly)
{
    usleep(300000);
}

TEST(runner_fails_a_passing_test_whose_run_faulted_the_gpu)
{
#if defined(__linux__) && !defined(__ANDROID__)
    if (getenv("RELIFT_TEST_KLOG"))
        return; /* this run is the child below */
    char self[512], script[64], cmd[1024];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    CHECK(sl > 0);
    self[sl] = 0;
    const char *base = strrchr(self, '/') ? strrchr(self, '/') + 1 : self;
    /* The kernel log the child's runner reads: one Xid 0.15 s into the run, from a process of this program's name. */
    snprintf(script, sizeof script, "xid-klog-%d.sh", (int)getpid());
    FILE *k = fopen(script, "w");
    CHECK(k != NULL);
    fprintf(k,
            "#!/bin/sh\nawk -v t=\"$1\" 'BEGIN { printf \"%%.6f k: NVRM: Xid (PCI:0000:01:00): 69, pid=1, name=%s, "
            "Class Error: channel 0x2e, Class 0000c797, Offset 00000274, Data 00000006, ErrorCode 00000132\\n\", "
            "t + 0.15 }'\n",
            base);
    fclose(k);
    chmod(script, 0755);
    snprintf(cmd, sizeof cmd, "RELIFT_TEST_KLOG=./%s %s runner_xid_probe_passes_slowly 2>&1", script, self);
    FILE *f = popen(cmd, "r");
    CHECK(f != NULL);
    char out[8192];
    size_t n = fread(out, 1, sizeof out - 1, f);
    out[n] = 0;
    int st = pclose(f);
    remove(script);
    CHECK(strstr(out, "FAIL runner_xid_probe_passes_slowly faulted the GPU: NVIDIA Xid 69") != NULL);
    CHECK(strstr(out, "1 tests, 1 failed") != NULL);
    CHECK(st != 0);
#endif
}
