/**
 * @file srv_nv.c
 * @brief nvdrv, nvdrv:a, nvdrv:s and nvdrv:t: the session, the file
 * descriptor table, ioctl argument handling and the device registry.
 *
 * Command ids, argument handling, result conversion and the checks follow
 * Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors)
 * INvDrvServices. Descriptors are per session; a cloned session shares them.
 * The devices are in srv_nv_map.c, srv_nv_ctrl.c, srv_nv_gpu.c, srv_nv_as.c
 * and srv_nv_ch.c.
 */
#include "nv.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define NV_MAX_ARG (1u << 20)
#define NV_MAX_FDS 4096u
#define NV_PATH_MAX 128u
#define STATUS_SIZE 0x20u

struct nv_session {
    uint64_t owner;
    uint64_t aruid;
    nv_file **files;
    uint32_t cap;
};

static pthread_mutex_t nv_mu = PTHREAD_MUTEX_INITIALIZER;
static bool debug_mode;

static const nv_dev_class *const devices[] = {
    &nv_dev_nvmap, &nv_dev_ctrl, &nv_dev_ctrl_gpu, &nv_dev_as_gpu, &nv_dev_gpu,     &nv_dev_nvdec,
    &nv_dev_vic,   &nv_dev_nvjpg, &nv_dev_nvenc,   &nv_dev_dbg_gpu, &nv_dev_prof_gpu,
};

void nv_lock(void) { pthread_mutex_lock(&nv_mu); }
void nv_unlock(void) { pthread_mutex_unlock(&nv_mu); }

void rl_nv_set_debug_mode(bool on)
{
    nv_lock();
    debug_mode = on;
    nv_unlock();
}

void rl_nv_reset(void)
{
    nv_lock();
    nv_map_reset();
    nv_sync_reset();
    nv_ch_reset();
    debug_mode = false;
    nv_unlock();
}

static uint32_t convert_error(int32_t e)
{
    switch (e) {
    case NVE_OK: return NVR_SUCCESS;
    case NVE_UNKNOWN_0x72: return NVR_ALREADY_ALLOCATED;
    case NVE_TIMED_OUT:
    case NVE_TRY_AGAIN:
    case NVE_INTERRUPTED: return NVR_TIMEOUT;
    case NVE_INVALID_ADDRESS: return NVR_INVALID_ADDRESS;
    case NVE_NOT_SUPPORTED:
    case NVE_UNKNOWN_0x18: return NVR_NOT_SUPPORTED;
    case NVE_INVALID_STATE: return NVR_INVALID_STATE;
    case NVE_READ_ONLY_ATTRIBUTE: return NVR_READ_ONLY_ATTRIBUTE;
    case NVE_NO_SPACE_LEFT:
    case NVE_FILE_TOO_BIG: return NVR_INVALID_SIZE;
    case NVE_FILE_TABLE_OVERFLOW:
    case NVE_BAD_FILE_NUMBER: return NVR_FILE_OPERATION_FAILED;
    case NVE_INVALID_INPUT: return NVR_INVALID_VALUE;
    case NVE_NOT_A_DIRECTORY: return NVR_DIRECTORY_OPERATION_FAILED;
    case NVE_BUSY: return NVR_BUSY;
    case NVE_BAD_ADDRESS: return NVR_INVALID_ADDRESS;
    case NVE_ACCESS_DENIED:
    case NVE_NOT_PERMITTED: return NVR_ACCESS_DENIED;
    case NVE_OUT_OF_MEMORY: return NVR_INSUFFICIENT_MEMORY;
    case NVE_DEVICE_NOT_FOUND: return NVR_MODULE_NOT_PRESENT;
    case NVE_IO_ERROR: return NVR_RESOURCE_ERROR;
    default: return NVR_IOCTL_FAILED;
    }
}

static nv_session *session_of(rl_ipc_req *r) { return r->self->state; }

nv_file *nv_file_get(nv_session *s, int32_t fd)
{
    if (fd <= 0 || (uint32_t)fd >= s->cap)
        return NULL;
    return s->files[fd];
}

