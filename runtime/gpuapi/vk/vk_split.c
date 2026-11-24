/**
 * @file vk_split.c
 * @brief Transfer commands the guest records inside a render pass.
 *
 * Games record copies, blits, clears of images and buffer updates between
 * vkCmdBeginRenderPass and vkCmdEndRenderPass, which Vulkan forbids; the
 * console's driver runs them (inferred: shipped games do it and run). On a
 * host such a command can lose the device. The layer ends the host render
 * pass before the first such command and begins it again, before the next
 * command that needs it (a draw, vkCmdClearAttachments,
 * vkCmdExecuteCommands, vkCmdNextSubpass or vkCmdEndRenderPass), with a
 * resume variant of the pass: the same attachments, subpass and
 * dependencies, every load op LOAD and every initial layout the final one,
 * so the attachments keep what was drawn so far. Only single-subpass render
 * passes have a resume variant; in a pass with several subpasses the
 * command is passed on unchanged with a warning.
 */
#include "vkf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static vkf_map *g_resume;

static void put_resume(uint64_t pass, VkRenderPass resume)
{
    pthread_mutex_lock(&g_lock);
    if (!g_resume)
        g_resume = vkf_map_new();
    vkf_map_put(g_resume, pass, resume ? (void *)resume : NULL);
    pthread_mutex_unlock(&g_lock);
}

static VkRenderPass get_resume(uint64_t pass)
{
    pthread_mutex_lock(&g_lock);
    VkRenderPass r = g_resume ? (VkRenderPass)vkf_map_get(g_resume, pass) : NULL;
    pthread_mutex_unlock(&g_lock);
    return r;
}

void vkf_split_note_pass(vkf_call *k, VkDevice dev, const VkRenderPassCreateInfo *ci, VkRenderPass pass)
{
    if (!ci || ci->subpassCount != 1 || !pass)
        return;
    VkAttachmentDescription *a = vkf_alloc(k, (ci->attachmentCount + 1) * sizeof *a);
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        a[i] = ci->pAttachments[i];
        a[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a[i].initialLayout = a[i].finalLayout;
    }
    VkRenderPassCreateInfo r = *ci;
    r.pAttachments = a;
    VkRenderPass out = NULL;
    if (VKF_FN(k->fns, vkCreateRenderPass)(dev, &r, NULL, &out) >= 0)
        put_resume((uint64_t)(uintptr_t)pass, out);
}

void vkf_split_note_pass2(vkf_call *k, VkDevice dev, const VkRenderPassCreateInfo2 *ci, VkRenderPass pass)
{
    if (!ci || ci->subpassCount != 1 || !pass)
        return;
    VkAttachmentDescription2 *a = vkf_alloc(k, (ci->attachmentCount + 1) * sizeof *a);
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        a[i] = ci->pAttachments[i];
        a[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a[i].initialLayout = a[i].finalLayout;
    }
    VkRenderPassCreateInfo2 r = *ci;
    r.pAttachments = a;
    VkRenderPass out = NULL;
    if (VKF_FN(k->fns, vkCreateRenderPass2)(dev, &r, NULL, &out) >= 0)
        put_resume((uint64_t)(uintptr_t)pass, out);
}

static void forget(vkf_call *k)
{
    uint64_t pass = k->c->x[1];
    VkRenderPass r = pass ? get_resume(pass) : NULL;
    if (!r)
        return;
    put_resume(pass, NULL);
    if (k->obj)
        VKF_FN(k->fns, vkDestroyRenderPass)((VkDevice)k->obj->host, r, NULL);
}

void vkf_pass_forget(vkf_call *k)
{
    vkf_depth_forget_pass(k);
    forget(k);
}

static void record(vkf_call *k, VkSubpassContents contents)
{
    vkf_obj *o = k->obj;
    const VkRenderPassBeginInfo *bi = o ? vkg_ain_VkRenderPassBeginInfo(k, k->c->x[1], 1) : NULL;
    if (!bi)
        return;
    o->split.in_pass = true;
    o->split.ended = false;
    o->split.pass = (uint64_t)(uintptr_t)bi->renderPass;
    o->split.framebuffer = (uint64_t)(uintptr_t)bi->framebuffer;
    o->split.area = bi->renderArea;
    o->split.contents = contents;
    o->split.nviews = 0;
    for (const VkBaseInStructure *b = bi->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            const VkRenderPassAttachmentBeginInfo *v = (const VkRenderPassAttachmentBeginInfo *)b;
            for (uint32_t i = 0; i < v->attachmentCount && i < VKF_SPLIT_VIEWS; i++)
                o->split.views[o->split.nviews++] = (uint64_t)(uintptr_t)v->pAttachments[i];
        }
}

void vkf_pass_begin(vkf_call *k)
{
    vkf_depth_begin_pass(k);
    record(k, (VkSubpassContents)(uint32_t)k->c->x[2]);
}

void vkf_pass_begin2(vkf_call *k)
{
    vkf_depth_begin_pass(k);
    uint64_t sbi = k->c->x[2];
    record(k, sbi ? (VkSubpassContents)vkf_g32(k, sbi + offsetof(VkSubpassBeginInfo, contents)) : VK_SUBPASS_CONTENTS_INLINE);
}

void vkf_pass_end(vkf_call *k)
{
    vkf_depth_end_pass(k);
    if (k->obj)
        k->obj->split.in_pass = k->obj->split.ended = false;
}

void vkf_split_reset(vkf_obj *o)
{
    o->split.in_pass = o->split.ended = false;
}

void vkf_split_out(vkf_call *k)
{
    vkf_obj *o = k->obj;
    if (!o || !o->split.in_pass || o->split.ended)
        return;
    if (!get_resume(o->split.pass)) {
        if (rl_once(0x766b73706c697400ull))
            rl_log(RL_LOG_WARN, "vk", "a transfer command inside a render pass with several subpasses is passed on as it is");
        return;
    }
    VKF_FN(k->fns, vkCmdEndRenderPass)((VkCommandBuffer)o->host);
    o->split.ended = true;
}

void vkf_split_in(vkf_call *k)
{
    vkf_obj *o = k->obj;
    if (!o || !o->split.ended)
        return;
    VkImageView views[VKF_SPLIT_VIEWS];
    for (uint32_t i = 0; i < o->split.nviews; i++)
        views[i] = (VkImageView)(uintptr_t)o->split.views[i];
    VkRenderPassAttachmentBeginInfo ab = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,
        .attachmentCount = o->split.nviews,
        .pAttachments = views,
    };
    VkRenderPassBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = o->split.nviews ? &ab : NULL,
        .renderPass = get_resume(o->split.pass),
        .framebuffer = (VkFramebuffer)(uintptr_t)o->split.framebuffer,
        .renderArea = o->split.area,
    };
    VKF_FN(k->fns, vkCmdBeginRenderPass)((VkCommandBuffer)o->host, &bi, o->split.contents);
    o->split.ended = false;
}
