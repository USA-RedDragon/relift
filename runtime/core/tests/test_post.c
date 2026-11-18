/**
 * @file test_post.c
 * @brief The core picture pipeline: its Vulkan declarations match the API's,
 * with every option off a window shows the frame it was given, byte for
 * byte, the FXAA pass on the host's device keeps flat pictures byte for
 * byte and matches the C reference, and SMAA at every preset keeps flat
 * pictures and softens only the pixels along edges.
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

/** @brief Whether the pipeline runs on this host (a Vulkan loader and a device with a compute queue); else only the off
 * path is tested. */
static bool have_vulkan(void)
{
    static uint8_t tiny[4 * 4 * 3];
    rl_settings_reset();
    rl_settings_declare_runtime();
    bool ok = rl_hostlib_vulkan_gipa() && rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK;
    ok = ok && rl_post_apply(tiny, 4, 4) != tiny;
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
    CHECK(rl_post_apply(rgb, W, H) == rgb);
    CHECK(rl_settings_set("picture.aa", "fxaa") == RL_SETTING_OK);
    CHECK(rl_settings_set("picture.aa", "off") == RL_SETTING_OK);
    CHECK(rl_post_apply(rgb, W, H) == rgb);
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
    const uint8_t *out = rl_post_apply(rgb, W, H);
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
    const uint8_t *got = rl_post_apply(in, W, H);
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
        const uint8_t *out = rl_post_apply(flat, W, H);
        CHECK(out != flat);
        CHECK(memcmp(out, flat, sizeof flat) == 0);
        out = rl_post_apply(in, W, H);
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
