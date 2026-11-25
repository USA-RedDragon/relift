/**
 * @file nvn_label.c
 * docgen:region overview
 * @brief Debug labels and object names for a capture tool (RenderDoc): with VK_EXT_debug_utils on (the core turns it
 * on only when a capture tool is in the process, rlvk_dev.debug_utils), each draw and dispatch the layer replays gets
 * a label that names the guest command (its command set and index in the set, as the game recorded it) and the
 * programs it runs, and each host image made for a guest texture or render target gets a name with its guest GPU
 * address, format and size. A capture then lists the draws as the game issued them and the images by the game's own
 * addresses. Without the extension every call returns at once.
 * docgen:end
 */
#include "nvn_vk.h"
#include "vkdev.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct {
    bool tried, on;
    PFN_vkCmdInsertDebugUtilsLabelEXT insert;
    PFN_vkSetDebugUtilsObjectNameEXT name;
    /** @brief The command being replayed: its set's number (counted from 1 per set replayed) and index in the set. */
    uint64_t set_seq;
    const void *set;
    uint32_t index;
    uint32_t op;
} L;

static bool on(void)
{
    if (!L.tried) {
        L.tried = true;
        const rlvk_dev *d = rl_vkdev();
        if (d && d->debug_utils) {
            L.insert = (PFN_vkCmdInsertDebugUtilsLabelEXT)d->gipa(d->instance, "vkCmdInsertDebugUtilsLabelEXT");
            L.name = (PFN_vkSetDebugUtilsObjectNameEXT)d->gipa(d->instance, "vkSetDebugUtilsObjectNameEXT");
            L.on = L.insert && L.name;
            if (L.on)
                rl_log(RL_LOG_INFO, "nvn", "labeling draws and naming images for the capture tool");
        }
    }
    return L.on;
}

bool nvn_label_on(void) { return on(); }

void nvn_label_command(const void *set, uint32_t index, uint32_t op)
{
    if (!on())
        return;
    if (set != L.set) {
        L.set = set;
        L.set_seq++;
    }
    L.index = index;
    L.op = op;
}

/** @brief The first 8 hex digits of a program key, or "-" for no program. */
static const char *short_key(const char *key, char out[9])
{
    if (!key)
        return "-";
    snprintf(out, 9, "%.8s", key);
    return out;
}

void nvn_label_draw(VkCommandBuffer cb, const char *kind, const char *const keys[6])
{
    if (!on())
        return;
    static const char *const stage[6] = { "vs", "tcs", "tes", "gs", "fs", "cs" };
    char text[256], k[9];
    int o = snprintf(text, sizeof text, "%s set %llu cmd %u (%s)", kind, (unsigned long long)L.set_seq, L.index,
                     nvn_cmd_name(L.op));
    for (uint32_t s = 0; s < 6 && o > 0 && (size_t)o < sizeof text; s++)
        if (keys[s])
            o += snprintf(text + o, sizeof text - (size_t)o, " %s %s", stage[s], short_key(keys[s], k));
    VkDebugUtilsLabelEXT l = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = text };
    L.insert(cb, &l);
}

void nvn_label_object(VkObjectType type, uint64_t handle, const char *fmt, ...)
{
    if (!on() || !handle)
        return;
    char text[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    VkDebugUtilsObjectNameInfoEXT n = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                                        .objectType = type,
                                        .objectHandle = handle,
                                        .pObjectName = text };
    L.name(g.dev, &n);
}
