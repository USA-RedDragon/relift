/**
 * @file test_post.c
 * @brief The core picture pipeline: its Vulkan declarations (vkmin.h, with the
 * presenter's) match the API's,
 * with every option off a window shows the frame it was given, byte for
 * byte, the FXAA pass on the host's device keeps flat pictures byte for
 * byte and matches the C reference, and SMAA at every preset keeps flat
 * pictures and softens only the pixels along edges, and an upscaler stage
 * runs first and sets the size the anti-aliasing passes and the window get.
 */
#include "runner.h"

#include "fxaa_ref.h"
#include "hostlib.h"
#include "vkmin.h"

#include <relift/post.h>
#include <relift/settings.h>
#include <relift/window.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>

#define SAME(ours, theirs)                                                                                           \
    _Static_assert(sizeof(ours) == sizeof(theirs), #ours " has the size of " #theirs)
#define AT(ours, theirs, f)                                                                                          \
    _Static_assert(offsetof(ours, f) == offsetof(theirs, f), #ours "." #f " is where " #theirs " has it")

SAME(rlvk_application_info, VkApplicationInfo);
AT(rlvk_application_info, VkApplicationInfo, apiVersion);
SAME(rlvk_instance_create_info, VkInstanceCreateInfo);
AT(rlvk_instance_create_info, VkInstanceCreateInfo, ppEnabledExtensionNames);
SAME(rlvk_queue_family_properties, VkQueueFamilyProperties);
SAME(rlvk_memory_properties, VkPhysicalDeviceMemoryProperties);
AT(rlvk_memory_properties, VkPhysicalDeviceMemoryProperties, memoryHeaps);
SAME(rlvk_extension_properties, VkExtensionProperties);
SAME(rlvk_device_queue_create_info, VkDeviceQueueCreateInfo);
SAME(rlvk_device_create_info, VkDeviceCreateInfo);
AT(rlvk_device_create_info, VkDeviceCreateInfo, pEnabledFeatures);
SAME(rlvk_buffer_create_info, VkBufferCreateInfo);
AT(rlvk_buffer_create_info, VkBufferCreateInfo, usage);
SAME(rlvk_memory_requirements, VkMemoryRequirements);
SAME(rlvk_memory_allocate_info, VkMemoryAllocateInfo);
SAME(rlvk_mapped_memory_range, VkMappedMemoryRange);
SAME(rlvk_shader_module_create_info, VkShaderModuleCreateInfo);
AT(rlvk_shader_module_create_info, VkShaderModuleCreateInfo, pCode);
SAME(rlvk_descriptor_set_layout_binding, VkDescriptorSetLayoutBinding);
SAME(rlvk_descriptor_set_layout_create_info, VkDescriptorSetLayoutCreateInfo);
SAME(rlvk_push_constant_range, VkPushConstantRange);
SAME(rlvk_pipeline_layout_create_info, VkPipelineLayoutCreateInfo);
SAME(rlvk_pipeline_shader_stage_create_info, VkPipelineShaderStageCreateInfo);
SAME(rlvk_compute_pipeline_create_info, VkComputePipelineCreateInfo);
AT(rlvk_compute_pipeline_create_info, VkComputePipelineCreateInfo, layout);
AT(rlvk_compute_pipeline_create_info, VkComputePipelineCreateInfo, basePipelineIndex);
SAME(rlvk_descriptor_pool_size, VkDescriptorPoolSize);
SAME(rlvk_descriptor_pool_create_info, VkDescriptorPoolCreateInfo);
SAME(rlvk_descriptor_set_allocate_info, VkDescriptorSetAllocateInfo);
SAME(rlvk_descriptor_buffer_info, VkDescriptorBufferInfo);
SAME(rlvk_write_descriptor_set, VkWriteDescriptorSet);
AT(rlvk_write_descriptor_set, VkWriteDescriptorSet, pBufferInfo);
SAME(rlvk_command_pool_create_info, VkCommandPoolCreateInfo);
SAME(rlvk_command_buffer_allocate_info, VkCommandBufferAllocateInfo);
SAME(rlvk_command_buffer_begin_info, VkCommandBufferBeginInfo);
SAME(rlvk_memory_barrier, VkMemoryBarrier);
SAME(rlvk_buffer_copy, VkBufferCopy);
SAME(rlvk_submit_info, VkSubmitInfo);
AT(rlvk_submit_info, VkSubmitInfo, pSignalSemaphores);
SAME(rlvk_fence_create_info, VkFenceCreateInfo);
SAME(rlvk_surface_capabilities, VkSurfaceCapabilitiesKHR);
AT(rlvk_surface_capabilities, VkSurfaceCapabilitiesKHR, supportedUsageFlags);
SAME(rlvk_surface_format, VkSurfaceFormatKHR);
SAME(rlvk_swapchain_create_info, VkSwapchainCreateInfoKHR);
AT(rlvk_swapchain_create_info, VkSwapchainCreateInfoKHR, imageExtent);
AT(rlvk_swapchain_create_info, VkSwapchainCreateInfoKHR, oldSwapchain);
SAME(rlvk_present_info, VkPresentInfoKHR);
AT(rlvk_present_info, VkPresentInfoKHR, pResults);
SAME(rlvk_image_create_info, VkImageCreateInfo);
AT(rlvk_image_create_info, VkImageCreateInfo, initialLayout);
SAME(rlvk_image_memory_barrier, VkImageMemoryBarrier);
AT(rlvk_image_memory_barrier, VkImageMemoryBarrier, subresourceRange);
SAME(rlvk_buffer_image_copy, VkBufferImageCopy);
AT(rlvk_buffer_image_copy, VkBufferImageCopy, imageExtent);
SAME(rlvk_image_blit, VkImageBlit);
AT(rlvk_image_blit, VkImageBlit, dstOffsets);
SAME(rlvk_clear_color, VkClearColorValue);
SAME(rlvk_semaphore_create_info, VkSemaphoreCreateInfo);
_Static_assert(RLVK_ST_SWAPCHAIN_CREATE_INFO_KHR == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, "sType");
_Static_assert(RLVK_ST_PRESENT_INFO_KHR == VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, "sType");
_Static_assert(RLVK_ST_IMAGE_MEMORY_BARRIER == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, "sType");
_Static_assert(RLVK_ST_IMAGE_CREATE_INFO == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, "sType");
_Static_assert(RLVK_ST_SEMAPHORE_CREATE_INFO == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, "sType");
_Static_assert(RLVK_IMAGE_LAYOUT_PRESENT_SRC_KHR == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, "layout");
_Static_assert(RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "layout");
_Static_assert(RLVK_FORMAT_B8G8R8A8_SRGB == VK_FORMAT_B8G8R8A8_SRGB, "format");
_Static_assert(RLVK_FORMAT_R8G8B8A8_UNORM == VK_FORMAT_R8G8B8A8_UNORM, "format");
_Static_assert(RLVK_ERROR_OUT_OF_DATE_KHR == VK_ERROR_OUT_OF_DATE_KHR, "result");
_Static_assert(RLVK_SUBOPTIMAL_KHR == VK_SUBOPTIMAL_KHR, "result");
_Static_assert(RLVK_ERROR_SURFACE_LOST_KHR == VK_ERROR_SURFACE_LOST_KHR, "result");
_Static_assert(RLVK_PRESENT_MODE_MAILBOX_KHR == VK_PRESENT_MODE_MAILBOX_KHR, "mode");
SAME(rlvk_descriptor_image_info, VkDescriptorImageInfo);
SAME(rlvk_sampler_create_info, VkSamplerCreateInfo);
AT(rlvk_sampler_create_info, VkSamplerCreateInfo, unnormalizedCoordinates);
SAME(rlvk_image_subresource_range, VkImageSubresourceRange);
SAME(rlvk_image_view_create_info, VkImageViewCreateInfo);
AT(rlvk_image_view_create_info, VkImageViewCreateInfo, subresourceRange);
SAME(rlvk_subgroup_properties, VkPhysicalDeviceSubgroupProperties);
_Static_assert(RLVK_ST_IMAGE_VIEW_CREATE_INFO == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, "sType");
_Static_assert(RLVK_ST_SAMPLER_CREATE_INFO == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, "sType");
_Static_assert(RLVK_ST_PHYSICAL_DEVICE_PROPERTIES_2 == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, "sType");
_Static_assert(RLVK_ST_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
               "sType");
_Static_assert(RLVK_FORMAT_R16G16B16A16_SFLOAT == VK_FORMAT_R16G16B16A16_SFLOAT, "format");
_Static_assert(RLVK_FORMAT_R32G32_SFLOAT == VK_FORMAT_R32G32_SFLOAT, "format");
_Static_assert(RLVK_FORMAT_R8G8_UNORM == VK_FORMAT_R8G8_UNORM, "format");
_Static_assert(RLVK_FORMAT_R16G16_SFLOAT == VK_FORMAT_R16G16_SFLOAT, "format");
_Static_assert(RLVK_FORMAT_R32_UINT == VK_FORMAT_R32_UINT, "format");
_Static_assert(RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, "descriptor type");
_Static_assert(RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, "descriptor type");
_Static_assert(RLVK_SUBGROUP_FEATURE_QUAD_BIT == VK_SUBGROUP_FEATURE_QUAD_BIT, "subgroup feature");
_Static_assert(RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, "sType");
_Static_assert(RLVK_ST_WRITE_DESCRIPTOR_SET == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, "sType");
_Static_assert(RLVK_ST_MEMORY_BARRIER == VK_STRUCTURE_TYPE_MEMORY_BARRIER, "sType");
_Static_assert(RLVK_ST_COMMAND_BUFFER_BEGIN_INFO == VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, "sType");
_Static_assert(RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, "sType");
_Static_assert(RLVK_ST_FENCE_CREATE_INFO == VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, "sType");
_Static_assert(RLVK_ST_MAPPED_MEMORY_RANGE == VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, "sType");
_Static_assert(RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "descriptor type");
_Static_assert(RLVK_PIPELINE_STAGE_HOST_BIT == VK_PIPELINE_STAGE_HOST_BIT, "stage");
_Static_assert(RLVK_ACCESS_HOST_READ_BIT == VK_ACCESS_HOST_READ_BIT, "access");
_Static_assert(RLVK_ERROR_EXTENSION_NOT_PRESENT == VK_ERROR_EXTENSION_NOT_PRESENT, "result");
_Static_assert(RLVK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR == VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
               "flag");
_Static_assert(RLVK_API_VERSION_1_0 == VK_API_VERSION_1_0, "version");
#endif

#define W 96
#define H 72

/** @brief A picture with edges in every direction: a dark diagonal stair, a bright disc and color bands. */
static void scene(uint8_t *rgb)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = rgb + (y * W + x) * 3;
            int dx = x - 64, dy = y - 30;
            if (dx * dx + dy * dy < 400) {
                p[0] = 250, p[1] = 230, p[2] = 40;
            } else if (x * 2 > y * 3 + 10) {
                p[0] = 20, p[1] = 40, p[2] = 120;
            } else {
                p[0] = (uint8_t)(x * 2), p[1] = (uint8_t)(160 + (y & 8) * 8), p[2] = 90;
            }
        }
}

