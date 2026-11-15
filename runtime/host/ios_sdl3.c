/**
 * @file ios_sdl3.c
 * @brief iOS glue through SDL3 (zlib license): main under UIKit, stdio, the app's Documents folder.
 */
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "core.h"

#include <relift/ios.h>

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_docs[PATH_MAX];

static void find_docs(void)
{
    const char *d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!d || !*d)
        return;
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/')
        n--;
    if (n >= sizeof g_docs)
        return;
    memcpy(g_docs, d, n);
    g_docs[n] = 0;
}

const char *rl_ios_documents_dir(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, find_docs);
    return g_docs[0] ? g_docs : NULL;
}

void rl_ios_init(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    const char *d = rl_ios_documents_dir();
    if (d && chdir(d) != 0)
        rl_log(RL_LOG_WARN, "ios", "cannot enter %s", d);
}

void rl_ios_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}

static int run(int argc, char **argv)
{
    int status = rl_ios_main(argc, argv);
    rl_ios_flush();
    exit(status);
}

int main(int argc, char **argv) { return SDL_RunApp(argc, argv, run, NULL); }

#endif
