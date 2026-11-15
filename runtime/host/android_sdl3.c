/**
 * @file android_sdl3.c
 * @brief Android glue through SDL3 (zlib license): logcat for stdio, the app's files directory.
 */
#ifdef __ANDROID__

#include <relift/android.h>

#include <SDL3/SDL.h>

#include <android/log.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "relift"
#define LINE_MAX_BYTES 1024

static int g_pipe[2] = {-1, -1};

static void *pump(void *arg)
{
    (void)arg;
    char line[LINE_MAX_BYTES + 1];
    size_t n = 0;
    char c;
    while (read(g_pipe[0], &c, 1) == 1) {
        if (c != '\n' && n < LINE_MAX_BYTES) {
            line[n++] = c;
            continue;
        }
        line[n] = 0;
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
        n = 0;
    }
    return NULL;
}

static void init_once(void)
{
    const char *dir = rl_android_files_dir();
    if (dir)
        setenv("HOME", dir, 1);
    if (pipe(g_pipe) != 0)
        return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(g_pipe[1], STDOUT_FILENO);
    dup2(g_pipe[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, NULL, pump, NULL) == 0)
        pthread_detach(t);
}

void rl_android_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_once);
}

const char *rl_android_files_dir(void) { return SDL_GetAndroidInternalStoragePath(); }

#endif