/** @brief rl_post_apply on a W by H picture, which must come back W by H. */
static const uint8_t *apply(const uint8_t *rgb)
{
    uint32_t w = 0, h = 0;
    const uint8_t *out = rl_post_apply(rgb, W, H, &w, &h);
    return w == W && h == H ? out : NULL;
}

/** @brief Whether the pipeline runs on this host (a Vulkan loader and a device with a compute queue); else only the off
 * path is tested. */
static bool have_vulkan(void)
{
    static uint8_t tiny[4 * 4 * 3];
    rl_settings_reset();
    rl_settings_declare_runtime();
    bool ok = rl_hostlib_vulkan_gipa() && rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK;
    uint32_t w = 0, h = 0;
    ok = ok && rl_post_apply(tiny, 4, 4, &w, &h) != tiny;
    rl_settings_reset();
    rl_settings_declare_runtime();
    if (!ok)
        printf("    (no Vulkan device: the passes are not run)\n");
    return ok;
}

static const uint8_t *g_shown;
static uint8_t g_shown_copy[W * H * 3];

static void fake_present(rl_window *w, const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t sw, uint32_t sh)
{
    (void)w, (void)sw, (void)sh;
    g_shown = rgb;
    memcpy(g_shown_copy, rgb, (size_t)width * height * 3);
}

