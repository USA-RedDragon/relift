/**
 * @file test_srv_nv.c
 * @brief The nvdrv services through IPC, the way libnx and the SDK drive
 * them: the session and descriptor rules, the init sequences, nvmap, the
 * syncpoints and events of nvhost-ctrl, ctrl-gpu, address spaces, channels
 * and the argument checks.
 */
#include "tc.h"

#include "nv.h"

#include <stdlib.h>

#define IOC(dir, type, nr, size) \
    (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr))
#define IOW(type, nr, size) IOC(1, type, nr, size)
#define IOR(type, nr, size) IOC(2, type, nr, size)
#define IOWR(type, nr, size) IOC(3, type, nr, size)

enum {
    IN_AT = 0x20000,
    OUT_AT = 0x22000,
    INL_AT = 0x24000,
    OUTL_AT = 0x26000,
    PATH_AT = 0x28000,
    EV_AT = 0x2A000,
    TMEM_AT = 0x100000,
    BUF_AT = 0x400000,
    HEAP_SIZE = 0x1000000
};

typedef struct nvt {
    rl_ctx *c;
    uint64_t heap;
    tc_obj o;
    uint32_t tmem_n;
} nvt;

static uint32_t nv_tmem(nvt *t)
{
    uint64_t at = t->heap + TMEM_AT + (uint64_t)t->tmem_n++ * 0x20000;
    if (t_svc(t->c, SVC_CREATE_TRANSFER_MEMORY, 0, at, 0x10000, HOS_PERM_R, 0, 0) != 0)
        return 0;
    return (uint32_t)t->c->x[1];
}

static uint32_t nv_init_with(nvt *t, tc_obj o, uint32_t proc, uint32_t tmem, uint32_t size)
{
    tc_req q = tc_new(3);
    tc_put32(&q, size);
    q.ncopy = 2;
    q.copy[0] = proc;
    q.copy[1] = tmem;
    tc_res r = tc_send(t->c, o, q);
    return r.svc_rc || r.result ? 0xBAD00000u | r.svc_rc | r.result : tc_u32(&r, 0);
}

static bool nv_init(nvt *t, tc_obj o)
{
    uint32_t tm = nv_tmem(t);
    if (!tm)
        return false;
    uint32_t err = nv_init_with(t, o, HOS_CUR_PROCESS, tm, 0x10000);
    tc_close(t->c, tm);
    return err == 0;
}

static bool nv_boot(nvt *t, const char *service, bool init)
{
    memset(t, 0, sizeof *t);
    t->c = t_boot(NULL);
    t->heap = t_heap(t->c, HEAP_SIZE);
    if (!t->heap)
        return false;
    t->o = tc_open_service(t->c, t->heap, service);
    if (!t->o.handle)
        return false;
    return !init || nv_init(t, t->o);
}

static uint32_t nv_open(nvt *t, tc_obj o, const char *path, int32_t *fd)
{
    memcpy(t_mem() + t->heap + PATH_AT, path, strlen(path));
    tc_req q = tc_new(0);
    q.na = 1;
    q.a_addr[0] = t->heap + PATH_AT;
    q.a_size[0] = strlen(path);
    tc_res r = tc_send(t->c, o, q);
    if (r.svc_rc || r.result) {
        *fd = -2;
        return 0xBAD00000u | r.svc_rc | r.result;
    }
    *fd = (int32_t)tc_u32(&r, 0);
    return tc_u32(&r, 4);
}

static int32_t nv_fd(nvt *t, const char *path)
{
    int32_t fd;
    return nv_open(t, t->o, path, &fd) == 0 ? fd : -2;
}

static uint32_t nv_close(nvt *t, tc_obj o, int32_t fd)
{
    tc_req q = tc_new(2);
    tc_put32(&q, (uint32_t)fd);
    tc_res r = tc_send(t->c, o, q);
    return r.svc_rc || r.result ? 0xBAD00000u : tc_u32(&r, 0);
}

static uint32_t nv_ioctl_raw(nvt *t, tc_obj o, uint32_t command, int32_t fd, uint32_t cmd, void *arg, uint32_t in_size,
                             uint32_t out_size, void *inl, uint32_t inl_size)
{
    uint8_t *m = t_mem() + t->heap;
    uint64_t g = t->heap;
    tc_req q = tc_new(command);
    tc_put32(&q, (uint32_t)fd);
    tc_put32(&q, cmd);
    if (in_size)
        memcpy(m + IN_AT, arg, in_size);
    memset(m + OUT_AT, 0, out_size);
    if (command == 11 && inl_size)
        memcpy(m + INL_AT, inl, inl_size);
    if (command == 12)
        memset(m + OUTL_AT, 0, inl_size);
    if (command == 1) {
        if (in_size) {
            q.a_addr[q.na] = g + IN_AT;
            q.a_size[q.na++] = in_size;
        }
        if (out_size) {
            q.b_addr[q.nb] = g + OUT_AT;
            q.b_size[q.nb++] = out_size;
        }
    } else if (command == 11) {
        q.a_addr[0] = g + IN_AT;
        q.a_size[0] = in_size;
        q.a_addr[1] = g + INL_AT;
        q.a_size[1] = inl_size;
        q.na = 2;
        if (out_size) {
            q.b_addr[0] = g + OUT_AT;
            q.b_size[0] = out_size;
            q.nb = 1;
        }
    } else {
        if (in_size) {
            q.a_addr[0] = g + IN_AT;
            q.a_size[0] = in_size;
            q.na = 1;
        }
        q.b_addr[0] = g + OUT_AT;
        q.b_size[0] = out_size;
        q.b_addr[1] = g + OUTL_AT;
        q.b_size[1] = inl_size;
        q.nb = 2;
    }
    tc_res r = tc_send(t->c, o, q);
    if (r.svc_rc || r.result)
        return 0xBAD00000u | r.svc_rc | r.result;
    if (out_size)
        memcpy(arg, m + OUT_AT, out_size);
    if (command == 12)
        memcpy(inl, m + OUTL_AT, inl_size);
    return tc_u32(&r, 0);
}

static uint32_t nv_ioctl_on(nvt *t, tc_obj o, int32_t fd, uint32_t cmd, void *arg)
{
    uint32_t size = (cmd >> 16) & 0x3FFF;
    return nv_ioctl_raw(t, o, 1, fd, cmd, arg, (cmd >> 30) & 1 ? size : 0, (cmd >> 30) & 2 ? size : 0, NULL, 0);
}

static uint32_t nv_ioctl(nvt *t, int32_t fd, uint32_t cmd, void *arg) { return nv_ioctl_on(t, t->o, fd, cmd, arg); }

static uint32_t nv_event(nvt *t, int32_t fd, uint32_t id, uint32_t *handle)
{
    tc_req q = tc_new(4);
    tc_put32(&q, (uint32_t)fd);
    tc_put32(&q, id);
    tc_res r = tc_send(t->c, t->o, q);
    *handle = r.ncopy == 1 ? r.copy[0] : 0;
    return r.svc_rc || r.result ? 0xBAD00000u : tc_u32(&r, 0);
}

static uint32_t wait_event(nvt *t, uint32_t handle, int64_t timeout)
{
    memcpy(t_mem() + t->heap + EV_AT, &handle, 4);
    return t_svc(t->c, SVC_WAIT_SYNCHRONIZATION, 0, t->heap + EV_AT, 1, (uint64_t)timeout, 0, 0);
}

typedef struct map_create {
    uint32_t size;
    int32_t handle;
} map_create;

typedef struct map_alloc {
    int32_t handle, heap_mask, flags, align;
    int64_t kind;
    uint64_t address;
} map_alloc;

typedef struct map_free {
    int32_t handle, padding;
    uint64_t address;
    uint32_t size;
    int32_t flags;
} map_free;

