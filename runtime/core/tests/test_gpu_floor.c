/**
 * @file test_gpu_floor.c
 * @brief The GPU clock floor: which floor a host gets, the frame span, and
 * the gpu.clock_floor setting.
 */
#include "gpu_floor.h"
#include "runner.h"

#include <relift/settings.h>

#include <stdlib.h>

TEST(gpu_floor_is_android_only_and_prefers_the_queue_hint)
{
    CHECK_EQ(rl_gpu_floor_pick(false, true, true, true), RL_GPU_FLOOR_OFF);
    CHECK_EQ(rl_gpu_floor_pick(true, false, true, true), RL_GPU_FLOOR_NONE);
    CHECK_EQ(rl_gpu_floor_pick(true, true, true, true), RL_GPU_FLOOR_QUEUE_HINT);
    CHECK_EQ(rl_gpu_floor_pick(true, true, false, true), RL_GPU_FLOOR_ADPF);
    CHECK_EQ(rl_gpu_floor_pick(true, true, false, false), RL_GPU_FLOOR_NONE);
}

TEST(gpu_floor_span_is_a_frame_or_nothing)
{
    CHECK_EQ(rl_gpu_floor_span(1000, 16001000), 16000000);
    CHECK_EQ(rl_gpu_floor_span(0, 5000), 0);
    CHECK_EQ(rl_gpu_floor_span(9000, 5000), 0);
    CHECK_EQ(rl_gpu_floor_span(1, 1000000001), 0);
    CHECK_EQ(rl_gpu_floor_span(1, 999999999), 999999998);
}

TEST(gpu_floor_setting_is_on_by_default_and_does_nothing_off_android)
{
    rl_settings_reset();
    unsetenv("RELIFT_GPU_CLOCK_FLOOR");
    rl_settings_declare_runtime();
    CHECK(rl_setting_bool(RL_GPU_FLOOR_KEY));
    setenv("RELIFT_GPU_CLOCK_FLOOR", "0", 1);
    CHECK(!rl_setting_bool(RL_GPU_FLOOR_KEY));
    unsetenv("RELIFT_GPU_CLOCK_FLOOR");
#if !defined(__ANDROID__)
    rl_gpu_floor_start(NULL);
    CHECK_EQ(rl_gpu_floor_active(), RL_GPU_FLOOR_NONE);
    rl_gpu_floor_work();
    rl_gpu_floor_frame();
    CHECK_EQ(rl_gpu_floor_active(), RL_GPU_FLOOR_NONE);
#endif
}