static uint32_t file_lookup(nv_session *s, int32_t fd, nv_file **out)
{
    *out = NULL;
    if (fd < 0)
        return NVR_INVALID_PARAMETER;
    nv_file *f = nv_file_get(s, fd);
    if (!f)
        return NVR_NOT_IMPLEMENTED;
    *out = f;
    return NVR_SUCCESS;
}

static uint32_t ensure_initialized(const nv_session *s)
{
    return s->owner == 0 ? NVR_NOT_INITIALIZED : NVR_SUCCESS;
}

static void file_release(nv_session *s, nv_file *f)
{
    if (f->dev->close)
        f->dev->close(f);
    s->files[f->fd] = NULL;
    free(f);
}

static int32_t fd_alloc(nv_session *s)
{
    for (uint32_t i = 1; i < s->cap; i++)
        if (!s->files[i])
            return (int32_t)i;
    if (s->cap >= NV_MAX_FDS)
        return -1;
    uint32_t cap = s->cap ? s->cap * 2 : 16;
    nv_file **p = realloc(s->files, cap * sizeof *p);
    if (!p)
        return -1;
    memset(p + s->cap, 0, (cap - s->cap) * sizeof *p);
    uint32_t first = s->cap ? s->cap : 1;
    s->files = p;
    s->cap = cap;
    return (int32_t)first;
}

static uint32_t open_path(nv_session *s, const char *path, int32_t *fd)
{
    *fd = -1;
    const nv_dev_class *dev = NULL;
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        if (strcmp(devices[i]->path, path) == 0) {
            dev = devices[i];
            break;
        }
    }
    if (!dev)
        return NVR_FILE_OPERATION_FAILED;
    if (dev->debug_only && !debug_mode)
        return NVR_NOT_SUPPORTED;
    int32_t id = fd_alloc(s);
    if (id < 0)
        return NVR_INSUFFICIENT_MEMORY;
    nv_file *f = calloc(1, sizeof *f);
    if (!f)
        return NVR_INSUFFICIENT_MEMORY;
    f->dev = dev;
    f->sess = s;
    f->fd = id;
    int32_t rc = dev->open ? dev->open(f) : NVE_OK;
    if (rc != NVE_OK) {
        free(f);
        return convert_error(rc);
    }
    s->files[id] = f;
    *fd = id;
    return NVR_SUCCESS;
}

static void session_destroy(void *state)
{
    nv_session *s = state;
    nv_lock();
    for (uint32_t i = 1; i < s->cap; i++)
        if (s->files[i])
            file_release(s, s->files[i]);
    nv_unlock();
    free(s->files);
    free(s);
}

static rl_result put_error(rl_ipc_req *r, uint32_t err)
{
    uint32_t *o = rl_ipc_out(r, 4);
    *o = err;
    return KR_SUCCESS;
}

static rl_result cmd_open(rl_ipc_req *r)
{
    nv_session *s = session_of(r);
    char path[NV_PATH_MAX];
    int32_t fd = -1;
    memset(path, 0, sizeof path);
    nv_lock();
    uint32_t err = ensure_initialized(s);
    if (err == NVR_SUCCESS) {
        rl_ipc_buf b = rl_ipc_auto_in(r, 0);
        uint64_t n = b.size < NV_PATH_MAX - 1 ? b.size : NV_PATH_MAX - 1;
        if (!rl_ipc_buf_read(&b, path, n))
            memset(path, 0, sizeof path);
        err = open_path(s, path, &fd);
        if (rl_cfg()->trace_os)
            rl_log(RL_LOG_INFO, "nvdrv", "Open(\"%s\") = fd %d, error %u", path, fd, err);
        if (err == NVR_FILE_OPERATION_FAILED && hos_switch2())
            rl_ipc_stop(r, "nvdrv", "Open(\"%s\"): the device is not answered yet", path);
    }
    nv_unlock();
    struct {
        int32_t fd;
        uint32_t error;
    } *o = rl_ipc_out(r, 8);
    o->fd = fd;
    o->error = err;
    return KR_SUCCESS;
}