typedef struct map_param {
    int32_t handle, param, result;
} map_param;

typedef struct map_id {
    int32_t id, handle;
} map_id;

static int32_t map_new(nvt *t, int32_t nvmap, uint32_t size, uint64_t addr, int32_t align, int32_t kind)
{
    map_create cr = { size, 0 };
    if (nv_ioctl(t, nvmap, IOWR(1, 1, 8), &cr) != 0)
        return -1;
    map_alloc al = { cr.handle, 1, 0, align, kind, addr };
    if (nv_ioctl(t, nvmap, IOWR(1, 4, 0x20), &al) != 0)
        return -1;
    return cr.handle;
}

TEST(nv_session_rules)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", false));
    int32_t fd;
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvmap", &fd), NVR_NOT_INITIALIZED);
    CHECK_EQ(fd, -1);
    CHECK_EQ(nv_close(&t, t.o, 1), NVR_NOT_INITIALIZED);
    uint32_t args[2] = { 0, 0 };
    CHECK_EQ(nv_ioctl(&t, 1, IOWR(1, 1, 8), args), NVR_NOT_INITIALIZED);
    uint32_t ev;
    CHECK_EQ(nv_event(&t, 1, 1, &ev), NVR_NOT_INITIALIZED);

    tc_res r = tc_send(t.c, t.o, tc_new(6));
    CHECK_EQ(r.result, 0);
    const uint32_t *tls = (const uint32_t *)(const void *)(t_mem() + t.c->tpidrro_el0);
    CHECK_EQ(tls[(32 + 0x20) / 4], NVR_NOT_INITIALIZED);

    uint32_t tm = nv_tmem(&t);
    CHECK(tm);
    CHECK_EQ(nv_init_with(&t, t.o, HOS_CUR_PROCESS, 0x1234, 0x10000), NVR_INVALID_PARAMETER);
    CHECK_EQ(nv_init_with(&t, t.o, 0x1234, tm, 0x10000), NVR_INVALID_PARAMETER);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvmap", &fd), NVR_NOT_INITIALIZED);
    CHECK_EQ(nv_init_with(&t, t.o, HOS_CUR_PROCESS, tm, 0x10000), 0);
    tc_close(t.c, tm);

    r = tc_send(t.c, t.o, tc_new(6));
    CHECK_EQ(tls[(32 + 0x20) / 4], NVR_SUCCESS);

    CHECK_EQ(nv_open(&t, t.o, "/dev/nvmap", &fd), 0);
    CHECK_EQ(fd, 1);
    int32_t ctrl;
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-ctrl", &ctrl), 0);
    CHECK_EQ(ctrl, 2);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-ctrl-gpu", &fd), 0);
    CHECK_EQ(fd, 3);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nope", &fd), NVR_FILE_OPERATION_FAILED);
    CHECK_EQ(fd, -1);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-msenc", &fd), NVR_FILE_OPERATION_FAILED);
    CHECK_EQ(nv_open(&t, t.o, "", &fd), NVR_FILE_OPERATION_FAILED);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-dbg-gpu", &fd), NVR_NOT_SUPPORTED);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-prof-gpu", &fd), NVR_NOT_SUPPORTED);
    rl_nv_set_debug_mode(true);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-dbg-gpu", &fd), 0);
    CHECK_EQ(fd, 4);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-prof-gpu", &fd), 0);
    CHECK_EQ(fd, 5);
    CHECK_EQ(nv_ioctl(&t, 4, IOWR(1, 1, 8), args), NVR_IOCTL_FAILED);
    rl_nv_set_debug_mode(false);

    CHECK_EQ(nv_close(&t, t.o, 2), 0);
    CHECK_EQ(nv_close(&t, t.o, 2), NVR_NOT_IMPLEMENTED);
    CHECK_EQ(nv_close(&t, t.o, -1), NVR_INVALID_PARAMETER);
    CHECK_EQ(nv_close(&t, t.o, 0), NVR_NOT_IMPLEMENTED);
    CHECK_EQ(nv_open(&t, t.o, "/dev/nvhost-as-gpu", &fd), 0);
    CHECK_EQ(fd, 2);

    tc_req q = tc_new(8);
    q.send_pid = true;
    tc_put64(&q, 0x1234);
    r = tc_send(t.c, t.o, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    CHECK_EQ(tc_cmd(t.c, t.o, 9), 0);
    q = tc_new(13);
    tc_put64(&q, 0);
    CHECK_EQ(tc_send(t.c, t.o, q).result, 0);
    CHECK_EQ(tc_cmd(t.c, t.o, 10), SF_RESULT_UNKNOWN_COMMAND);

    tc_obj a = tc_open_service(t.c, t.heap, "nvdrv:a");
    CHECK(a.handle);
    CHECK_EQ(nv_open(&t, a, "/dev/nvmap", &fd), NVR_NOT_INITIALIZED);
    CHECK(nv_init(&t, a));
    CHECK_EQ(nv_open(&t, a, "/dev/nvmap", &fd), 0);
    CHECK_EQ(fd, 1);
    CHECK_EQ(nv_close(&t, a, 3), NVR_NOT_IMPLEMENTED);
    tc_obj s = tc_open_service(t.c, t.heap, "nvdrv:s");
    tc_obj tt = tc_open_service(t.c, t.heap, "nvdrv:t");
    CHECK(s.handle && tt.handle);
    CHECK_EQ(nv_open(&t, s, "/dev/nvmap", &fd), NVR_NOT_INITIALIZED);
    CHECK_EQ(nv_open(&t, tt, "/dev/nvmap", &fd), NVR_NOT_INITIALIZED);
}

TEST(nv_libnx_init_sequence)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", false));
    uint32_t tm = nv_tmem(&t);
    CHECK(tm);
    CHECK_EQ(nv_init_with(&t, t.o, HOS_CUR_PROCESS, tm, 0x800000), 0);
    tc_close(t.c, tm);
    tc_req q = tc_new(4);
    q.type = HIPC_CONTROL;
    tc_res r = tc_send(t.c, t.o, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    tc_obj clone = { r.move[0], 0, false };
    q = tc_new(8);
    q.send_pid = true;
    tc_put64(&q, 0x5678);
    CHECK_EQ(tc_send(t.c, t.o, q).result, 0);

    int32_t gpu = nv_fd(&t, "/dev/nvhost-ctrl-gpu");
    CHECK_EQ(gpu, 1);
    uint8_t chars[0xB0];
    memset(chars, 0, sizeof chars);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x47, 5, 0xB0), chars), 0);
    int32_t v32;
    int64_t v64;
    memcpy(&v64, chars, 8);
    CHECK_EQ(v64, 0xA0);
    memcpy(&v32, chars + 16, 4);
    CHECK_EQ(v32, 0x120);
    memcpy(&v32, chars + 16 + 4, 4);
    CHECK_EQ(v32, 0xB);
    memcpy(&v32, chars + 16 + 72, 4);
    CHECK_EQ(v32, 0x28);
    memcpy(&v32, chars + 16 + 92, 4);
    CHECK_EQ(v32, 0xB197);
    uint32_t zc = 0;
    CHECK_EQ(nv_ioctl(&t, gpu, IOR(0x47, 1, 4), &zc), 0);
    CHECK_EQ(zc, 1);
    uint32_t zinfo[10];
    CHECK_EQ(nv_ioctl(&t, gpu, IOR(0x47, 2, 0x28), zinfo), 0);
    CHECK_EQ(zinfo[0], 0x20);
    CHECK_EQ(zinfo[3], 0x800);
    CHECK_EQ(zinfo[9], 0x10);
    uint8_t tpc[0x18];
    memset(tpc, 0, sizeof tpc);
    uint32_t mask_size = 4;
    memcpy(tpc, &mask_size, 4);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x47, 6, 0x18), tpc), 0);
    memcpy(&v32, tpc + 16, 4);
    CHECK_EQ(v32, 3);

    int32_t nvmap = nv_fd(&t, "/dev/nvmap");
    CHECK_EQ(nvmap, 2);
    int32_t h = map_new(&t, nvmap, 0x10000, t.heap + BUF_AT, 0x1000, 0);
    CHECK_EQ(h, 4);
    map_id gi = { 0, h };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 0x0E, 8), &gi), 0);
    CHECK_EQ(gi.id, h);

    uint64_t times[2] = { 0, 0 };
    CHECK_EQ(nv_ioctl_on(&t, clone, gpu, IOWR(0x47, 0x1C, 16), times), 0);
    uint64_t t0 = times[0];
    CHECK_EQ(nv_ioctl_on(&t, clone, gpu, IOWR(0x47, 0x1C, 16), times), 0);
    CHECK(times[0] >= t0);
    CHECK(times[0] < (1ull << 56));
    CHECK_EQ(nv_close(&t, t.o, gpu), 0);
    CHECK_EQ(nv_ioctl_on(&t, clone, gpu, IOWR(0x47, 0x1C, 16), times), NVR_NOT_IMPLEMENTED);
    int32_t fd = -2;
    CHECK_EQ(nv_open(&t, clone, "/dev/nvhost-ctrl", &fd), 0);
    CHECK_EQ(fd, 1);
}

