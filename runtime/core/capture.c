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
#include "snapshot.h"

#include <relift/settings.h>

#include "../third_party/renderdoc/renderdoc_app.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    /** @brief The lookup found no library while RenderDoc's layer is enabled: the next Vulkan instance loads it. */
    bool wait_layer;
    /** @brief Between rl_capture_present_begin and rl_capture_present_end. */
    bool in_present;
    uint64_t presents, first, frames;
    /** @brief renderdoc.capture=file: the file whose appearance asks for a capture of the next present. */
    char trigger[1100];
    /** @brief The capture follows the trigger file, again after each capture. */
    bool trigger_mode;
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
    snprintf(rd.trigger, sizeof rd.trigger, "%s%ccapture-next", dir, SEP);
    int k = snprintf(tmpl, sizeof tmpl, "%s%c%s", dir, SEP, app && *app ? app : "relift");
    if (k > 0 && (size_t)k < sizeof tmpl) {
        rd.api->SetCaptureFilePathTemplate(tmpl);
        rl_log(RL_LOG_INFO, "capture", "RenderDoc captures go to %s_capture.rdc", tmpl);
    }
}

static void rd_init(bool can_wait)
{
    rd.init = true;
    rl_settings_declare_runtime();
    const char *v = rl_setting_str("renderdoc.capture");
    if (!v || !*v)
        return;
    char *end = NULL;
    bool on_file = strcmp(v, "file") == 0;
    unsigned long long first = on_file ? 0 : strtoull(v, &end, 10), frames = 1;
    if (!on_file && (end == v || first == 0 || (*end && *end != ':') ||
                     (*end == ':' && (frames = strtoull(end + 1, &end, 10)) == 0) || *end)) {
        rl_log(RL_LOG_WARN, "capture", "renderdoc.capture=%s is not <present>[:<frames>] with both from 1, or file; ignored",
               v);
        return;
    }
    static const char *const names[] = { "librenderdoc.so", "renderdoc.dll", NULL };
    void *lib = rl_hostlib_loaded(names);
    pRENDERDOC_GetAPI get = (pRENDERDOC_GetAPI)rl_hostlib_sym(lib, "RENDERDOC_GetAPI");
    if (!get || !get(eRENDERDOC_API_Version_1_1_2, (void **)&rd.api) || !rd.api) {
        rd.api = NULL;
        const char *layer = getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
        if (can_wait && layer && strcmp(layer, "1") == 0) {
            rd.wait_layer = true;
            return;
        }
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
        rd_init(true);
    rd.in_present = !done;
    if (rd.armed) {
        if (done)
            rd.presents++;
        bool on_file = rd.first == 0 || rd.trigger_mode;
        if (on_file && done && !rd.active && rd.trigger[0]) {
            /* renderdoc.capture=file: a capture-next file in the capture folder asks for the next present. */
            FILE *f = fopen(rd.trigger, "r");
            if (f) {
                fclose(f);
                remove(rd.trigger);
                rd.trigger_mode = true;
                rd.first = rd.presents + 1;
            }
        }
        bool start = done ? rd.presents + 1 == rd.first : rd.presents == 0 && rd.first == 1;
        if (!rd.active && start) {
            rd.api->StartFrameCapture(NULL, NULL);
            rd.active = true;
            rl_log(RL_LOG_INFO, "capture", "RenderDoc capture started before frame %llu", (unsigned long long)rd.first);
        } else if (rd.active && done && rd.presents == rd.first + rd.frames - 1) {
            uint32_t ok = rd.api->EndFrameCapture(NULL, NULL);
            rd.active = false;
            rd.armed = rd.trigger_mode;
            if (rd.trigger_mode)
                rd.first = 0;
            rl_log(RL_LOG_INFO, "capture", "RenderDoc capture ended after present %llu (%s)",
                   (unsigned long long)rd.presents, ok ? "written" : "failed");
        }
    }
    pthread_mutex_unlock(&rd.lock);
}

void rl_capture_present_begin(void) { edge(false); }

void rl_capture_present_end(void)
{
    edge(true);
    rl_snapshot_present();
}

/**
 * docgen:region vulkan-up
 * @brief RenderDoc's Vulkan layer (ENABLE_VULKAN_RENDERDOC_CAPTURE=1) loads its library into the process when a
 * Vulkan instance is made, which for a presenter can be inside the first present, after the first look for it. So
 * the look waits for the instance when the layer is enabled, and is made again here, once the core device is up or a
 * guest's own instance is made; a capture that starts with present 1 starts here when that present is under way.
 * docgen:end
 */
void rl_capture_vulkan_up(void)
{
    pthread_mutex_lock(&rd.lock);
    if (!rd.init || rd.wait_layer) {
        rd.wait_layer = false;
        rd_init(false);
    }
    if (rd.armed && !rd.active && rd.in_present && rd.presents == 0 && rd.first == 1) {
        rd.api->StartFrameCapture(NULL, NULL);
        rd.active = true;
        rl_log(RL_LOG_INFO, "capture", "RenderDoc capture started before frame 1");
    }
    pthread_mutex_unlock(&rd.lock);
}

bool rl_capture_tool(void)
{
    static const char *const names[] = { "librenderdoc.so", "renderdoc.dll", NULL };
    const char *layer = getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
    return (layer && strcmp(layer, "1") == 0) || rl_hostlib_loaded(names) != NULL;
}