static const nv_ioctl_entry *find_ioctl(const nv_dev_class *dev, uint32_t cmd, uint32_t variant)
{
    uint8_t type = (uint8_t)(cmd >> 8), nr = (uint8_t)cmd;
    for (uint32_t i = 0; i < dev->nioctls; i++) {
        const nv_ioctl_entry *e = &dev->ioctls[i];
        if (e->type == type && e->nr == nr && (e->variants & variant))
            return e;
    }
    return NULL;
}

static rl_result ioctl_common(rl_ipc_req *r, uint32_t variant)
{
    nv_session *s = session_of(r);
    struct {
        int32_t fd;
        uint32_t cmd;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    nv_lock();
    uint32_t err = ensure_initialized(s);
    uint8_t *data = NULL, *inl = NULL;
    uint32_t len = 0, inl_len = 0;
    rl_ipc_buf in_buf = rl_ipc_auto_in(r, 0), out_buf = rl_ipc_auto_out(r, 0), inl_buf = { 0, 0, 0 };
    bool is_read = (in.cmd >> 30) & 1, is_write = (in.cmd >> 30) & 2;
    uint32_t arg_size = (in.cmd >> 16) & 0x3FFF;
    if (err == NVR_SUCCESS) {
        if (variant == NV_V_IOCTL2)
            inl_buf = rl_ipc_auto_in(r, 1);
        else if (variant == NV_V_IOCTL3)
            inl_buf = rl_ipc_auto_out(r, 1);
        if ((is_write && arg_size > out_buf.size) || (is_read && arg_size > in_buf.size) ||
            (is_read && is_write && out_buf.size < in_buf.size))
            err = NVR_INVALID_SIZE;
        else if (inl_buf.size > NV_MAX_ARG || (is_write ? out_buf.size : in_buf.size) > NV_MAX_ARG)
            err = NVR_INVALID_SIZE;
    }
    if (err == NVR_SUCCESS) {
        len = (uint32_t)(is_write ? out_buf.size : in_buf.size);
        inl_len = (uint32_t)inl_buf.size;
        data = calloc(1, len ? len : 1);
        inl = calloc(1, inl_len ? inl_len : 1);
        if (!data || !inl)
            err = NVR_INSUFFICIENT_MEMORY;
    }
    if (err == NVR_SUCCESS && (is_read || !is_write) && !rl_ipc_buf_read(&in_buf, data, in_buf.size))
        err = NVR_INVALID_ADDRESS;
    if (err == NVR_SUCCESS && variant == NV_V_IOCTL2 && !rl_ipc_buf_read(&inl_buf, inl, inl_buf.size))
        err = NVR_INVALID_ADDRESS;
    nv_file *f = NULL;
    if (err == NVR_SUCCESS)
        err = file_lookup(s, in.fd, &f);
    if (err == NVR_SUCCESS) {
        const nv_ioctl_entry *e = find_ioctl(f->dev, in.cmd, variant);
        nv_args a = { data, len, variant == NV_V_IOCTL ? NULL : inl, inl_len, variant, in.cmd, NULL };
        int32_t rc = NVE_NOT_IMPLEMENTED;
        if (!e && hos_switch2())
            rl_ipc_stop(r, "nvdrv", "%s ioctl 0x%08x (type 0x%02x nr 0x%02x, %u bytes, %s) is not answered yet",
                        f->dev->path, in.cmd, (in.cmd >> 8) & 0xff, in.cmd & 0xff, arg_size,
                        variant == NV_V_IOCTL ? "Ioctl" : variant == NV_V_IOCTL2 ? "Ioctl2" : "Ioctl3");
        if (e)
            rc = a.size < e->min_size ? NVE_INVALID_INPUT : e->fn(f, &a);
        if (a.stop)
            rl_ipc_stop(r, "nvdrv", "%s", a.stop);
        err = rc == NVE_NOT_IMPLEMENTED ? NVR_IOCTL_FAILED : convert_error(rc);
        if (rl_cfg()->trace_os)
            rl_log(RL_LOG_INFO, "nvdrv", "%s ioctl 0x%08x %s (%u bytes, inline %u) -> error %u", f->dev->path, in.cmd,
                   e ? e->name : "(unknown)", len, inl_len, err);
        if (rc != NVE_NOT_IMPLEMENTED && is_write) {
            bool ok = rl_ipc_buf_write(&out_buf, data, len);
            if (variant == NV_V_IOCTL3)
                ok = ok && rl_ipc_buf_write(&inl_buf, inl, inl_len);
            if (!ok && err == NVR_SUCCESS)
                err = NVR_INVALID_ADDRESS;
        }
    }
    nv_unlock();
    free(data);
    free(inl);
    return put_error(r, err);
}

static rl_result cmd_ioctl(rl_ipc_req *r) { return ioctl_common(r, NV_V_IOCTL); }
static rl_result cmd_ioctl2(rl_ipc_req *r) { return ioctl_common(r, NV_V_IOCTL2); }
static rl_result cmd_ioctl3(rl_ipc_req *r) { return ioctl_common(r, NV_V_IOCTL3); }

static rl_result cmd_close(rl_ipc_req *r)
{
    nv_session *s = session_of(r);
    int32_t fd;
    if (!rl_srv_in(r, &fd, 4))
        return SF_RESULT_INVALID_HEADER;
    nv_lock();
    uint32_t err = ensure_initialized(s);
    nv_file *f = NULL;
    if (err == NVR_SUCCESS)
        err = file_lookup(s, fd, &f);
    if (err == NVR_SUCCESS)
        file_release(s, f);
    nv_unlock();
    return put_error(r, err);
}

static rl_result cmd_initialize(rl_ipc_req *r)
{
    nv_session *s = session_of(r);
    uint32_t transfer_size;
    if (!rl_srv_in(r, &transfer_size, 4))
        return SF_RESULT_INVALID_HEADER;
    uint32_t err = NVR_SUCCESS;
    if (r->n_in_copy < 2 || r->in_copy[0] != HOS_CUR_PROCESS) {
        err = NVR_INVALID_PARAMETER;
    } else {
        hos_lock();
        hos_kobj *tm = hos_handle_get(r->in_copy[1], KOBJ_TRANSFER_MEMORY);
        hos_unlock();
        if (!tm)
            err = NVR_INVALID_PARAMETER;
        else
            hos_kobj_unref(tm);
    }
    if (err == NVR_SUCCESS) {
        nv_lock();
        s->owner = HOS_PID;
        nv_unlock();
    }
    return put_error(r, err);
}

static rl_result cmd_query_event(rl_ipc_req *r)
{
    nv_session *s = session_of(r);
    struct {
        int32_t fd;
        uint32_t id;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    nv_lock();
    uint32_t err = ensure_initialized(s);
    nv_file *f = NULL;
    hos_revent *ev = NULL;
    if (err == NVR_SUCCESS)
        err = file_lookup(s, in.fd, &f);
    if (err == NVR_SUCCESS) {
        int32_t rc = f->dev->query_event ? f->dev->query_event(f, in.id, &ev) : NVE_NOT_IMPLEMENTED;
        err = rc == NVE_NOT_IMPLEMENTED ? NVR_IOCTL_FAILED : convert_error(rc);
    }
    rl_result rc = KR_SUCCESS;
    if (err == NVR_SUCCESS)
        rc = rl_ipc_push_copy(r, &ev->obj);
    nv_unlock();
    if (rc != KR_SUCCESS)
        return rc;
    return put_error(r, err);
}

static rl_result cmd_map_shared_memory(rl_ipc_req *r)
{
    nv_session *s = session_of(r);
    struct {
        int32_t fd;
        uint32_t argument;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    nv_lock();
    uint32_t err = ensure_initialized(s);
    nv_file *f = NULL;
    if (err == NVR_SUCCESS)
        err = file_lookup(s, in.fd, &f);
    if (err == NVR_SUCCESS)
        err = NVR_IOCTL_FAILED;
    nv_unlock();
    return put_error(r, err);
}

static rl_result cmd_get_status(rl_ipc_req *r)
{
    nv_session *s = session_of(r);
    nv_lock();
    uint32_t err = ensure_initialized(s);
    nv_unlock();
    uint8_t *o = rl_ipc_out(r, STATUS_SIZE + 4);
    memcpy(o + STATUS_SIZE, &err, 4);
    return KR_SUCCESS;
}

static rl_result cmd_set_client_pid(rl_ipc_req *r)
{
    nv_session *s = session_of(r);
    uint64_t aruid;
    if (!rl_srv_in(r, &aruid, 8))
        return SF_RESULT_INVALID_HEADER;
    nv_lock();
    s->aruid = aruid;
    nv_unlock();
    return put_error(r, NVR_SUCCESS);
}

static const rl_ipc_cmd nv_cmds[] = {
    { 0, "Open", cmd_open },
    { 1, "Ioctl", cmd_ioctl },
    { 2, "Close", cmd_close },
    { 3, "Initialize", cmd_initialize },
    { 4, "QueryEvent", cmd_query_event },
    { 5, "MapSharedMemory", cmd_map_shared_memory },
    { 6, "GetStatus", cmd_get_status },
    { 7, "ForceSetClientPid*", cmd_set_client_pid },
    { 8, "SetClientPID", cmd_set_client_pid },
    RL_SRV_STUB(9, "DumpGraphicsMemoryInfo"),
    { 11, "Ioctl2", cmd_ioctl2 },
    { 12, "Ioctl3", cmd_ioctl3 },
    RL_SRV_STUB(13, "FinishInitialize"),
};

static const rl_ipc_class nv_class = {
    .name = "nvdrv",
    .cmds = nv_cmds,
    .ncmds = sizeof nv_cmds / sizeof nv_cmds[0],
    .pointer_buffer_size = 0,
    .destroy = session_destroy,
};

static rl_ipc_object *nv_open_session(void)
{
    nv_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    rl_ipc_object *o = rl_srv_object(&nv_class, s);
    if (!o)
        free(s);
    return o;
}

/**
 * @brief nn::nvdrv::INvDrv2User, the Switch 2's nvdrv session from gsv
 * (CreateNvDrv2UserSession*): the nvdrv commands without Initialize, which
 * the supervisor's boot replaces, and SetConfiguration (13), whose bit 0 is
 * the graphics firmware memory margin.
 */
static const rl_ipc_cmd nv2_cmds[] = {
    { 0, "Open", cmd_open },
    { 1, "Ioctl", cmd_ioctl },
    { 2, "Close", cmd_close },
    { 4, "QueryEvent", cmd_query_event },
    RL_SRV_STUB(9, "DumpStatus"),
    RL_SRV_STUB(10, "InitializeDevtools"),
    { 11, "Ioctl2", cmd_ioctl2 },
    { 12, "Ioctl3", cmd_ioctl3 },
    RL_SRV_STUB(13, "SetConfiguration"),
};

static const rl_ipc_class nv2_class = {
    .name = "nn::nvdrv::INvDrv2User",
    .cmds = nv2_cmds,
    .ncmds = sizeof nv2_cmds / sizeof nv2_cmds[0],
    .pointer_buffer_size = 0,
    .destroy = session_destroy,
};

rl_result rl_srv_nv2_open(uint32_t *handle)
{
    nv_session *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    s->owner = HOS_PID;
    rl_ipc_object *o = rl_srv_object(&nv2_class, s);
    if (!o) {
        free(s);
        return KR_OUT_OF_RESOURCE;
    }
    rl_result rc = rl_ipc_session_open(o, handle);
    rl_ipc_object_unref(o);
    return rc;
}

void rl_srv_install_nv(void)
{
    rl_ipc_register_service("nvdrv", nv_open_session);
    rl_ipc_register_service("nvdrv:a", nv_open_session);
    rl_ipc_register_service("nvdrv:s", nv_open_session);
    rl_ipc_register_service("nvdrv:t", nv_open_session);
}