TEST(nv_nvmap_handles)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t nvmap = nv_fd(&t, "/dev/nvmap");
    CHECK_EQ(nvmap, 1);
    map_create cr = { 0x1234, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 1, 8), &cr), 0);
    CHECK_EQ(cr.handle, 4);
    map_create cr2 = { 0x3000, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 1, 8), &cr2), 0);
    CHECK_EQ(cr2.handle, 8);
    map_create zero = { 0, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 1, 8), &zero), NVR_INVALID_VALUE);

    rl_nv_map_info info;
    CHECK(rl_nv_map_lookup(4, &info));
    CHECK_EQ(info.size, 0x2000);
    CHECK(!info.allocated);
    CHECK(!rl_nv_map_lookup(12, &info));
    CHECK(!rl_nv_map_lookup(5, &info));

    uint64_t addr = t.heap + BUF_AT;
    map_alloc al = { 4, 0x1, 0x21, 0x20000, 0xFE, addr };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 4, 0x20), &al), 0);
    CHECK(rl_nv_map_lookup(4, &info));
    CHECK(info.allocated);
    CHECK_EQ(info.handle_id, 4);
    CHECK_EQ(info.size, 0x2000);
    CHECK_EQ(info.alignment, 0x20000);
    CHECK_EQ(info.flags, 0x21);
    CHECK_EQ(info.kind, 0xFE);
    CHECK_EQ(info.heap_id, 1);
    CHECK_EQ(info.addr, addr);

    map_param pa = { 4, 1, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), 0);
    CHECK_EQ(pa.result, 0x2000);
    pa.param = 2;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), 0);
    CHECK_EQ(pa.result, 0x20000);
    pa.param = 4;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), 0);
    CHECK_EQ(pa.result, 0x40000000);
    pa.param = 5;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), 0);
    CHECK_EQ(pa.result, 0xFE);
    pa.param = 6;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), 0);
    CHECK_EQ(pa.result, 0);
    pa.param = 3;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), NVR_INVALID_VALUE);
    pa.param = 9;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), NVR_INVALID_VALUE);
    pa.handle = 99;
    pa.param = 1;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 9, 12), &pa), NVR_INVALID_VALUE);

    map_alloc bad = { 8, 1, 0, 3, 0, addr };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 4, 0x20), &bad), NVR_INVALID_VALUE);
    map_alloc none = { 8, 1, 0, 0x100, 0, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 4, 0x20), &none), NVR_INSUFFICIENT_MEMORY);
    CHECK(rl_nv_map_lookup(8, &info));
    CHECK(!info.allocated);
    map_alloc good = { 8, 2, 0, 0x100, 0x10, addr + 0x10000 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 4, 0x20), &good), 0);
    CHECK_EQ(good.align, 0x1000);
    CHECK(rl_nv_map_lookup(8, &info));
    CHECK_EQ(info.alignment, 0x1000);
    CHECK_EQ(info.kind, 0x10);
    map_alloc again = { 8, 2, 0, 0x1000, 0x20, 0x77000 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 4, 0x20), &again), 0);
    CHECK(rl_nv_map_lookup(8, &info));
    CHECK_EQ(info.addr, addr + 0x10000);

    map_id gi = { 0, 4 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 0x0E, 8), &gi), 0);
    CHECK_EQ(gi.id, 4);
    map_id fi = { 4, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 3, 8), &fi), 0);
    CHECK_EQ(fi.handle, 4);
    map_id none_id = { 20, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 3, 8), &none_id), NVR_INVALID_VALUE);
    map_free fr = { 4, 0, 0, 0, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 5, 0x18), &fr), 0);
    CHECK_EQ(fr.flags, 1);
    CHECK_EQ(fr.address, 0);
    CHECK_EQ(fr.size, 0x2000);
    CHECK(rl_nv_map_lookup(4, &info));
    memset(&fr, 0, sizeof fr);
    fr.handle = 4;
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 5, 0x18), &fr), 0);
    CHECK_EQ(fr.flags, 0);
    CHECK_EQ(fr.address, addr);
    CHECK_EQ(fr.size, 0x2000);
    CHECK(!rl_nv_map_lookup(4, &info));
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 5, 0x18), &fr), NVR_INVALID_VALUE);

    map_create cr3 = { 0x1000, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 1, 8), &cr3), 0);
    CHECK_EQ(cr3.handle, 12);

    uint32_t junk[8] = { 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 2, 8), junk), NVR_NOT_SUPPORTED);
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 0x11, 8), junk), NVR_NOT_SUPPORTED);
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 0x30, 8), junk), NVR_IOCTL_FAILED);
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(0x47, 1, 8), junk), NVR_IOCTL_FAILED);
    map_create small = { 0x1000, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 1, 4), &small), NVR_INVALID_VALUE);

    rl_nv_reset();
    CHECK(!rl_nv_map_lookup(8, &info));
    CHECK(!rl_nv_map_lookup(12, &info));
}

TEST(nv_ctrl_syncpoints)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t ctrl = nv_fd(&t, "/dev/nvhost-ctrl");
    CHECK_EQ(ctrl, 1);
    uint32_t f[3] = { 5, 99, 0 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x14, 8), f), 0);
    CHECK_EQ(f[1], 0);
    f[0] = 192;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x14, 8), f), NVR_INVALID_VALUE);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1A, 8), f), NVR_INVALID_VALUE);

    uint32_t id = 5;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x15, 4), &id), 0);
    f[0] = 5;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x14, 8), f), 0);
    CHECK_EQ(f[1], 1);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1A, 8), f), 0);
    CHECK_EQ(f[1], 0);
    id = 26;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x15, 4), &id), 0);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x15, 4), &id), 0);
    f[0] = 26;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x14, 8), f), 0);
    CHECK_EQ(f[1], 2);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1A, 8), f), 0);
    CHECK_EQ(f[1], 2);
    id = 192;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x15, 4), &id), NVR_INVALID_VALUE);

    uint32_t w[3] = { 26, 2, 0 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x16, 12), w), 0);
    w[1] = 3;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x16, 12), w), NVR_TIMEOUT);
    uint32_t wx[4] = { 26, 2, 0, 0x55 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x19, 16), wx), 0);
    CHECK_EQ(wx[3], 2);
    w[0] = 500;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x16, 12), w), NVR_INVALID_VALUE);

    uint8_t cfg[0x183];
    memset(cfg, 0, sizeof cfg);
    memcpy(cfg, "nv", 3);
    memcpy(cfg + 0x41, "x", 2);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1B, 0x183), cfg), NVR_INVALID_VALUE);
}

