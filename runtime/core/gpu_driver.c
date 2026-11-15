/**
 * @file gpu_driver.c
 * @brief Custom Vulkan driver packages (gpu_driver.h).
 */
#include "gpu_driver.h"
#include "core.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define META_MAX (64 * 1024)

bool rl_gpu_driver_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n >= RL_GPU_DRIVER_NAME_MAX || name[0] == '.' || !strcmp(name, RL_GPU_DRIVER_SYSTEM))
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    }
    return true;
}

bool rl_gpu_drivers_dir(char *out, size_t size)
{
    const char *env = getenv(RL_GPU_DRIVERS_ENV);
    int n;
    if (env && *env)
        n = snprintf(out, size, "%s", env);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return false;
        n = snprintf(out, size, "%s/gpu_drivers", home);
    }
    if (n <= 0 || (size_t)n >= size)
        return false;
    while (n > 1 && out[n - 1] == '/')
        out[--n] = 0;
    return true;
}

static char *read_meta(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(META_MAX + 1);
    size_t n = buf ? fread(buf, 1, META_MAX + 1, f) : 0;
    fclose(f);
    if (!buf || n > META_MAX) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    return buf;
}

static const char *json_string(const char *p, char *out, size_t size)
{
    size_t n = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' || (unsigned char)*p < 0x20 || n + 1 >= size)
            return NULL;
        out[n++] = *p;
    }
    if (*p != '"')
        return NULL;
    out[n] = 0;
    return p + 1;
}

static bool meta_library(const char *json, char *out, size_t size)
{
    char s[256];
    for (const char *p = strchr(json, '"'); p; p = strchr(p, '"')) {
        const char *end = json_string(p, s, sizeof s);
        if (!end) {
            p++;
            continue;
        }
        p = end;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p != ':' || strcmp(s, "libraryName"))
            continue;
        for (p++; *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'; p++)
            ;
        return *p == '"' && json_string(p, out, size) != NULL;
    }
    return false;
}

static bool library_ok(const char *lib)
{
    return lib[0] && lib[0] != '.' && !strchr(lib, '/');
}

static bool read_package(const char *name, rl_gpu_driver *out, bool warn)
{
    char root[900], meta[1100];
    struct stat st;
    if (!rl_gpu_driver_name_ok(name) || !rl_gpu_drivers_dir(root, sizeof root))
        return false;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);
    snprintf(out->dir, sizeof out->dir, "%s/%s/", root, name);
    snprintf(meta, sizeof meta, "%smeta.json", out->dir);
    char *json = read_meta(meta);
    bool ok = json && meta_library(json, out->library, sizeof out->library) && library_ok(out->library);
    free(json);
    if (!ok) {
        if (warn)
            rl_log(RL_LOG_WARN, "gpu", "%s: no meta.json with a libraryName; not a driver package", out->dir);
        return false;
    }
    char lib[1400];
    snprintf(lib, sizeof lib, "%s%s", out->dir, out->library);
    if (stat(lib, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (warn)
            rl_log(RL_LOG_WARN, "gpu", "%s: the package's driver library is missing", lib);
        return false;
    }
    return true;
}

bool rl_gpu_driver_find(const char *name, rl_gpu_driver *out)
{
    if (!rl_gpu_driver_name_ok(name)) {
        rl_log(RL_LOG_WARN, "gpu", "%s: not a driver package name", name ? name : "(null)");
        return false;
    }
    return read_package(name, out, true);
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

size_t rl_gpu_driver_list(char (*names)[RL_GPU_DRIVER_NAME_MAX], size_t max)
{
    char root[900];
    if (!rl_gpu_drivers_dir(root, sizeof root))
        return 0;
    DIR *d = opendir(root);
    if (!d)
        return 0;
    size_t n = 0;
    rl_gpu_driver pkg;
    for (struct dirent *e; (e = readdir(d));) {
        if (!read_package(e->d_name, &pkg, false))
            continue;
        if (n < max)
            memcpy(names[n], pkg.name, sizeof names[n]);
        n++;
    }
    closedir(d);
    qsort(names, n < max ? n : max, RL_GPU_DRIVER_NAME_MAX, by_name);
    return n;
}
