/**
 * @file android_sdl3.c
 * @brief Android glue through SDL3 (zlib license): logcat for stdio, the app's files directory, the haptic tick,
 * the app's settings page, its GPU driver checks and its game data import.
 */
#ifdef __ANDROID__

#include "core.h"
#include "hostlib.h"

#include <relift/android.h>
#include <relift/touchpad.h>

#include <SDL3/SDL.h>

#include <android/log.h>
#include <jni.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

static void drain(void)
{
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < 100; i++) {
        int pending = 0;
        if (ioctl(g_pipe[0], FIONREAD, &pending) != 0 || pending == 0)
            break;
        usleep(2000);
    }
    usleep(5000);
}

void rl_android_flush(void) { drain(); }

static void leave(uint32_t code)
{
    drain();
    _exit((int)code);
}

static bool call_activity(const char *method, const char *arg)
{
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jobject act = SDL_GetAndroidActivity();
    if (!env || !act)
        return false;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = c ? (*env)->GetMethodID(env, c, method, arg ? "(Ljava/lang/String;)V" : "()V") : NULL;
    if (m) {
        if (arg) {
            jstring s = (*env)->NewStringUTF(env, arg);
            (*env)->CallVoidMethod(env, act, m, s);
            if (s)
                (*env)->DeleteLocalRef(env, s);
        } else {
            (*env)->CallVoidMethod(env, act, m);
        }
    }
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (c)
        (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return m != NULL;
}

static void driver_failed(const char *name) { call_activity("reliftGpuDriverFailed", name); }

static void driver_loaded(const char *name) { call_activity("reliftGpuDriverLoaded", name); }

static void *apk_asset(const char *name, size_t *len)
{
    size_t n = 0;
    void *data = SDL_LoadFile(name, &n);
    if (!data)
        return NULL;
    void *copy = malloc(n ? n : 1);
    if (copy) {
        memcpy(copy, data, n);
        *len = n;
    }
    SDL_free(data);
    return copy;
}

static void init_once(void)
{
    rl_set_asset_loader(apk_asset);
    rl_hostlib_on_gpu_driver_failed(driver_failed);
    rl_hostlib_on_gpu_driver_loaded(driver_loaded);
    {
        /* the manifest says whether this app offers a GPU driver choice (relift.gpu_driver) */
        char offered[8] = "";
        if (rl_android_call_str("reliftGpuDriverOffered", offered, sizeof offered))
            rl_hostlib_set_driver_choice(strcmp(offered, "false") != 0);
    }
    const char *dir = rl_android_files_dir();
    if (dir) {
        setenv("HOME", dir, 1);
        char reports[4096];
        snprintf(reports, sizeof reports, "%s/reports", dir);
        rl_report_set_dir(reports);
    }
    if (pipe(g_pipe) != 0)
        return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(g_pipe[1], STDOUT_FILENO);
    dup2(g_pipe[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, NULL, pump, NULL) == 0) {
        pthread_detach(t);
        atexit(drain);
        rl_set_exit_hook(leave);
    }
}

void rl_android_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_once);
}

const char *rl_android_files_dir(void) { return SDL_GetAndroidInternalStoragePath(); }

const char *rl_android_external_dir(void) { return SDL_GetAndroidExternalStoragePath(); }

JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeSettingsDone(JNIEnv *env, jclass cls, jint next);
JNIEXPORT jstring JNICALL Java_dev_relift_app_MainActivity_nativeGpuDriverLoaded(JNIEnv *env, jclass cls);
JNIEXPORT jstring JNICALL Java_dev_relift_app_GpuProbe_nativeProbe(JNIEnv *env, jclass cls, jstring dir,
                                                                  jstring library);

void rl_android_haptic(void) { call_activity("reliftHaptic", NULL); }

bool rl_android_data_checked(const char *report) { return call_activity("reliftDataChecked", report); }

void rl_android_settings(void)
{
    if (!call_activity("reliftOpenSettings", NULL))
        rl_touchpad_host_settings_done(RL_TOUCHPAD_HOST_PAUSE);
}

JNIEXPORT void JNICALL Java_dev_relift_app_MainActivity_nativeSettingsDone(JNIEnv *env, jclass cls, jint next)
{
    (void)env;
    (void)cls;
    rl_touchpad_host_settings_done(next);
}

JNIEXPORT jstring JNICALL Java_dev_relift_app_MainActivity_nativeGpuDriverLoaded(JNIEnv *env, jclass cls)
{
    (void)cls;
    const char *name = rl_hostlib_gpu_driver_loaded();
    return name ? (*env)->NewStringUTF(env, name) : NULL;
}

JNIEXPORT jstring JNICALL Java_dev_relift_app_GpuProbe_nativeProbe(JNIEnv *env, jclass cls, jstring dir,
                                                                  jstring library)
{
    (void)cls;
    const char *d = dir ? (*env)->GetStringUTFChars(env, dir, NULL) : NULL;
    const char *l = library ? (*env)->GetStringUTFChars(env, library, NULL) : NULL;
    char out[2048], text[2100];
    bool ok = rl_hostlib_vulkan_probe(d, l, out, sizeof out);
    snprintf(text, sizeof text, "%s\n%s", ok ? "ok" : "error", out);
    if (d)
        (*env)->ReleaseStringUTFChars(env, dir, d);
    if (l)
        (*env)->ReleaseStringUTFChars(env, library, l);
    return (*env)->NewStringUTF(env, text);
}

#endif