TEST(nv_ctrl_events)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t ctrl = nv_fd(&t, "/dev/nvhost-ctrl");
    CHECK_EQ(ctrl, 1);
    uint32_t h;
    CHECK_EQ(nv_event(&t, ctrl, 0x101A0000, &h), NVR_INVALID_VALUE);

    uint32_t ew[4] = { 26, 1, 100, 0 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1D, 16), ew), NVR_TIMEOUT);
    CHECK_EQ(ew[3], 0x101A0000);
    CHECK_EQ(nv_event(&t, ctrl, ew[3], &h), 0);
    CHECK(h);
    CHECK_EQ(nv_event(&t, ctrl, 0x101B0000, &h), NVR_INVALID_VALUE);
    CHECK_EQ(nv_event(&t, ctrl, 0x101A0001, &h), NVR_INVALID_VALUE);
    CHECK_EQ(nv_event(&t, ctrl, 0x101A0000, &h), 0);
    CHECK_EQ(wait_event(&t, h, 0), KR_TIMED_OUT);

    uint32_t id = 26;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x15, 4), &id), 0);
    CHECK_EQ(wait_event(&t, h, 0), 0);
    CHECK_EQ(t_svc(t.c, SVC_RESET_SIGNAL, h, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(wait_event(&t, h, 0), KR_TIMED_OUT);

    uint32_t ew2[4] = { 26, 3, 100, 0 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1D, 16), ew2), NVR_TIMEOUT);
    CHECK_EQ(ew2[3], 0x101A0000);
    CHECK_EQ(wait_event(&t, h, 0), KR_TIMED_OUT);
    for (int i = 0; i < 2; i++)
        CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x15, 4), &id), 0);
    CHECK_EQ(wait_event(&t, h, 0), 0);

    uint32_t slot = 0;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1C, 4), &slot), 0);
    CHECK_EQ(wait_event(&t, h, 0), KR_TIMED_OUT);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x20, 4), &slot), 0);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1C, 4), &slot), NVR_INVALID_VALUE);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x20, 4), &slot), 0);
    slot = 64;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1F, 4), &slot), NVR_INVALID_VALUE);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x20, 4), &slot), NVR_INVALID_VALUE);
    slot = 7;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1F, 4), &slot), 0);

    uint32_t ea[4] = { 26, 9, 100, 9 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1E, 16), ea), NVR_INVALID_VALUE);
    ea[3] = 70;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1E, 16), ea), NVR_INVALID_VALUE);
    ea[3] = 7;
    ea[1] = 6;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1E, 16), ea), NVR_TIMEOUT);
    CHECK_EQ(ea[3], (26u << 4) | 7);
    uint32_t h7;
    CHECK_EQ(nv_event(&t, ctrl, ea[3], &h7), 0);
    CHECK_EQ(wait_event(&t, h7, 0), KR_TIMED_OUT);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x20, 4), &slot), NVR_BUSY);
    uint64_t mask = 1ull << 7;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x21, 8), &mask), NVR_BUSY);
    for (int i = 0; i < 3; i++)
        CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x15, 4), &id), 0);
    CHECK_EQ(wait_event(&t, h7, 0), 0);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x21, 8), &mask), 0);
    CHECK_EQ(nv_event(&t, ctrl, ea[3], &h7), NVR_INVALID_VALUE);

    uint32_t imm[4] = { 26, 1, 0, 0 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1D, 16), imm), 0);
    CHECK_EQ(imm[3], 6);
    imm[1] = 100;
    imm[2] = 0;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1D, 16), imm), NVR_TIMEOUT);
    imm[2] = 5;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1D, 16), imm), NVR_TIMEOUT);
    CHECK_EQ(imm[3], 0x101A0000);
}

TEST(nv_ctrl_gpu_device)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t gpu = nv_fd(&t, "/dev/nvhost-ctrl-gpu");
    CHECK_EQ(gpu, 1);

    uint8_t chars[0xA0];
    uint8_t hdr[0x10];
    memset(hdr, 0, sizeof hdr);
    memset(chars, 0, sizeof chars);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, gpu, IOWR(0x47, 5, 0x10), hdr, 0x10, 0x10, chars, sizeof chars), 0);
    int64_t size;
    memcpy(&size, hdr, 8);
    CHECK_EQ(size, 0xA0);
    int32_t v;
    memcpy(&v, chars + 0, 4);
    CHECK_EQ(v, 0x120);
    memcpy(&v, chars + 60, 4);
    CHECK_EQ(v, 0x503);
    memcpy(&v, chars + 100, 4);
    CHECK_EQ(v, 0xB06F);
    int64_t chip;
    memcpy(&chip, chars + 144, 8);
    CHECK_EQ(chip, 0x6230326D67);

    uint8_t full[0xB0];
    memset(full, 0, sizeof full);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x47, 5, 0xB0), full), 0);
    CHECK(memcmp(full + 16, chars, sizeof chars) == 0);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 11, gpu, IOWR(0x47, 5, 0x10), hdr, 0x10, 0x10, chars, 4), NVR_IOCTL_FAILED);

    uint8_t tpc[0x18];
    memset(tpc, 0, sizeof tpc);
    int32_t mask = 4;
    memcpy(tpc, &mask, 4);
    int32_t inline_mask = 0;
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, gpu, IOWR(0x47, 6, 0x18), tpc, 0x18, 0x18, &inline_mask, 4), 0);
    CHECK_EQ(inline_mask, 3);
    memset(tpc, 0, sizeof tpc);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x47, 6, 0x18), tpc), 0);
    memcpy(&v, tpc + 16, 4);
    CHECK_EQ(v, 0);

    uint32_t nv[2] = { 0, 0 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOR(0x47, 0x12, 8), nv), 0);
    CHECK_EQ(nv[0], 2);
    nv[0] = 0;
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, gpu, IOR(0x47, 0x12, 8), nv, 0, 8, nv + 1, 0), 0);
    CHECK_EQ(nv[0], 2);
    uint8_t vm[8] = { 0 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOR(0x47, 0x13, 8), vm), 0);
    CHECK_EQ(vm[3], 1);
    int32_t slot[2] = { 0, 0 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOR(0x47, 0x14, 8), slot), 0);
    CHECK_EQ(slot[0], 7);
    CHECK_EQ(slot[1], 1);
    uint32_t zbc[11] = { 0 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x47, 3, 0x2C), zbc), 0);

    uint64_t a[2] = { 0, 0 }, b[2] = { 0, 0 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x47, 0x1C, 16), a), 0);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x47, 0x1C, 16), b), 0);
    CHECK(b[0] >= a[0]);

    uint32_t e1, e2, e3;
    CHECK_EQ(nv_event(&t, gpu, 1, &e1), 0);
    CHECK_EQ(nv_event(&t, gpu, 2, &e2), 0);
    CHECK(e1 && e2 && e1 != e2);
    CHECK_EQ(nv_event(&t, gpu, 3, &e3), NVR_INVALID_VALUE);
    CHECK_EQ(wait_event(&t, e1, 0), KR_TIMED_OUT);
}

