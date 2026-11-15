/**
 * @file data_sdl3.c
 * @brief The game data check of a mobile app at start (relift/datacheck.h), with the manifest read through SDL3
 * (zlib license): from the iOS app bundle or the Android APK's assets.
 */
#include <relift/datacheck.h>

#include "core.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST "data-manifest.txt"

static void *load_manifest(size_t *len)
{
#if defined(__ANDROID__)
    return SDL_LoadFile(MANIFEST, len);
#else
    const char *base = SDL_GetBasePath();
    char path[4096];
    if (!base || snprintf(path, sizeof path, "%s%s", base, MANIFEST) >= (int)sizeof path)
        return NULL;
    return SDL_LoadFile(path, len);
#endif
}

int rl_app_check_data(void)
{
    size_t len = 0;
    char *text = load_manifest(&len);
    if (!text) {
        rl_log(RL_LOG_INFO, "data", "no %s in the app: game data not checked", MANIFEST);
        return 0;
    }
    const char *v = getenv("RELIFT_DATA_VERIFY");
    bool hash = v && !strcmp(v, "hash");
    const char *romfs = getenv("RELIFT_ROMFS"), *dlc = getenv("RELIFT_DLC");
    uint64_t t0 = rl_clock_ns();
    rl_data_report r;
    int rc = rl_data_check(text, len, romfs, dlc, hash, &r);
    SDL_free(text);
    uint64_t ms = (rl_clock_ns() - t0) / 1000000;
    if (rc == 0) {
        rl_log(RL_LOG_INFO, "data", "game data matches the manifest: %llu files, %llu MiB, %s, %llu ms",
               (unsigned long long)r.files, (unsigned long long)(r.bytes >> 20), hash ? "sizes and SHA-256" : "sizes",
               (unsigned long long)ms);
        return 0;
    }
    char msg[1024];
    if (rc < 0)
        snprintf(msg, sizeof msg, "The app's %s is malformed. Rebuild the app.", MANIFEST);
    else
        snprintf(msg, sizeof msg,
                 "The game data is incomplete: %llu of %llu files do not match. First: %s %s.\n"
                 "Copy the game's romfs folder into %s again (relift push does it and checks it).",
                 (unsigned long long)r.bad, (unsigned long long)r.files, r.first, r.why, romfs ? romfs : "the app");
    rl_log(RL_LOG_ERROR, "data", "%s", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game data", msg, NULL);
    return -1;
}
