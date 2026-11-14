/**
 * @file fs_romfs.c
 * @brief The data storages of fsp-srv: the program's RomFS and add-on content
 * as read-only IStorage objects over synthesized RomFS images.
 *
 * Commands 200 to 203 of IFileSystemProxy and the IStorage commands follow
 * switchbrew.org (Filesystem services) and libnx (ISC) fs_sf.h, read as
 * specifications.
 */
#include "fs_priv.h"
#include "hos.h"
#include "hos_fs.h"
#include "romfs_image.h"
#include "services.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Largest piece moved between an image and guest memory in one step. */
#define STORAGE_CHUNK 0x100000u

/** @brief One image opened on first use. */
typedef struct source {
    uint64_t id;
    char *dir;
    rl_romfs_image *img;
    int err;
    bool tried;
} source;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static source g_main;
static char *g_addon_root;
static source **g_addons;
static size_t g_naddons;

static void source_clear(source *s)
{
    rl_romfs_image_close(s->img);
    free(s->dir);
    memset(s, 0, sizeof *s);
}

static char *dup_dir(const char *dir) { return dir && *dir ? strdup(dir) : NULL; }

void rl_hos_fs_set_romfs(const char *dir)
{
    pthread_mutex_lock(&g_lock);
    source_clear(&g_main);
    g_main.dir = dup_dir(dir);
    pthread_mutex_unlock(&g_lock);
}

void rl_hos_fs_set_addons(const char *dir)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_naddons; i++) {
        source_clear(g_addons[i]);
        free(g_addons[i]);
    }
    free(g_addons);
    g_addons = NULL;
    g_naddons = 0;
    free(g_addon_root);
    g_addon_root = dup_dir(dir);
    pthread_mutex_unlock(&g_lock);
}

void rl_fs_romfs_env(void)
{
    const char *romfs = getenv("RELIFT_ROMFS");
    const char *dlc = getenv("RELIFT_DLC");
    rl_hos_fs_set_romfs(romfs);
    rl_hos_fs_set_addons(dlc);
}

static rl_result open_error(int e) { return e == ENOENT || e == ENOTDIR ? FS_TARGET_NOT_FOUND : FS_UNEXPECTED; }

static rl_romfs_image *source_image(source *s, rl_result *rc)
{
    if (!s->dir) {
        *rc = FS_TARGET_NOT_FOUND;
        return NULL;
    }
    if (!s->tried) {
        s->tried = true;
        s->img = rl_romfs_image_open(s->dir);
        s->err = s->img ? 0 : errno;
    }
    if (!s->img)
        *rc = open_error(s->err);
    return s->img;
}

static source *addon_source(uint64_t id)
{
    char dir[PATH_MAX];
    source **grown;
    source *s;
    if (!g_addon_root)
        return NULL;
    for (size_t i = 0; i < g_naddons; i++)
        if (g_addons[i]->id == id)
            return g_addons[i];
    if (snprintf(dir, sizeof dir, "%s/%016llx/0/romfs", g_addon_root, (unsigned long long)id) >= (int)sizeof dir)
        return NULL;
    s = calloc(1, sizeof *s);
    grown = s ? realloc(g_addons, (g_naddons + 1) * sizeof *grown) : NULL;
    if (!grown || !(s->dir = strdup(dir))) {
        free(s);
        return NULL;
    }
    s->id = id;
    g_addons = grown;
    g_addons[g_naddons++] = s;
    return s;
}

static rl_result storage_read(rl_ipc_req *r);
static rl_result storage_write(rl_ipc_req *r);
static rl_result storage_flush(rl_ipc_req *r);
static rl_result storage_set_size(rl_ipc_req *r);
static rl_result storage_get_size(rl_ipc_req *r);

static const rl_ipc_cmd storage_cmds[] = {
    { 0, "Read", storage_read },        { 1, "Write", storage_write },     { 2, "Flush", storage_flush },
    { 3, "SetSize", storage_set_size }, { 4, "GetSize", storage_get_size },
};