static const rl_window_backend g_fake = { .name = "fake", .present_rgb = fake_present };

TEST(post_off_a_window_shows_the_frame_it_was_given)
{
    rl_settings_reset();
    rl_settings_declare_runtime();
    static uint8_t rgb[W * H * 3];
    scene(rgb);
    rl_window win = { .b = &g_fake };
    CHECK(rl_window_present_rgb_shown(&win, rgb, W, H, W * 2, H * 2));
    CHECK(g_shown == rgb);
    CHECK(memcmp(g_shown_copy, rgb, sizeof rgb) == 0);
    if (have_vulkan()) {
        CHECK(rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK);
        CHECK(rl_window_present_rgb_shown(&win, rgb, W, H, W, H));
        CHECK(g_shown != rgb);
        CHECK(memcmp(g_shown_copy, rgb, sizeof rgb) != 0);
        CHECK(rl_settings_set("picture.aa", "off") == RL_SETTING_OK);
        CHECK(rl_window_present_rgb(&win, rgb, W, H));
        CHECK(g_shown == rgb);
        CHECK(memcmp(g_shown_copy, rgb, sizeof rgb) == 0);
    }
    rl_settings_reset();
}

TEST(post_with_every_option_off_gives_the_frame_itself)
{
    rl_settings_reset();
    rl_settings_declare_runtime();
    static uint8_t rgb[W * H * 3];
    scene(rgb);
    CHECK(apply(rgb) == rgb);
    CHECK(rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK);
    CHECK(rl_settings_set("picture.aa", "off") == RL_SETTING_OK);
    CHECK(apply(rgb) == rgb);
    rl_settings_reset();
}

