/**
 * @file event.c
 * @brief eventfd, timerfd and epoll: a runtime with its own scheduler (Go's
 * netpoller and timers) sleeps in an epoll wait and is woken through an
 * eventfd or a timerfd. Each call forwards to the host layer (hostio.h),
 * which gives the host's own object where it has one.
 */
#include "hostio.h"
#include "linux_priv.h"

#include <string.h>

enum {
    EFD_NONBLOCK = 04000,
    EFD_CLOEXEC = 02000000,
    EFD_SEMAPHORE = 1,
    TFD_NONBLOCK = 04000,
    TFD_CLOEXEC = 02000000,
    TFD_TIMER_ABSTIME = 1,
    EPOLL_CLOEXEC = 02000000,
    EP_MAX_EVENTS = 1024
};

static int64_t install(int64_t h, bool cloexec)
{
    if (h < 0)
        return h;
    for (int i = 0; i < LX_MAX_FDS; i++)
        if (lx.fds[i].host < 0) {
            lx.fds[i] = (lx_fd){ .host = (int)h, .cloexec = cloexec };
            return i;
        }
    hio_close((int)h);
    return -L_EMFILE;
}

static int64_t sys_eventfd2(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint32_t flags = (uint32_t)a[1];
    if (flags & ~(uint32_t)(EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE))
        return -L_EINVAL;
    return install(hio_eventfd((uint32_t)a[0], flags), (flags & EFD_CLOEXEC) != 0);
}

static int64_t sys_timerfd_create(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint32_t flags = (uint32_t)a[1];
    if (flags & ~(uint32_t)(TFD_NONBLOCK | TFD_CLOEXEC))
        return -L_EINVAL;
    return install(hio_timerfd_create((int)a[0], flags), (flags & TFD_CLOEXEC) != 0);
}

static int64_t sys_timerfd_settime(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    if ((uint32_t)a[1] & ~(uint32_t)TFD_TIMER_ABSTIME)
        return -L_EINVAL;
    int64_t nv[4], old[4];
    if (!lx_get(nv, a[2], sizeof nv))
        return -L_EFAULT;
    if (nv[1] < 0 || nv[1] >= 1000000000 || nv[3] < 0 || nv[3] >= 1000000000)
        return -L_EINVAL;
    int64_t r = hio_timerfd_settime(h, (uint32_t)a[1], nv, a[3] ? old : NULL);
    if (r < 0)
        return r;
    if (a[3] && !lx_put(a[3], old, sizeof old))
        return -L_EFAULT;
    return 0;
}

static int64_t sys_timerfd_gettime(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    int64_t v[4];
    int64_t r = hio_timerfd_gettime(h, v);
    if (r < 0)
        return r;
    return lx_put(a[1], v, sizeof v) ? 0 : -L_EFAULT;
}

static int64_t sys_epoll_create1(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if ((uint32_t)a[0] & ~(uint32_t)EPOLL_CLOEXEC)
        return -L_EINVAL;
    return install(hio_epoll_create(), true);
}

static int64_t sys_epoll_ctl(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int ep = lx_host_fd((int)a[0]), h = lx_host_fd((int)a[2]);
    if (ep < 0 || h < 0)
        return -L_EBADF;
    uint32_t ev[4] = { 0 };
    if ((int)a[1] != 2 && !lx_get(ev, a[3], 16))
        return -L_EFAULT;
    uint64_t data;
    memcpy(&data, &ev[2], 8);
    return hio_epoll_ctl(ep, (int)a[1], h, ev[0], data);
}

static int64_t sys_epoll_pwait(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int ep = lx_host_fd((int)a[0]);
    int64_t max = (int32_t)a[2], ms = (int32_t)a[3];
    if (ep < 0)
        return -L_EBADF;
    if (max <= 0 || max > EP_MAX_EVENTS)
        return -L_EINVAL;
    if (!lx_ok(a[1], (uint64_t)max * 16, RL_VM_W))
        return -L_EFAULT;
    int64_t end = ms < 0 ? 0 : (int64_t)rl_clock_ns() + ms * 1000000;
    hio_epoll_event ev[EP_MAX_EVENTS];
    uint32_t out[EP_MAX_EVENTS][4];
    for (;;) {
        int64_t left = ms < 0 ? -1 : end - (int64_t)rl_clock_ns();
        if (ms >= 0 && left < 0)
            left = 0;
        int64_t n = lx_epoll_wait(ep, ev, (int)max, left);
        if (n == -L_EINTR)
            return -L_ERESTARTNOHAND;
        if (n < 0)
            return n;
        if (n > 0) {
            memset(out, 0, (size_t)n * 16);
            for (int64_t i = 0; i < n; i++) {
                out[i][0] = ev[i].events;
                memcpy(&out[i][2], &ev[i].data, 8);
            }
            return lx_put(a[1], out, (uint64_t)n * 16) ? n : -L_EFAULT;
        }
        if (ms >= 0 && (int64_t)rl_clock_ns() >= end)
            return 0;
    }
}

lx_sys_fn *lx_event_call(uint32_t nr)
{
    switch (nr) {
    case NR_eventfd2: return sys_eventfd2;
    case NR_epoll_create1: return sys_epoll_create1;
    case NR_epoll_ctl: return sys_epoll_ctl;
    case NR_epoll_pwait: return sys_epoll_pwait;
    case NR_timerfd_create: return sys_timerfd_create;
    case NR_timerfd_settime: return sys_timerfd_settime;
    case NR_timerfd_gettime: return sys_timerfd_gettime;
    default: return NULL;
    }
}
