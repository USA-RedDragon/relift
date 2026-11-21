/**
 * @file capture.c
 * @brief The capture trigger of the renderdoc.capture setting (capture.h): the value is <present>[:<frames>]; frame
 * k ends at the k-th guest present. When RenderDoc's library is already loaded into the process (its Vulkan layer
 * loads it privately, so it is only looked up, never loaded), a frame capture starts after present k-1 (before
 * present 1 for k = 1) and ends after present k + frames - 1. The calls are made without a device or window handle,
 * so they cover whatever device the process renders on, and a run that never calls vkQueuePresent is captured too.
 * Only Vulkan work shows in a capture: a guest that draws its frames on the CPU and shows them through a window has
 * nothing to capture, and a run with no window and no Vulkan device has no device to capture from.
 */
#include "capture.h"
#include "core.h"
#include "hostlib.h"

#include <relift/settings.h>

#include "../third_party/renderdoc/renderdoc_app.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define SEP '\\'
#else
#define MKDIR(p) mkdir((p), 0755)
#define SEP '/'
#endif

static struct {
    pthread_mutex_t lock;
    bool init, armed, active;
    uint64_t presents, first, frames;
    RENDERDOC_API_1_1_2 *api;
} rd = { .lock = PTHREAD_MUTEX_INITIALIZER };

/**
 * @brief Points RenderDoc's capture files at renderdoc.capture_dir, or at the captures folder of the app's user
 * data folder (settings.h), so a capture never lands in a system temporary folder. The folders are made.
 */
static void set_capture_path(void)
{
    char dir[1024], tmpl[1100];
    const char *set = rl_setting_str("renderdoc.capture_dir");
    const char *app = rl_settings_app();
    if (set && *set) {
        snprintf(dir, sizeof dir, "%s", set);
    } else {
        char user[900];
        if (!rl_settings_user_dir(user, sizeof user, app && *app ? app : "relift"))
            return;
        MKDIR(user);
        snprintf(dir, sizeof dir, "%s%ccaptures", user, SEP);
    }
    MKDIR(dir);
    int k = snprintf(tmpl, sizeof tmpl, "%s%c%s", dir, SEP, app && *app ? app : "relift");
    if (k > 0 && (size_t)k < sizeof tmpl) {
        rd.api->SetCaptureFilePathTemplate(tmpl);
        rl_log(RL_LOG_INFO, "capture", "RenderDoc captures go to %s_capture.rdc", tmpl);
    }
}

static void rd_init(void)
{
    rd.init = true;
    rl_settings_declare_runtime();
    const char *v = rl_setting_str("renderdoc.capture");
    if (!v || !*v)
        return;
    char *end;
    unsigned long long first = strtoull(v, &end, 10), frames = 1;
    if (end == v || first == 0 || (*end && *end != ':') || (*end == ':' && (frames = strtoull(end + 1, &end, 10)) == 0) ||
        *end) {
        rl_log(RL_LOG_WARN, "capture", "renderdoc.capture=%s is not <present>[:<frames>] with both from 1; ignored", v);
        return;
    }
    static const char *const names[] = { "librenderdoc.so", "renderdoc.dll", NULL };
    void *lib = rl_hostlib_loaded(names);
    pRENDERDOC_GetAPI get = (pRENDERDOC_GetAPI)rl_hostlib_sym(lib, "RENDERDOC_GetAPI");
    if (!get || !get(eRENDERDOC_API_Version_1_1_2, (void **)&rd.api) || !rd.api) {
        rl_log(RL_LOG_WARN, "capture", "renderdoc.capture is set but RenderDoc is not loaded into the process");
        return;
    }
    set_capture_path();
    rd.first = first;
    rd.frames = frames;
    rd.armed = true;
}

static void edge(bool done)
{
    pthread_mutex_lock(&rd.lock);
    if (!rd.init)
        rd_init();
    if (rd.armed) {
        if (done)
            rd.presents++;
        bool start = done ? rd.presents + 1 == rd.first : rd.presents == 0 && rd.first == 1;
        if (!rd.active && start) {
            rd.api->StartFrameCapture(NULL, NULL);
            rd.active = true;
            rl_log(RL_LOG_INFO, "capture", "RenderDoc capture started before frame %llu", (unsigned long long)rd.first);
        } else if (rd.active && done && rd.presents == rd.first + rd.frames - 1) {
            uint32_t ok = rd.api->EndFrameCapture(NULL, NULL);
            rd.active = false;
            rd.armed = false;
            rl_log(RL_LOG_INFO, "capture", "RenderDoc capture ended after present %llu (%s)",
                   (unsigned long long)rd.presents, ok ? "written" : "failed");
        }
    }
    pthread_mutex_unlock(&rd.lock);
}

void rl_capture_present_begin(void) { edge(false); }

void rl_capture_present_end(void) { edge(true); }