TEST(post_fxaa_keeps_a_flat_picture_byte_for_byte)
{
    if (!have_vulkan())
        return;
    rl_settings_reset();
    rl_settings_declare_runtime();
    static uint8_t rgb[W * H * 3];
    for (size_t i = 0; i < sizeof rgb; i++)
        rgb[i] = (uint8_t)(i % 3 * 70 + 13);
    CHECK(rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK);
    const uint8_t *out = apply(rgb);
    CHECK(out != rgb);
    CHECK(memcmp(out, rgb, sizeof rgb) == 0);
    rl_settings_reset();
}

TEST(post_fxaa_matches_the_c_reference)
{
    if (!have_vulkan())
        return;
    rl_settings_reset();
    rl_settings_declare_runtime();
    static uint8_t in[W * H * 3], want[W * H * 3];
    scene(in);
    CHECK(fxaa_ref(in, want, W, H));
    CHECK(memcmp(in, want, sizeof in) != 0);
    CHECK(rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK);
    const uint8_t *got = apply(in);
    CHECK(got != in);
    int worst = 0, off = 0;
    for (size_t i = 0; i < sizeof in; i++) {
        int d = abs((int)got[i] - (int)want[i]);
        worst = d > worst ? d : worst;
        off += d > 0;
    }
    printf("    fxaa vs reference: %d of %zu bytes differ, by at most %d\n", off, sizeof in, worst);
    CHECK(worst <= 2);
    CHECK(off * 100 <= (int)sizeof in);
    rl_settings_reset();
}

