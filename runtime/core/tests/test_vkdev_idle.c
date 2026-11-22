/**
 * @file test_vkdev_idle.c
 * @brief rl_vkdev_wait_idle holds both queue locks while the device waits: a
 * submit on the guest queue or on the core's queue started meanwhile blocks
 * until the wait is over, as Vulkan's external synchronization of
 * vkDeviceWaitIdle requires.
 */
#include "runner.h"

#include "vkdev.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

static volatile int g_got_guest, g_got_core;

static void *take_guest(void *arg)
{
    (void)arg;
    rl_vkdev_guest_queue_lock();
    g_got_guest = 1;
    rl_vkdev_guest_queue_unlock();
    return NULL;
}

static void *take_core(void *arg)
{
    (void)arg;
    rl_vkdev_queue_lock();
    g_got_core = 1;
    rl_vkdev_queue_unlock();
    return NULL;
}

static int g_blocked;

static rlvk_result RLVK_CALL fake_idle(rlvk_handle dev)
{
    (void)dev;
    pthread_t a, b;
    pthread_create(&a, NULL, take_guest, NULL);
    pthread_create(&b, NULL, take_core, NULL);
    struct timespec t = { 0, 50 * 1000 * 1000 };
    nanosleep(&t, NULL);
    g_blocked = !g_got_guest && !g_got_core;
    pthread_detach(a);
    pthread_detach(b);
    return 0;
}

TEST(vkdev_wait_idle_holds_every_queue)
{
    rlvk_dev d;
    memset(&d, 0, sizeof d);
    d.vkDeviceWaitIdle = fake_idle;
    rl_vkdev_wait_idle(&d);
    CHECK(g_blocked);
    for (int i = 0; i < 100 && !(g_got_guest && g_got_core); i++) {
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    CHECK(g_got_guest && g_got_core);
}
