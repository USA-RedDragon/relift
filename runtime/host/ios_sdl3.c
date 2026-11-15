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

#include <objc/message.h>
#include <objc/runtime.h>

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

static char g_bundle[PATH_MAX];

static void find_bundle(void)
{
    const char *d = SDL_GetBasePath();
    if (!d || !*d)
        return;
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/')
        n--;
    if (n < sizeof g_bundle) {
        memcpy(g_bundle, d, n);
        g_bundle[n] = 0;
    }
}

const char *rl_ios_bundle_dir(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, find_bundle);
    return g_bundle[0] ? g_bundle : NULL;
}

static id msg(id o, const char *sel) { return o ? ((id(*)(id, SEL))objc_msgSend)(o, sel_registerName(sel)) : NULL; }

static const char *text(id s)
{
    const char *t = s ? ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")) : NULL;
    return t ? t : "?";
}

void rl_ios_log_audio_route(void)
{
    id session = msg((id)objc_getClass("AVAudioSession"), "sharedInstance");
    id outs = msg(msg(session, "currentRoute"), "outputs");
    unsigned long n = outs ? ((unsigned long (*)(id, SEL))objc_msgSend)(outs, sel_registerName("count")) : 0;
    double vol = session ? ((float (*)(id, SEL))objc_msgSend)(session, sel_registerName("outputVolume")) : -1;
    rl_log(RL_LOG_INFO, "audio", "AVAudioSession category %s, volume %.2f, %lu output port%s", text(msg(session, "category")),
           vol, n, n == 1 ? "" : "s");
    for (unsigned long i = 0; i < n; i++) {
        id port = ((id(*)(id, SEL, unsigned long))objc_msgSend)(outs, sel_registerName("objectAtIndex:"), i);
        rl_log(RL_LOG_INFO, "audio", "route output %lu: %s (%s)", i, text(msg(port, "portName")),
               text(msg(port, "portType")));
    }
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