static const rl_ipc_class storage_class = { "nn::fssrv::sf::IStorage", storage_cmds,
                                            sizeof storage_cmds / sizeof storage_cmds[0], 0, NULL };

static rl_result push_storage(rl_ipc_req *r, rl_romfs_image *img)
{
    rl_ipc_object *o = rl_ipc_object_new(&storage_class, img);
    if (!o)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_object(r, o);
}

static rl_result open_source(rl_ipc_req *r, source *s)
{
    rl_result rc = FS_TARGET_NOT_FOUND;
    rl_romfs_image *img;
    if (!s)
        return rc;
    pthread_mutex_lock(&g_lock);
    img = source_image(s, &rc);
    pthread_mutex_unlock(&g_lock);
    return img ? push_storage(r, img) : rc;
}

rl_result rl_fs_open_data_storage_current(rl_ipc_req *r) { return open_source(r, &g_main); }

rl_result rl_fs_open_data_storage_program(rl_ipc_req *r)
{
    uint64_t id;
    source *s;
    if (!rl_ipc_in(r, &id, sizeof id))
        return SF_RESULT_INVALID_HEADER;
    if (id == hos.cfg.program_id)
        return open_source(r, &g_main);
    pthread_mutex_lock(&g_lock);
    s = addon_source(id);
    pthread_mutex_unlock(&g_lock);
    return open_source(r, s);
}

rl_result rl_fs_open_data_storage_data_id(rl_ipc_req *r)
{
    struct {
        uint8_t storage_id;
        uint8_t pad[7];
        uint64_t data_id;
    } in;
    source *s;
    if (!rl_ipc_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_lock);
    s = addon_source(in.data_id);
    pthread_mutex_unlock(&g_lock);
    return open_source(r, s);
}

rl_result rl_fs_open_patch_data_storage(rl_ipc_req *r)
{
    (void)r;
    return FS_TARGET_NOT_FOUND;
}

static rl_result storage_read(rl_ipc_req *r)
{
    rl_romfs_image *img = r->self->state;
    struct {
        int64_t offset;
        uint64_t size;
    } in;
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint8_t *buf;
    uint64_t done = 0;
    if (!rl_ipc_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    if (in.offset < 0)
        return FS_INVALID_OFFSET;
    if (in.size > out.size)
        return FS_INVALID_SIZE;
    if ((uint64_t)in.offset > rl_romfs_image_size(img) || in.size > rl_romfs_image_size(img) - (uint64_t)in.offset)
        return FS_OUT_OF_RANGE;
    if (in.size == 0)
        return KR_SUCCESS;
    buf = malloc(in.size < STORAGE_CHUNK ? (size_t)in.size : STORAGE_CHUNK);
    if (!buf)
        return KR_OUT_OF_RESOURCE;
    while (done < in.size) {
        uint64_t want = in.size - done < STORAGE_CHUNK ? in.size - done : STORAGE_CHUNK;
        rl_ipc_buf piece;
        int rc = rl_romfs_image_read(img, (uint64_t)in.offset + done, buf, want);
        if (rc) {
            free(buf);
            return rc == RL_ROMFS_RANGE ? FS_OUT_OF_RANGE : FS_UNEXPECTED;
        }
        piece.addr = out.addr + done;
        piece.size = want;
        piece.attr = out.attr;
        if (!rl_ipc_buf_write(&piece, buf, want)) {
            free(buf);
            return KR_INVALID_MEM_STATE;
        }
        done += want;
    }
    free(buf);
    return KR_SUCCESS;
}

static rl_result storage_write(rl_ipc_req *r)
{
    (void)r;
    return FS_WRITE_NOT_PERMITTED;
}

static rl_result storage_flush(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result storage_set_size(rl_ipc_req *r)
{
    (void)r;
    return FS_WRITE_NOT_PERMITTED;
}

static rl_result storage_get_size(rl_ipc_req *r)
{
    const rl_romfs_image *img = r->self->state;
    *(uint64_t *)rl_ipc_out(r, 8) = rl_romfs_image_size(img);
    return KR_SUCCESS;
}