TEST(nv_ctrl_gpu_switch2_characteristics)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    rl_hos_switch2_config(&cfg);
    nvt t;
    memset(&t, 0, sizeof t);
    t.c = t_boot(&cfg);
    t.heap = t_heap(t.c, HEAP_SIZE);
    t.o = tc_open_service(t.c, t.heap, "nvdrv");
    CHECK(t.o.handle != 0);
    CHECK(nv_init(&t, t.o));
    int32_t gpu = nv_fd(&t, "/dev/nvhost-ctrl-gpu");
    CHECK(gpu > 0);
    uint8_t hdr[0xE0], chars[0xD0];
    memset(hdr, 0, sizeof hdr);
    memset(chars, 0xEE, sizeof chars);
    hdr[0] = 0xD0;
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, gpu, 0xC0E04705u, hdr, 0xE0, 0xE0, chars, sizeof chars), 0);
    uint64_t size, flags, chip;
    memcpy(&size, hdr, 8);
    CHECK_EQ(size, 0xD0);
    uint32_t v;
    memcpy(&v, chars + 0x00, 4);
    CHECK_EQ(v, 0x170);
    memcpy(&v, chars + 0x04, 4);
    CHECK_EQ(v, 0xE);
    memcpy(&flags, chars + 0x50, 8);
    CHECK_EQ(flags, 0x935FAF1EDC0155ull);
    memcpy(&v, chars + 0x5C, 4);
    CHECK_EQ(v, 0xC797);
    memcpy(&v, chars + 0x6C, 4);
    CHECK_EQ(v, 0xC7B5);
    int16_t last;
    memcpy(&last, chars + 0x7A, 2);
    CHECK_EQ(last, -1);
    memcpy(&chip, chars + 0xA4, 8);
    CHECK_EQ(chip, 0x6761313066ull);
    memcpy(&v, chars + 0xB0, 4);
    CHECK_EQ(v, 0x2);
    memcpy(&v, chars + 0xCC, 4);
    CHECK_EQ(v, 0);

    uint8_t tpc[0x18];
    memset(tpc, 0, sizeof tpc);
    tpc[0] = 4;
    int32_t mask = 0;
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, gpu, IOWR(0x47, 6, 0x18), tpc, 0x18, 0x18, &mask, 4), 0);
    CHECK_EQ(mask, 0x3F);

    uint8_t info[16];
    memset(info, 0, sizeof info);
    info[1] = 4;
    uint32_t items[256];
    memset(items, 0xEE, sizeof items);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, gpu, 0xC010471Fu, info, 16, 16, items, sizeof items), 0);
    memcpy(&v, info, 4);
    CHECK_EQ(v, 32);
    CHECK_EQ(items[0], 0);
    CHECK_EQ(items[4], 2);
    CHECK_EQ(items[6], 1);

    uint8_t vs[8] = { 0 }, sms[24];
    memset(sms, 0xEE, sizeof sms);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, gpu, IOWR(0x47, 0x13, 8), vs, 8, 8, sms, sizeof sms), 0);
    CHECK_EQ(sms[0], 0);
    CHECK_EQ(sms[1], 0);
    CHECK_EQ(sms[22], 0);
    CHECK_EQ(sms[23], 5);

    uint32_t word = 0;
    nv_ioctl(&t, gpu, IOWR(0x47, 0x7E, 4), &word);
    CHECK_EQ(t_svc_rc, RL_PENDING);
    CHECK_EQ(t.c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(nv_as_gpu_ranges)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t nvmap = nv_fd(&t, "/dev/nvmap");
    int32_t as = nv_fd(&t, "/dev/nvhost-as-gpu");
    int32_t ch = nv_fd(&t, "/dev/nvhost-gpu");
    CHECK(nvmap > 0 && as > 0 && ch > 0);
    int32_t h = map_new(&t, nvmap, 0x10000, t.heap + BUF_AT, 0x10000, 0);
    CHECK(h > 0);

    uint32_t init[10] = { 1, as, 0x10000, 0 };
    CHECK_EQ(nv_ioctl(&t, as, IOW(0x41, 9, 40), init), 0);

    struct {
        uint64_t unused;
        uint32_t size, pad;
        struct {
            uint64_t offset;
            uint32_t page_size, pad;
            uint64_t pages;
        } r[2];
    } va;
    memset(&va, 0, sizeof va);
    va.size = 0x30;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 8, 0x40), &va), 0);
    CHECK_EQ(va.size, 0x30);
    CHECK_EQ(va.r[0].offset, 0x100000000ull);
    CHECK_EQ(va.r[0].page_size, 0x1000);
    CHECK_EQ(va.r[0].pages, 0x300000);
    CHECK_EQ(va.r[1].offset, 0x400000000ull);
    CHECK_EQ(va.r[1].page_size, 0x10000);
    CHECK_EQ(va.r[1].pages, (0x2000000000ull - 0x400000000ull) / 0x10000);
    memset(&va, 0, sizeof va);
    va.size = 0x18;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 8, 0x40), &va), 0);
    CHECK_EQ(va.r[0].page_size, 0x1000);
    CHECK_EQ(va.r[1].page_size, 0);
    memset(&va, 0, sizeof va);
    va.size = 0x30;
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, as, IOWR(0x41, 8, 0x40), &va, 0x40, 0x40, &va, 0), 0);
    CHECK_EQ(va.r[1].offset, 0x400000000ull);

    struct {
        uint32_t pages, page_size, flags, pad;
        uint64_t offset;
    } al = { 0x10, 0x10000, 0, 0, 0x10000 };
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 2, 24), &al), 0);
    CHECK_EQ(al.offset, 0x100000000ull);
    uint64_t first = al.offset;
    al.offset = 0x10000;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 2, 24), &al), 0);
    CHECK_EQ(al.offset, 0x100100000ull);
    al.pages = 4;
    al.flags = 1;
    al.offset = 0x200000000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 2, 24), &al), 0);
    CHECK_EQ(al.offset, 0x200000000ull);
    al.offset = 0x200010000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 2, 24), &al), NVR_INSUFFICIENT_MEMORY);
    CHECK_EQ(al.offset, 0);

    struct {
        uint64_t offset;
        uint32_t pages, page_size;
    } fr = { first, 0x10, 0x10000 };
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 3, 16), &fr), 0);
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 3, 16), &fr), NVR_INVALID_VALUE);
    al.pages = 0x10;
    al.flags = 0;
    al.offset = 0x10000;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 2, 24), &al), 0);
    CHECK_EQ(al.offset, first);

    struct {
        uint32_t flags;
        int32_t kind, handle, page_size;
        uint64_t buffer_offset, mapping_size, offset;
    } mb = { 0, 0, h, 0x10000, 0, 0, 0 };
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), 0);
    CHECK_EQ(mb.offset, 0x100200000ull);
    uint64_t mapped = mb.offset;
    struct {
        uint64_t offset;
    } um = { mapped };
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 5, 8), &um), 0);
    mb.offset = 0;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), 0);
    CHECK_EQ(mb.offset, mapped);

    mb.flags = 1;
    mb.offset = 0x200000000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), 0);
    CHECK_EQ(mb.offset, 0x200000000ull);
    mb.offset = 0x200000000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), NVR_INVALID_VALUE);
    mb.offset = 0x200010000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), 0);
    mb.offset = 0x300000000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), NVR_INVALID_VALUE);
    mb.offset = 0x200020800ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), NVR_INVALID_VALUE);
    mb.flags = 0;
    mb.handle = 99;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), NVR_INVALID_VALUE);
    mb.flags = 0x100;
    mb.handle = h;
    mb.offset = 0x200000000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), 0);
    mb.offset = 0x7000000000ull;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), NVR_INVALID_VALUE);
    um.offset = 0x9999000;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 5, 8), &um), 0);

    struct {
        uint16_t flags, kind;
        int32_t handle;
        uint32_t map_offset, gpu_offset, pages;
    } rm[2] = { { 0, 0, h, 0, 0x50000, 1 }, { 0, 0, 0, 0, 0x50000, 1 } };
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 0x14, 40), rm), 0);
    rm[0].handle = 77;
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 0x14, 20), rm), NVR_INVALID_VALUE);

    struct {
        int32_t fd;
    } bind = { ch };
    CHECK_EQ(nv_ioctl(&t, as, IOW(0x41, 1, 4), &bind), 0);
    bind.fd = nvmap;
    CHECK_EQ(nv_ioctl(&t, as, IOW(0x41, 1, 4), &bind), NVR_INVALID_VALUE);
    bind.fd = 99;
    CHECK_EQ(nv_ioctl(&t, as, IOW(0x41, 1, 4), &bind), NVR_INVALID_VALUE);

    CHECK_EQ(nv_close(&t, t.o, as), 0);
    int32_t as2 = nv_fd(&t, "/dev/nvhost-as-gpu");
    CHECK_EQ(as2, as);
    al.pages = 1;
    al.page_size = 0x1000;
    al.offset = 0x1000;
    CHECK_EQ(nv_ioctl(&t, as2, IOWR(0x41, 2, 24), &al), 0);
    CHECK_EQ(al.offset, 0x100000000ull);
}