TEST(post_smaa_keeps_flat_pictures_and_softens_only_edges)
{
    if (!have_vulkan())
        return;
    static const char *const presets[] = { "low", "medium", "high", "ultra" };
    static uint8_t flat[W * H * 3], in[W * H * 3];
    for (size_t i = 0; i < sizeof flat; i++)
        flat[i] = (uint8_t)(i % 3 * 70 + 13);
    scene(in);
    for (int q = 0; q < 4; q++) {
        rl_settings_reset();
        rl_settings_declare_runtime();
        CHECK(rl_settings_set("picture.aa", "smaa") == RL_SETTING_OK);
        CHECK(rl_settings_set("picture.aa_quality", presets[q]) == RL_SETTING_OK);
        const uint8_t *out = apply(flat);
        CHECK(out != flat);
        CHECK(memcmp(out, flat, sizeof flat) == 0);
        out = apply(in);
        int changed = 0, far = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                size_t i = ((size_t)y * W + (size_t)x) * 3;
                if (!memcmp(out + i, in + i, 3))
                    continue;
                changed++;
                bool edge = false;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < W && ny < H &&
                            memcmp(in + ((size_t)ny * W + (size_t)nx) * 3, in + i, 3))
                            edge = true;
                    }
                far += !edge;
            }
        printf("    smaa %s: %d pixels changed, %d away from an edge\n", presets[q], changed, far);
        CHECK(changed > H);
        CHECK(far == 0);
    }
    rl_settings_reset();
}

typedef struct fake_stage {
    int starts, plans, records;
    uint32_t in_w, in_h;
    void(RLVK_CALL *fill)(rlvk_handle cmd, rlvk_handle buffer, rlvk_size offset, rlvk_size size, uint32_t data);
} fake_stage;

#define FILL 0xff336699u

static bool fake_start(void *user, const rl_post_device *d)
{
    fake_stage *f = user;
    f->starts++;
    void *(RLVK_CALL *gdpa)(rlvk_handle, const char *) = (void *(RLVK_CALL *)(rlvk_handle, const char *))d->get_device_proc_addr;
    f->fill = (void(RLVK_CALL *)(rlvk_handle, rlvk_handle, rlvk_size, rlvk_size, uint32_t))gdpa(d->device, "vkCmdFillBuffer");
    return f->fill != NULL && d->device && d->queue;
}

static bool fake_plan(void *user, uint32_t w, uint32_t h, uint32_t *ow, uint32_t *oh)
{
    fake_stage *f = user;
    f->plans++;
    *ow = w * 2;
    *oh = h * 2;
    return true;
}

static void fake_record(void *user, uint64_t cmd, uint64_t in, uint64_t out, uint32_t w, uint32_t h, uint32_t ow,
                        uint32_t oh)
{
    fake_stage *f = user;
    (void)in;
    f->records++;
    f->in_w = w;
    f->in_h = h;
    f->fill(cmd, out, 0, (rlvk_size)ow * oh * 4, FILL);
}

static const rl_post_stage g_fake_stage = { "fake", fake_start, fake_plan, fake_record };

TEST(post_an_upscaler_runs_first_and_sets_the_size)
{
    if (!have_vulkan())
        return;
    rl_settings_reset();
    rl_settings_declare_runtime();
    static uint8_t rgb[W * H * 3];
    scene(rgb);
    fake_stage f = { 0 };
    rl_post_set_upscaler(&g_fake_stage, &f);
    for (int aa = 0; aa < 3; aa++) {
        CHECK(rl_settings_set("picture.aa", aa == 0 ? "off" : aa == 1 ? "fxaa" : "smaa") == RL_SETTING_OK);
        uint32_t w = 0, h = 0;
        const uint8_t *out = rl_post_apply(rgb, W, H, &w, &h);
        CHECK(out != rgb);
        CHECK_EQ(w, 2 * W);
        CHECK_EQ(h, 2 * H);
        CHECK_EQ(f.in_w, W);
        CHECK_EQ(f.in_h, H);
        bool flat = true;
        for (uint32_t i = 0; out && i < w * h; i++)
            flat = flat && out[3 * i] == 0x99 && out[3 * i + 1] == 0x66 && out[3 * i + 2] == 0x33;
        CHECK(flat);
    }
    CHECK_EQ(f.starts, 1);
    CHECK_EQ(f.records, 3);
    rl_post_set_upscaler(NULL, NULL);
    CHECK(rl_settings_set("picture.aa", "off") == RL_SETTING_OK);
    uint32_t w = 0, h = 0;
    CHECK(rl_post_apply(rgb, W, H, &w, &h) == rgb);
    CHECK_EQ(w, W);
    CHECK_EQ(h, H);
    rl_settings_reset();
}