TEST(nv_channels)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t ctrl = nv_fd(&t, "/dev/nvhost-ctrl");
    int32_t nvmap = nv_fd(&t, "/dev/nvmap");
    int32_t gpu = nv_fd(&t, "/dev/nvhost-gpu");
    int32_t vic = nv_fd(&t, "/dev/nvhost-vic");
    int32_t dec = nv_fd(&t, "/dev/nvhost-nvdec");
    int32_t jpg = nv_fd(&t, "/dev/nvhost-nvjpg");
    int32_t enc = nv_fd(&t, "/dev/nvhost-nvenc");
    CHECK(ctrl > 0 && nvmap > 0 && gpu > 0 && vic > 0 && dec > 0 && jpg > 0 && enc > 0);

    int32_t nvmap_fd = nvmap;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x48, 1, 4), &nvmap_fd), 0);
    uint32_t timeout = 3000;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x48, 3, 4), &timeout), 0);
    uint32_t prio = 100;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x48, 0x0D, 4), &prio), 0);
    prio = 7;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x48, 0x0D, 4), &prio), NVR_INVALID_VALUE);
    uint32_t slice = 999;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x48, 0x1D, 4), &slice), NVR_INVALID_VALUE);
    slice = 5000;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x48, 0x1D, 4), &slice), 0);
    uint64_t user = 0x1234;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x47, 0x14, 8), &user), 0);

    struct {
        uint32_t entries, jobs, flags;
        uint32_t fence_id, fence_value;
        uint32_t r[3];
    } gf = { 0x800, 1, 0, 0, 77, { 0 } };
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 0x1A, 32), &gf), 0);
    CHECK_EQ(gf.fence_id, 1);
    CHECK_EQ(gf.fence_value, 0);
    uint32_t gpu_sync = gf.fence_id;
    gf.fence_id = 0;
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0x48, 0x18, 32), &gf), 0);
    CHECK_EQ(gf.fence_id, 2);
    uint32_t oc[4] = { 0xB197, 0, 0, 0 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 9, 16), oc), 0);
    uint64_t zc[2] = { 0x100000000ull, 1 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 0x0B, 16), zc), 0);
    uint64_t en[3] = { 0, 0x1000, 1 };
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 0x0C, 24), en), 0);
    uint8_t notif[16];
    memset(notif, 0xFF, sizeof notif);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 0x17, 16), notif), 0);
    CHECK_EQ(notif[3], 0);

    uint32_t e[3];
    for (uint32_t i = 1; i <= 3; i++)
        CHECK_EQ(nv_event(&t, gpu, i, &e[i - 1]), 0);
    CHECK(e[0] && e[1] && e[2] && e[0] != e[1] && e[1] != e[2]);
    uint32_t bad;
    CHECK_EQ(nv_event(&t, gpu, 4, &bad), NVR_INVALID_VALUE);
    CHECK_EQ(nv_event(&t, vic, 1, &bad), NVR_IOCTL_FAILED);

    uint32_t gp[2] = { 0, 0 };
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 2, 8), gp), 0);
    uint32_t dev_sync = gp[1];
    CHECK(dev_sync > 2);
    gp[1] = 0;
    CHECK_EQ(nv_ioctl(&t, dec, IOWR(0, 2, 8), gp), 0);
    CHECK_EQ(gp[1], dev_sync);
    gp[0] = 16;
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 2, 8), gp), NVR_INVALID_VALUE);
    gp[0] = 1;
    gp[1] = 9;
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 3, 8), gp), 0);
    CHECK_EQ(gp[1], 0);
    uint32_t st = 1000;
    CHECK_EQ(nv_ioctl(&t, vic, IOW(0, 7, 4), &st), 0);

    struct {
        int64_t address;
        int32_t entries;
        uint32_t flags, fence_id, fence_value;
        uint64_t list[2];
    } sg = { 0x1000, 2, 2, 0, 0, { 1, 2 } };
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 8, 40), &sg), 0);
    CHECK_EQ(sg.fence_id, gpu_sync);
    CHECK_EQ(sg.fence_value, 2);
    CHECK_EQ(sg.flags, 0);
    uint32_t f[2] = { gpu_sync, 0 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x14, 8), f), 0);
    CHECK_EQ(f[1], 2);
    uint32_t ew[4] = { gpu_sync, 2, 100, 0 };
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1D, 16), ew), 0);
    CHECK_EQ(ew[3], 2);

    sg.flags = 0x100;
    sg.fence_value = 3;
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 8, 40), &sg), 0);
    CHECK_EQ(sg.fence_value, 5);
    sg.flags = 0;
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 8, 40), &sg), 0);
    CHECK_EQ(sg.fence_value, 5);
    sg.flags = 1 | 0x100;
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 8, 40), &sg), NVR_INVALID_VALUE);
    sg.flags = 0;
    sg.entries = 3;
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 8, 40), &sg), NVR_INVALID_VALUE);

    struct {
        int64_t address;
        int32_t entries;
        uint32_t flags, fence_id, fence_value;
    } sx = { 0x1000, 2, 2, 0, 0 };
    uint64_t list[2] = { 1, 2 };
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 11, gpu, IOWR(0x48, 0x1B, 24), &sx, 24, 24, list, 16), 0);
    CHECK_EQ(sx.fence_id, gpu_sync);
    CHECK_EQ(sx.fence_value, 7);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 11, gpu, IOWR(0x48, 0x1B, 24), &sx, 24, 24, list, 8), NVR_INVALID_VALUE);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 11, vic, IOWR(0x48, 0x1B, 24), &sx, 24, 24, list, 16), NVR_IOCTL_FAILED);

    int32_t m1 = map_new(&t, nvmap, 0x3000, t.heap + BUF_AT, 0x1000, 0);
    int32_t m2 = map_new(&t, nvmap, 0x2000, t.heap + BUF_AT + 0x10000, 0x1000, 0);
    CHECK(m1 > 0 && m2 > 0);
    uint8_t mcb[12 + 16];
    memset(mcb, 0, sizeof mcb);
    int32_t n = 2;
    memcpy(mcb, &n, 4);
    memcpy(mcb + 12, &m1, 4);
    memcpy(mcb + 20, &m2, 4);
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 9, 28), mcb), 0);
    int32_t a1, a2;
    memcpy(&a1, mcb + 16, 4);
    memcpy(&a2, mcb + 24, 4);
    CHECK_EQ(a1, 0x1000);
    CHECK_EQ(a2, 0x4000);
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 9, 28), mcb), 0);
    memcpy(&a1, mcb + 16, 4);
    CHECK_EQ(a1, 0x1000);
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 0x0A, 28), mcb), 0);
    int32_t bad_handle = 123;
    memcpy(mcb + 20, &bad_handle, 4);
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 9, 28), mcb), NVR_INVALID_VALUE);
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 0x0A, 28), mcb), NVR_INVALID_VALUE);

    uint8_t sub[16 + 12 + 20 + 4];
    memset(sub, 0, sizeof sub);
    int32_t hdr[4] = { 1, 0, 1, 1 };
    memcpy(sub, hdr, 16);
    uint32_t cmdbuf[3] = { (uint32_t)m1, 0, 4 };
    memcpy(sub + 16, cmdbuf, 12);
    uint32_t incr[5] = { gpu_sync, 3, 0, 0, 0 };
    memcpy(sub + 28, incr, 20);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0, 1, 52), sub), 0);
    uint32_t thr;
    memcpy(&thr, sub + 48, 4);
    CHECK_EQ(thr, 10);
    f[0] = gpu_sync;
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x1A, 8), f), 0);
    CHECK_EQ(f[1], 10);
    CHECK_EQ(nv_ioctl(&t, ctrl, IOWR(0, 0x14, 8), f), 0);
    CHECK_EQ(f[1], 10);
    incr[0] = 500;
    memcpy(sub + 28, incr, 20);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0, 1, 52), sub), NVR_INVALID_VALUE);
    incr[0] = gpu_sync;
    memcpy(sub + 28, incr, 20);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0, 1, 40), sub), NVR_INVALID_VALUE);
    cmdbuf[0] = 4000;
    memcpy(sub + 16, cmdbuf, 12);
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0, 1, 52), sub), NVR_INVALID_VALUE);

    CHECK_EQ(nv_close(&t, t.o, gpu), 0);
    int32_t gpu2 = nv_fd(&t, "/dev/nvhost-gpu");
    CHECK_EQ(gpu2, gpu);
    CHECK_EQ(nv_ioctl(&t, gpu2, IOWR(0x48, 0x1A, 32), &gf), 0);
    CHECK_EQ(gf.fence_id, gpu_sync);
    CHECK_EQ(gf.fence_value, 10);
}

TEST(nv_ioctl_argument_checks)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t nvmap = nv_fd(&t, "/dev/nvmap");
    CHECK_EQ(nvmap, 1);
    map_create cr = { 0x1000, 0 };
    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    memcpy(buf, &cr, 8);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, nvmap, IOWR(1, 1, 8), buf, 8, 4, NULL, 0), NVR_INVALID_SIZE);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, nvmap, IOWR(1, 1, 8), buf, 4, 8, NULL, 0), NVR_INVALID_SIZE);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, nvmap, IOWR(1, 1, 8), buf, 16, 8, NULL, 0), NVR_INVALID_SIZE);
    memcpy(buf, &cr, 8);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, nvmap, IOWR(1, 1, 8), buf, 8, 16, NULL, 0), 0);
    int32_t handle;
    memcpy(&handle, buf + 4, 4);
    CHECK_EQ(handle, 4);

    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, 99, IOWR(1, 1, 8), buf, 8, 8, NULL, 0), NVR_NOT_IMPLEMENTED);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, -1, IOWR(1, 1, 8), buf, 8, 8, NULL, 0), NVR_INVALID_PARAMETER);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, 0, IOWR(1, 1, 8), buf, 8, 8, NULL, 0), NVR_NOT_IMPLEMENTED);

    memset(buf, 0xAB, 16);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 1, nvmap, IOWR(0x99, 1, 8), buf, 8, 8, NULL, 0), NVR_IOCTL_FAILED);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 11, nvmap, IOWR(1, 1, 8), buf, 8, 8, buf + 8, 8), NVR_IOCTL_FAILED);
    CHECK_EQ(nv_ioctl_raw(&t, t.o, 12, nvmap, IOWR(1, 1, 8), buf, 8, 8, buf + 8, 8), NVR_IOCTL_FAILED);
}

TEST(nv_sdk_graphics_bringup)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", false));
    uint32_t tm = nv_tmem(&t);
    CHECK(tm);
    CHECK_EQ(nv_init_with(&t, t.o, HOS_CUR_PROCESS, tm, 0x800000), 0);
    int32_t nvmap = nv_fd(&t, "/dev/nvmap");
    int32_t ctrl = nv_fd(&t, "/dev/nvhost-ctrl");
    int32_t cgpu = nv_fd(&t, "/dev/nvhost-ctrl-gpu");
    int32_t as = nv_fd(&t, "/dev/nvhost-as-gpu");
    int32_t gpu = nv_fd(&t, "/dev/nvhost-gpu");
    CHECK(nvmap == 1 && ctrl == 2 && cgpu == 3 && as == 4 && gpu == 5);

    uint8_t chars[0xB0];
    memset(chars, 0, sizeof chars);
    CHECK_EQ(nv_ioctl(&t, cgpu, IOWR(0x47, 5, 0xB0), chars), 0);
    int32_t arch;
    memcpy(&arch, chars + 16, 4);
    CHECK_EQ(arch, 0x120);

    uint32_t init[10] = { 0, (uint32_t)as, 0x20000, 0 };
    CHECK_EQ(nv_ioctl(&t, as, IOW(0x41, 9, 40), init), 0);
    struct {
        uint32_t pages, page_size, flags, pad;
        uint64_t offset;
    } space = { 0x400, 0x10000, 0, 0, 0x10000 };
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 2, 24), &space), 0);
    uint64_t reserved = space.offset;
    CHECK_EQ(reserved, 0x100000000ull);

    const uint32_t gb_size = 0x1C2000;
    uint64_t gb_addr = t.heap + BUF_AT;
    int32_t h = map_new(&t, nvmap, gb_size, gb_addr, 0x20000, 0xFE);
    CHECK(h > 0);
    map_id gi = { 0, h };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 0x0E, 8), &gi), 0);

    struct {
        uint32_t flags;
        int32_t kind, handle, page_size;
        uint64_t buffer_offset, mapping_size, offset;
    } mb = { 1, 0xFE, h, 0x20000, 0, 0, reserved };
    CHECK_EQ(nv_ioctl(&t, as, IOWR(0x41, 6, 40), &mb), 0);
    CHECK_EQ(mb.offset, reserved);

    int32_t chan_nvmap = nvmap;
    CHECK_EQ(nv_ioctl(&t, gpu, IOW(0x48, 1, 4), &chan_nvmap), 0);
    struct {
        int32_t fd;
    } bind = { gpu };
    CHECK_EQ(nv_ioctl(&t, as, IOW(0x41, 1, 4), &bind), 0);
    struct {
        uint32_t entries, jobs, flags, fence_id, fence_value, r[3];
    } gf = { 0x800, 1, 0, 0, 0, { 0 } };
    CHECK_EQ(nv_ioctl(&t, gpu, IOWR(0x48, 0x1A, 32), &gf), 0);
    CHECK(gf.fence_id != 0);
    uint32_t err_event;
    CHECK_EQ(nv_event(&t, gpu, 3, &err_event), 0);
    CHECK(err_event);

    rl_nv_map_info info;
    CHECK(rl_nv_map_lookup((uint32_t)gi.id, &info));
    CHECK_EQ(info.addr, gb_addr);
    CHECK_EQ(info.size, gb_size);
    CHECK_EQ(info.kind, 0xFE);
    CHECK_EQ(info.alignment, 0x20000);

    CHECK_EQ(nv_close(&t, t.o, gpu), 0);
    CHECK_EQ(nv_close(&t, t.o, as), 0);
    CHECK_EQ(nv_close(&t, t.o, cgpu), 0);
    CHECK_EQ(nv_close(&t, t.o, ctrl), 0);
    map_free fr = { h, 0, 0, 0, 0 };
    CHECK_EQ(nv_ioctl(&t, nvmap, IOWR(1, 5, 0x18), &fr), 0);
    CHECK_EQ(fr.address, gb_addr);
    CHECK(!rl_nv_map_lookup((uint32_t)gi.id, &info));
    CHECK_EQ(nv_close(&t, t.o, nvmap), 0);
    tc_close(t.c, tm);
}

TEST(nv_va_allocator)
{
    nv_va a;
    nv_va_init(&a, 0x1000, 0x100000);
    uint64_t at;
    CHECK(nv_va_find(&a, 0x4000, 0x2000, 0x10000, &at));
    CHECK_EQ(at, 0x10000);
    CHECK(nv_va_take(&a, at, 0x4000));
    CHECK(!nv_va_take(&a, 0x12000, 0x1000));
    CHECK(!nv_va_is_free(&a, 0x12000, 0x1000));
    CHECK(nv_va_is_free(&a, 0x14000, 0x1000));
    CHECK(nv_va_find(&a, 0x1000, 1, 0x10000, &at));
    CHECK_EQ(at, 0x14000);
    CHECK(nv_va_find(&a, 0xF0000, 1, 0x10000, &at) == false);
    CHECK(nv_va_find(&a, 0xA000, 1, 0x20000, &at));
    CHECK_EQ(at, 0x20000);
    CHECK(nv_va_take(&a, 0x18000, 0x1000));
    CHECK(nv_va_find(&a, 0x8000, 0x1000, 0x10000, &at));
    CHECK_EQ(at, 0x19000);
    nv_va_put(&a, 0x10000, 0x4000);
    CHECK(nv_va_is_free(&a, 0x10000, 0x8000));
    nv_va_put(&a, 0x18000, 0x1000);
    CHECK(nv_va_is_free(&a, 0x1000, 0xFF000));
    CHECK(!nv_va_take(&a, 0xFF000, 0x2000));
    CHECK(nv_va_take(&a, 0x1000, 0xFF000));
    CHECK(!nv_va_find(&a, 0x1000, 1, 0, &at));
    nv_va_destroy(&a);
}

/** @brief Offset of byte x of row y in a block-linear surface one GOB wide with two-GOB blocks. */
static uint32_t bl_at(uint32_t x, uint32_t y)
{
    uint32_t gob = (y / 16) * 2 + (y / 8) % 2;
    return gob * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}

static void put_bits(uint8_t *s, uint32_t lo, uint32_t n, uint64_t v)
{
    for (uint32_t i = 0; i < n; i++)
        if ((v >> i) & 1)
            s[(lo + i) / 8] |= (uint8_t)(1u << ((lo + i) % 8));
}

TEST(nv_vic_copies_nv12_to_pitch_linear)
{
    nvt t;
    CHECK(nv_boot(&t, "nvdrv", true));
    int32_t nvmap = nv_fd(&t, "/dev/nvmap");
    int32_t vic = nv_fd(&t, "/dev/nvhost-vic");
    CHECK(nvmap > 0 && vic > 0);
    enum { W = 64, H = 16, CMD = 0, CFG = 1, SY = 2, SC = 3, DY = 4, DC = 5, N = 6 };
    int32_t h[N];
    uint32_t dma[N];
    uint8_t *m = t_mem() + t.heap;
    for (int i = 0; i < N; i++) {
        h[i] = map_new(&t, nvmap, 0x1000, t.heap + BUF_AT + (uint64_t)i * 0x1000, 0x1000, 0);
        CHECK(h[i] > 0);
        uint8_t mcb[12 + 8];
        memset(mcb, 0, sizeof mcb);
        int32_t one = 1;
        memcpy(mcb, &one, 4);
        memcpy(mcb + 12, &h[i], 4);
        CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 9, 20), mcb), 0);
        memcpy(&dma[i], mcb + 16, 4);
        memset(m + BUF_AT + (size_t)i * 0x1000, 0, 0x1000);
    }
    uint8_t *sy = m + BUF_AT + SY * 0x1000, *sc = m + BUF_AT + SC * 0x1000;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            sy[bl_at(x, y)] = (uint8_t)(x * 3 + y * 7);
    for (uint32_t y = 0; y < H / 2; y++)
        for (uint32_t x = 0; x < W; x++)
            sc[bl_at(x, y)] = (uint8_t)(200 - x - y * 5);
    CHECK_EQ(bl_at(16, 0), 32);
    CHECK_EQ(bl_at(0, 1), 16);
    CHECK_EQ(bl_at(32, 0), 256);
    CHECK_EQ(bl_at(0, 8), 512);

    uint8_t *cfg = m + BUF_AT + CFG * 0x1000;
    put_bits(cfg + 0x10, 80, 14, W - 1);
    put_bits(cfg + 0x10, 112, 14, H - 1);
    uint8_t *osc = cfg + 0x20;
    put_bits(osc, 0, 7, 68);
    put_bits(osc, 32, 14, W - 1);
    put_bits(osc, 46, 14, H - 1);
    put_bits(osc, 64, 14, W - 1);
    put_bits(osc, 78, 14, H - 1);
    put_bits(osc, 96, 14, W / 2 - 1);
    put_bits(osc, 110, 14, H / 2 - 1);
    uint8_t *slot = cfg + 0x90;
    put_bits(slot, 0, 1, 1);
    put_bits(slot, 138, 10, 1023);
    uint8_t *ssc = slot + 0x40;
    put_bits(ssc, 0, 7, 68);
    put_bits(ssc, 11, 4, 1);
    put_bits(ssc, 15, 4, 1);
    put_bits(ssc, 64, 14, W - 1);
    put_bits(ssc, 78, 14, H - 1);
    put_bits(ssc, 96, 14, W / 2 - 1);
    put_bits(ssc, 110, 14, H / 2 - 1);

    uint32_t *cmd = (uint32_t *)(void *)(m + BUF_AT + CMD * 0x1000);
    uint32_t n = 0;
    cmd[n++] = (0x5Du << 6);
    const uint32_t writes[][2] = {
        { 0x1C2, dma[CFG] >> 8 }, { 0x100, dma[SY] >> 8 }, { 0x101, dma[SC] >> 8 },
        { 0x1C8, dma[DY] >> 8 },  { 0x1C9, dma[DC] >> 8 },
    };
    for (size_t i = 0; i < sizeof writes / sizeof writes[0]; i++) {
        cmd[n++] = (1u << 28) | (0x10u << 16) | 2;
        cmd[n++] = writes[i][0];
        cmd[n++] = writes[i][1];
    }
    cmd[n++] = (3u << 28) | (0x10u << 16) | 3;
    cmd[n++] = 0xC0;
    cmd[n++] = 0;

    uint32_t gp[2] = { 0, 0 };
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 2, 8), gp), 0);
    uint8_t sub[16 + 12 + 20 + 4];
    memset(sub, 0, sizeof sub);
    int32_t hdr[4] = { 1, 0, 1, 1 };
    memcpy(sub, hdr, 16);
    uint32_t cmdbuf[3] = { (uint32_t)h[CMD], 0, n };
    memcpy(sub + 16, cmdbuf, 12);
    uint32_t incr[5] = { gp[1], 1, 0, 0, 0 };
    memcpy(sub + 28, incr, 20);
    CHECK_EQ(nv_ioctl(&t, vic, IOWR(0, 1, 52), sub), 0);

    const uint8_t *dy = m + BUF_AT + DY * 0x1000, *dc = m + BUF_AT + DC * 0x1000;
    bool same = true;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            same = same && dy[y * 256 + x] == (uint8_t)(x * 3 + y * 7);
    for (uint32_t y = 0; y < H / 2; y++)
        for (uint32_t x = 0; x < W; x++)
            same = same && dc[y * 256 + x] == (uint8_t)(200 - x - y * 5);
    CHECK(same);
}
