/**
 * @file vkmin.h
 * @brief The part of the Vulkan 1.0 API, VK_KHR_surface and VK_KHR_swapchain
 * the core's Vulkan device (vkdev.c), picture pipeline (post.c) and window
 * presenter (present.c) use, declared here so the core builds on every host without Vulkan
 * headers. Names are the API's with an rlvk_ prefix; layouts and values are
 * the API's (test_post.c checks them against <vulkan/vulkan.h> where the
 * build has it). Every handle is a 64-bit value: a pointer for the
 * dispatchable ones on 64-bit hosts, which is the same size and passes the
 * same way.
 */
#ifndef RELIFT_CORE_VKMIN_H
#define RELIFT_CORE_VKMIN_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define RLVK_CALL __stdcall
#else
#define RLVK_CALL
#endif

typedef uint64_t rlvk_handle;
typedef int32_t rlvk_result;
typedef uint32_t rlvk_flags;
typedef uint64_t rlvk_size;

/** @brief VkResult values post.c tells apart. */
enum {
    RLVK_SUCCESS = 0,
    RLVK_NOT_READY = 1,
    RLVK_TIMEOUT = 2,
    RLVK_INCOMPLETE = 5,
    RLVK_ERROR_EXTENSION_NOT_PRESENT = -7,
    RLVK_ERROR_SURFACE_LOST_KHR = -1000000000,
    RLVK_SUBOPTIMAL_KHR = 1000001003,
    RLVK_ERROR_OUT_OF_DATE_KHR = -1000001004
};

/** @brief VkStructureType values of the structures below. */
enum {
    RLVK_ST_APPLICATION_INFO = 0,
    RLVK_ST_INSTANCE_CREATE_INFO = 1,
    RLVK_ST_DEVICE_QUEUE_CREATE_INFO = 2,
    RLVK_ST_DEVICE_CREATE_INFO = 3,
    RLVK_ST_SUBMIT_INFO = 4,
    RLVK_ST_MEMORY_ALLOCATE_INFO = 5,
    RLVK_ST_MAPPED_MEMORY_RANGE = 6,
    RLVK_ST_FENCE_CREATE_INFO = 8,
    RLVK_ST_BUFFER_CREATE_INFO = 12,
    RLVK_ST_SHADER_MODULE_CREATE_INFO = 16,
    RLVK_ST_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO = 29,
    RLVK_ST_PIPELINE_LAYOUT_CREATE_INFO = 30,
    RLVK_ST_SAMPLER_CREATE_INFO = 31,
    RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO = 33,
    RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    RLVK_ST_WRITE_DESCRIPTOR_SET = 35,
    RLVK_ST_COMMAND_POOL_CREATE_INFO = 39,
    RLVK_ST_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    RLVK_ST_COMMAND_BUFFER_BEGIN_INFO = 42,
    RLVK_ST_MEMORY_BARRIER = 46,
    RLVK_ST_SEMAPHORE_CREATE_INFO = 9,
    RLVK_ST_IMAGE_CREATE_INFO = 14,
    RLVK_ST_IMAGE_MEMORY_BARRIER = 45,
    RLVK_ST_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
    RLVK_ST_PRESENT_INFO_KHR = 1000001001,
};

/** @brief Flag and enum values post.c uses. */
enum {
    RLVK_QUEUE_COMPUTE_BIT = 0x2,
    RLVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x1,
    RLVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2,
    RLVK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4,
    RLVK_MEMORY_PROPERTY_HOST_CACHED_BIT = 0x8,
    RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x1,
    RLVK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x2,
    RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x20,
    RLVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1,
    RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE = 3,
    RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
    RLVK_IMAGE_LAYOUT_GENERAL = 1,
    RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2,
    RLVK_SHADER_STAGE_COMPUTE_BIT = 0x20,
    RLVK_PIPELINE_BIND_POINT_COMPUTE = 1,
    RLVK_COMMAND_BUFFER_LEVEL_PRIMARY = 0,
    RLVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2,
    RLVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1,
    RLVK_ACCESS_SHADER_READ_BIT = 0x20,
    RLVK_ACCESS_SHADER_WRITE_BIT = 0x40,
    RLVK_ACCESS_TRANSFER_READ_BIT = 0x800,
    RLVK_ACCESS_TRANSFER_WRITE_BIT = 0x1000,
    RLVK_ACCESS_HOST_READ_BIT = 0x2000,
    RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT = 0x800,
    RLVK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000,
    RLVK_PIPELINE_STAGE_HOST_BIT = 0x4000,
    RLVK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR = 0x1,
    RLVK_QUEUE_GRAPHICS_BIT = 0x1,
    RLVK_FENCE_CREATE_SIGNALED_BIT = 0x1,
    RLVK_FORMAT_R8G8B8A8_UNORM = 37,
    RLVK_FORMAT_R8G8B8A8_SRGB = 43,
    RLVK_FORMAT_B8G8R8A8_UNORM = 44,
    RLVK_FORMAT_B8G8R8A8_SRGB = 50,
    RLVK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0,
    RLVK_IMAGE_TYPE_2D = 1,
    RLVK_IMAGE_TILING_OPTIMAL = 0,
    RLVK_SAMPLE_COUNT_1_BIT = 0x1,
    RLVK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x1,
    RLVK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2,
    RLVK_IMAGE_ASPECT_COLOR_BIT = 0x1,
    RLVK_IMAGE_LAYOUT_UNDEFINED = 0,
    RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
    RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
    RLVK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002,
    RLVK_FILTER_NEAREST = 0,
    RLVK_FILTER_LINEAR = 1,
    RLVK_QUEUE_FAMILY_IGNORED = ~0u,
    RLVK_ACCESS_MEMORY_READ_BIT = 0x8000,
    RLVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x1,
    RLVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x2000,
    RLVK_PRESENT_MODE_IMMEDIATE_KHR = 0,
    RLVK_PRESENT_MODE_MAILBOX_KHR = 1,
    RLVK_PRESENT_MODE_FIFO_KHR = 2,
    RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR = 0x1,
    RLVK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR = 0x2,
    RLVK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR = 0x4,
    RLVK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR = 0x8,
    RLVK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 0x1,
    RLVK_COMPOSITE_ALPHA_INHERIT_BIT_KHR = 0x8,
};

/** @brief VK_API_VERSION_1_0. */
#define RLVK_API_VERSION_1_0 ((1u << 22) | (0u << 12))

/** @brief VkApplicationInfo. */
typedef struct rlvk_application_info {
    int32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} rlvk_application_info;

/** @brief VkInstanceCreateInfo. */
typedef struct rlvk_instance_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    const rlvk_application_info *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} rlvk_instance_create_info;

/** @brief VkExtent3D. */
typedef struct rlvk_extent3d {
    uint32_t width, height, depth;
} rlvk_extent3d;

/** @brief VkQueueFamilyProperties. */
typedef struct rlvk_queue_family_properties {
    rlvk_flags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    rlvk_extent3d minImageTransferGranularity;
} rlvk_queue_family_properties;

/** @brief VkMemoryType. */
typedef struct rlvk_memory_type {
    rlvk_flags propertyFlags;
    uint32_t heapIndex;
} rlvk_memory_type;

/** @brief VkMemoryHeap. */
typedef struct rlvk_memory_heap {
    rlvk_size size;
    rlvk_flags flags;
} rlvk_memory_heap;

/** @brief VkPhysicalDeviceMemoryProperties. */
typedef struct rlvk_memory_properties {
    uint32_t memoryTypeCount;
    rlvk_memory_type memoryTypes[32];
    uint32_t memoryHeapCount;
    rlvk_memory_heap memoryHeaps[16];
} rlvk_memory_properties;

/** @brief VkExtensionProperties. */
typedef struct rlvk_extension_properties {
    char extensionName[256];
    uint32_t specVersion;
} rlvk_extension_properties;

/** @brief VkDeviceQueueCreateInfo. */
typedef struct rlvk_device_queue_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} rlvk_device_queue_create_info;

/** @brief VkDeviceCreateInfo. */
typedef struct rlvk_device_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    uint32_t queueCreateInfoCount;
    const rlvk_device_queue_create_info *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} rlvk_device_create_info;

/** @brief VkBufferCreateInfo. */
typedef struct rlvk_buffer_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    rlvk_size size;
    rlvk_flags usage;
    int32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
} rlvk_buffer_create_info;

/** @brief VkMemoryRequirements. */
typedef struct rlvk_memory_requirements {
    rlvk_size size;
    rlvk_size alignment;
    uint32_t memoryTypeBits;
} rlvk_memory_requirements;

/** @brief VkMemoryAllocateInfo. */
typedef struct rlvk_memory_allocate_info {
    int32_t sType;
    const void *pNext;
    rlvk_size allocationSize;
    uint32_t memoryTypeIndex;
} rlvk_memory_allocate_info;

/** @brief VkMappedMemoryRange. */
typedef struct rlvk_mapped_memory_range {
    int32_t sType;
    const void *pNext;
    rlvk_handle memory;
    rlvk_size offset;
    rlvk_size size;
} rlvk_mapped_memory_range;

/** @brief VkShaderModuleCreateInfo. */
typedef struct rlvk_shader_module_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    size_t codeSize;
    const uint32_t *pCode;
} rlvk_shader_module_create_info;

/** @brief VkDescriptorSetLayoutBinding. */
typedef struct rlvk_descriptor_set_layout_binding {
    uint32_t binding;
    int32_t descriptorType;
    uint32_t descriptorCount;
    rlvk_flags stageFlags;
    const rlvk_handle *pImmutableSamplers;
} rlvk_descriptor_set_layout_binding;

/** @brief VkDescriptorSetLayoutCreateInfo. */
typedef struct rlvk_descriptor_set_layout_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    uint32_t bindingCount;
    const rlvk_descriptor_set_layout_binding *pBindings;
} rlvk_descriptor_set_layout_create_info;

/** @brief VkPushConstantRange. */
typedef struct rlvk_push_constant_range {
    rlvk_flags stageFlags;
    uint32_t offset;
    uint32_t size;
} rlvk_push_constant_range;

/** @brief VkPipelineLayoutCreateInfo. */
typedef struct rlvk_pipeline_layout_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    uint32_t setLayoutCount;
    const rlvk_handle *pSetLayouts;
    uint32_t pushConstantRangeCount;
    const rlvk_push_constant_range *pPushConstantRanges;
} rlvk_pipeline_layout_create_info;

/** @brief VkPipelineShaderStageCreateInfo. */
typedef struct rlvk_pipeline_shader_stage_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    rlvk_flags stage;
    rlvk_handle module;
    const char *pName;
    const void *pSpecializationInfo;
} rlvk_pipeline_shader_stage_create_info;

/** @brief VkComputePipelineCreateInfo. */
typedef struct rlvk_compute_pipeline_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    rlvk_pipeline_shader_stage_create_info stage;
    rlvk_handle layout;
    rlvk_handle basePipelineHandle;
    int32_t basePipelineIndex;
} rlvk_compute_pipeline_create_info;

/** @brief VkDescriptorPoolSize. */
typedef struct rlvk_descriptor_pool_size {
    int32_t type;
    uint32_t descriptorCount;
} rlvk_descriptor_pool_size;

/** @brief VkDescriptorPoolCreateInfo. */
typedef struct rlvk_descriptor_pool_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    uint32_t maxSets;
    uint32_t poolSizeCount;
    const rlvk_descriptor_pool_size *pPoolSizes;
} rlvk_descriptor_pool_create_info;

/** @brief VkDescriptorSetAllocateInfo. */
typedef struct rlvk_descriptor_set_allocate_info {
    int32_t sType;
    const void *pNext;
    rlvk_handle descriptorPool;
    uint32_t descriptorSetCount;
    const rlvk_handle *pSetLayouts;
} rlvk_descriptor_set_allocate_info;

/** @brief VkDescriptorBufferInfo. */
typedef struct rlvk_descriptor_buffer_info {
    rlvk_handle buffer;
    rlvk_size offset;
    rlvk_size range;
} rlvk_descriptor_buffer_info;

/** @brief VkDescriptorImageInfo. */
typedef struct rlvk_descriptor_image_info {
    rlvk_handle sampler;
    rlvk_handle imageView;
    int32_t imageLayout;
} rlvk_descriptor_image_info;

/** @brief VkSamplerCreateInfo. */
typedef struct rlvk_sampler_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    int32_t magFilter;
    int32_t minFilter;
    int32_t mipmapMode;
    int32_t addressModeU;
    int32_t addressModeV;
    int32_t addressModeW;
    float mipLodBias;
    uint32_t anisotropyEnable;
    float maxAnisotropy;
    uint32_t compareEnable;
    int32_t compareOp;
    float minLod;
    float maxLod;
    int32_t borderColor;
    uint32_t unnormalizedCoordinates;
} rlvk_sampler_create_info;

/** @brief VkWriteDescriptorSet. */
typedef struct rlvk_write_descriptor_set {
    int32_t sType;
    const void *pNext;
    rlvk_handle dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    int32_t descriptorType;
    const void *pImageInfo;
    const rlvk_descriptor_buffer_info *pBufferInfo;
    const rlvk_handle *pTexelBufferView;
} rlvk_write_descriptor_set;

/** @brief VkCommandPoolCreateInfo. */
typedef struct rlvk_command_pool_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    uint32_t queueFamilyIndex;
} rlvk_command_pool_create_info;

/** @brief VkCommandBufferAllocateInfo. */
typedef struct rlvk_command_buffer_allocate_info {
    int32_t sType;
    const void *pNext;
    rlvk_handle commandPool;
    int32_t level;
    uint32_t commandBufferCount;
} rlvk_command_buffer_allocate_info;

/** @brief VkCommandBufferBeginInfo. */
typedef struct rlvk_command_buffer_begin_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    const void *pInheritanceInfo;
} rlvk_command_buffer_begin_info;

/** @brief VkMemoryBarrier. */
typedef struct rlvk_memory_barrier {
    int32_t sType;
    const void *pNext;
    rlvk_flags srcAccessMask;
    rlvk_flags dstAccessMask;
} rlvk_memory_barrier;

/** @brief VkBufferCopy. */
typedef struct rlvk_buffer_copy {
    rlvk_size srcOffset;
    rlvk_size dstOffset;
    rlvk_size size;
} rlvk_buffer_copy;

/** @brief VkSubmitInfo. */
typedef struct rlvk_submit_info {
    int32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const rlvk_handle *pWaitSemaphores;
    const rlvk_flags *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const rlvk_handle *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const rlvk_handle *pSignalSemaphores;
} rlvk_submit_info;

/** @brief VkFenceCreateInfo. */
typedef struct rlvk_fence_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
} rlvk_fence_create_info;

/** @brief VkExtent2D. */
typedef struct rlvk_extent2d {
    uint32_t width, height;
} rlvk_extent2d;

/** @brief VkOffset3D. */
typedef struct rlvk_offset3d {
    int32_t x, y, z;
} rlvk_offset3d;

/** @brief VkSurfaceCapabilitiesKHR. */
typedef struct rlvk_surface_capabilities {
    uint32_t minImageCount;
    uint32_t maxImageCount;
    rlvk_extent2d currentExtent;
    rlvk_extent2d minImageExtent;
    rlvk_extent2d maxImageExtent;
    uint32_t maxImageArrayLayers;
    rlvk_flags supportedTransforms;
    rlvk_flags currentTransform;
    rlvk_flags supportedCompositeAlpha;
    rlvk_flags supportedUsageFlags;
} rlvk_surface_capabilities;

/** @brief VkSurfaceFormatKHR. */
typedef struct rlvk_surface_format {
    int32_t format;
    int32_t colorSpace;
} rlvk_surface_format;

/** @brief VkSwapchainCreateInfoKHR. */
typedef struct rlvk_swapchain_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    rlvk_handle surface;
    uint32_t minImageCount;
    int32_t imageFormat;
    int32_t imageColorSpace;
    rlvk_extent2d imageExtent;
    uint32_t imageArrayLayers;
    rlvk_flags imageUsage;
    int32_t imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    rlvk_flags preTransform;
    rlvk_flags compositeAlpha;
    int32_t presentMode;
    uint32_t clipped;
    rlvk_handle oldSwapchain;
} rlvk_swapchain_create_info;

/** @brief VkPresentInfoKHR. */
typedef struct rlvk_present_info {
    int32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const rlvk_handle *pWaitSemaphores;
    uint32_t swapchainCount;
    const rlvk_handle *pSwapchains;
    const uint32_t *pImageIndices;
    rlvk_result *pResults;
} rlvk_present_info;

/** @brief VkImageCreateInfo. */
typedef struct rlvk_image_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
    int32_t imageType;
    int32_t format;
    rlvk_extent3d extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    rlvk_flags samples;
    int32_t tiling;
    rlvk_flags usage;
    int32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    int32_t initialLayout;
} rlvk_image_create_info;

/** @brief VkImageSubresourceRange. */
typedef struct rlvk_image_subresource_range {
    rlvk_flags aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} rlvk_image_subresource_range;

/** @brief VkImageMemoryBarrier. */
typedef struct rlvk_image_memory_barrier {
    int32_t sType;
    const void *pNext;
    rlvk_flags srcAccessMask;
    rlvk_flags dstAccessMask;
    int32_t oldLayout;
    int32_t newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    rlvk_handle image;
    rlvk_image_subresource_range subresourceRange;
} rlvk_image_memory_barrier;

/** @brief VkImageSubresourceLayers. */
typedef struct rlvk_image_subresource_layers {
    rlvk_flags aspectMask;
    uint32_t mipLevel;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} rlvk_image_subresource_layers;

/** @brief VkBufferImageCopy. */
typedef struct rlvk_buffer_image_copy {
    rlvk_size bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    rlvk_image_subresource_layers imageSubresource;
    rlvk_offset3d imageOffset;
    rlvk_extent3d imageExtent;
} rlvk_buffer_image_copy;

/** @brief VkImageCopy. */
typedef struct rlvk_image_copy {
    rlvk_image_subresource_layers srcSubresource;
    rlvk_offset3d srcOffset;
    rlvk_image_subresource_layers dstSubresource;
    rlvk_offset3d dstOffset;
    rlvk_extent3d extent;
} rlvk_image_copy;

/** @brief VkImageBlit. */
typedef struct rlvk_image_blit {
    rlvk_image_subresource_layers srcSubresource;
    rlvk_offset3d srcOffsets[2];
    rlvk_image_subresource_layers dstSubresource;
    rlvk_offset3d dstOffsets[2];
} rlvk_image_blit;

/** @brief VkClearColorValue. */
typedef union rlvk_clear_color {
    float float32[4];
    int32_t int32[4];
    uint32_t uint32[4];
} rlvk_clear_color;

/** @brief VkSemaphoreCreateInfo. */
typedef struct rlvk_semaphore_create_info {
    int32_t sType;
    const void *pNext;
    rlvk_flags flags;
} rlvk_semaphore_create_info;

/** @brief Every Vulkan 1.0 function the core calls, as X(name, return type, parameters); the first group from the instance. */
#define RLVK_INSTANCE_FNS(X)                                                                                         \
    X(vkDestroyInstance, void, (rlvk_handle, const void *))                                                         \
    X(vkEnumeratePhysicalDevices, rlvk_result, (rlvk_handle, uint32_t *, rlvk_handle *))                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties, void, (rlvk_handle, uint32_t *, rlvk_queue_family_properties *))    \
    X(vkGetPhysicalDeviceMemoryProperties, void, (rlvk_handle, rlvk_memory_properties *))                           \
    X(vkEnumerateDeviceExtensionProperties, rlvk_result,                                                            \
      (rlvk_handle, const char *, uint32_t *, rlvk_extension_properties *))                                         \
    X(vkCreateDevice, rlvk_result, (rlvk_handle, const rlvk_device_create_info *, const void *, rlvk_handle *))     \
    X(vkGetDeviceProcAddr, void *, (rlvk_handle, const char *))

/** @brief The Vulkan 1.0 functions the core loads from the device. */
#define RLVK_DEVICE_FNS(X)                                                                                           \
    X(vkDestroyDevice, void, (rlvk_handle, const void *))                                                           \
    X(vkGetDeviceQueue, void, (rlvk_handle, uint32_t, uint32_t, rlvk_handle *))                                     \
    X(vkCreateBuffer, rlvk_result, (rlvk_handle, const rlvk_buffer_create_info *, const void *, rlvk_handle *))     \
    X(vkDestroyBuffer, void, (rlvk_handle, rlvk_handle, const void *))                                              \
    X(vkGetBufferMemoryRequirements, void, (rlvk_handle, rlvk_handle, rlvk_memory_requirements *))                  \
    X(vkAllocateMemory, rlvk_result, (rlvk_handle, const rlvk_memory_allocate_info *, const void *, rlvk_handle *)) \
    X(vkFreeMemory, void, (rlvk_handle, rlvk_handle, const void *))                                                 \
    X(vkBindBufferMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_handle, rlvk_size))                          \
    X(vkMapMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_size, rlvk_size, rlvk_flags, void **))              \
    X(vkUnmapMemory, void, (rlvk_handle, rlvk_handle))                                                              \
    X(vkFlushMappedMemoryRanges, rlvk_result, (rlvk_handle, uint32_t, const rlvk_mapped_memory_range *))            \
    X(vkInvalidateMappedMemoryRanges, rlvk_result, (rlvk_handle, uint32_t, const rlvk_mapped_memory_range *))       \
    X(vkCreateShaderModule, rlvk_result,                                                                            \
      (rlvk_handle, const rlvk_shader_module_create_info *, const void *, rlvk_handle *))                           \
    X(vkDestroyShaderModule, void, (rlvk_handle, rlvk_handle, const void *))                                        \
    X(vkCreateDescriptorSetLayout, rlvk_result,                                                                     \
      (rlvk_handle, const rlvk_descriptor_set_layout_create_info *, const void *, rlvk_handle *))                   \
    X(vkDestroyDescriptorSetLayout, void, (rlvk_handle, rlvk_handle, const void *))                                 \
    X(vkCreatePipelineLayout, rlvk_result,                                                                          \
      (rlvk_handle, const rlvk_pipeline_layout_create_info *, const void *, rlvk_handle *))                         \
    X(vkDestroyPipelineLayout, void, (rlvk_handle, rlvk_handle, const void *))                                      \
    X(vkCreateComputePipelines, rlvk_result,                                                                        \
      (rlvk_handle, rlvk_handle, uint32_t, const rlvk_compute_pipeline_create_info *, const void *, rlvk_handle *)) \
    X(vkDestroyPipeline, void, (rlvk_handle, rlvk_handle, const void *))                                            \
    X(vkCreateDescriptorPool, rlvk_result,                                                                          \
      (rlvk_handle, const rlvk_descriptor_pool_create_info *, const void *, rlvk_handle *))                         \
    X(vkDestroyDescriptorPool, void, (rlvk_handle, rlvk_handle, const void *))                                      \
    X(vkAllocateDescriptorSets, rlvk_result, (rlvk_handle, const rlvk_descriptor_set_allocate_info *, rlvk_handle *)) \
    X(vkUpdateDescriptorSets, void,                                                                                 \
      (rlvk_handle, uint32_t, const rlvk_write_descriptor_set *, uint32_t, const void *))                           \
    X(vkCreateCommandPool, rlvk_result,                                                                             \
      (rlvk_handle, const rlvk_command_pool_create_info *, const void *, rlvk_handle *))                            \
    X(vkDestroyCommandPool, void, (rlvk_handle, rlvk_handle, const void *))                                         \
    X(vkAllocateCommandBuffers, rlvk_result, (rlvk_handle, const rlvk_command_buffer_allocate_info *, rlvk_handle *)) \
    X(vkBeginCommandBuffer, rlvk_result, (rlvk_handle, const rlvk_command_buffer_begin_info *))                     \
    X(vkEndCommandBuffer, rlvk_result, (rlvk_handle))                                                               \
    X(vkResetCommandBuffer, rlvk_result, (rlvk_handle, rlvk_flags))                                                 \
    X(vkCmdBindPipeline, void, (rlvk_handle, int32_t, rlvk_handle))                                                 \
    X(vkCmdBindDescriptorSets, void,                                                                                \
      (rlvk_handle, int32_t, rlvk_handle, uint32_t, uint32_t, const rlvk_handle *, uint32_t, const uint32_t *))     \
    X(vkCmdPushConstants, void, (rlvk_handle, rlvk_handle, rlvk_flags, uint32_t, uint32_t, const void *))           \
    X(vkCmdDispatch, void, (rlvk_handle, uint32_t, uint32_t, uint32_t))                                             \
    X(vkCmdCopyBuffer, void, (rlvk_handle, rlvk_handle, rlvk_handle, uint32_t, const rlvk_buffer_copy *))           \
    X(vkCmdPipelineBarrier, void,                                                                                   \
      (rlvk_handle, rlvk_flags, rlvk_flags, rlvk_flags, uint32_t, const rlvk_memory_barrier *, uint32_t,           \
       const void *, uint32_t, const void *))                                                                       \
    X(vkQueueSubmit, rlvk_result, (rlvk_handle, uint32_t, const rlvk_submit_info *, rlvk_handle))                   \
    X(vkCreateFence, rlvk_result, (rlvk_handle, const rlvk_fence_create_info *, const void *, rlvk_handle *))       \
    X(vkDestroyFence, void, (rlvk_handle, rlvk_handle, const void *))                                               \
    X(vkWaitForFences, rlvk_result, (rlvk_handle, uint32_t, const rlvk_handle *, uint32_t, uint64_t))               \
    X(vkResetFences, rlvk_result, (rlvk_handle, uint32_t, const rlvk_handle *))                                   \
    X(vkDeviceWaitIdle, rlvk_result, (rlvk_handle))                                                                 \
    X(vkCreateImage, rlvk_result, (rlvk_handle, const rlvk_image_create_info *, const void *, rlvk_handle *))       \
    X(vkDestroyImage, void, (rlvk_handle, rlvk_handle, const void *))                                               \
    X(vkGetImageMemoryRequirements, void, (rlvk_handle, rlvk_handle, rlvk_memory_requirements *))                   \
    X(vkBindImageMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_handle, rlvk_size))                           \
    X(vkCmdCopyBufferToImage, void,                                                                                 \
      (rlvk_handle, rlvk_handle, rlvk_handle, int32_t, uint32_t, const rlvk_buffer_image_copy *))                   \
    X(vkCmdCopyImageToBuffer, void,                                                                                 \
      (rlvk_handle, rlvk_handle, int32_t, rlvk_handle, uint32_t, const rlvk_buffer_image_copy *))                   \
    X(vkCmdCopyImage, void,                                                                                         \
      (rlvk_handle, rlvk_handle, int32_t, rlvk_handle, int32_t, uint32_t, const rlvk_image_copy *))                 \
    X(vkCmdBlitImage, void,                                                                                         \
      (rlvk_handle, rlvk_handle, int32_t, rlvk_handle, int32_t, uint32_t, const rlvk_image_blit *, int32_t))        \
    X(vkCmdClearColorImage, void,                                                                                   \
      (rlvk_handle, rlvk_handle, int32_t, const rlvk_clear_color *, uint32_t, const rlvk_image_subresource_range *)) \
    X(vkCreateSemaphore, rlvk_result, (rlvk_handle, const rlvk_semaphore_create_info *, const void *, rlvk_handle *)) \
    X(vkDestroySemaphore, void, (rlvk_handle, rlvk_handle, const void *))

/** @brief VK_KHR_surface's functions, loaded from an instance made with it. */
#define RLVK_SURFACE_FNS(X)                                                                                          \
    X(vkDestroySurfaceKHR, void, (rlvk_handle, rlvk_handle, const void *))                                          \
    X(vkGetPhysicalDeviceSurfaceSupportKHR, rlvk_result, (rlvk_handle, uint32_t, rlvk_handle, uint32_t *))          \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_surface_capabilities *)) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR, rlvk_result, (rlvk_handle, rlvk_handle, uint32_t *, rlvk_surface_format *)) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR, rlvk_result, (rlvk_handle, rlvk_handle, uint32_t *, int32_t *))

/** @brief VK_KHR_swapchain's functions, loaded from a device made with it. */
#define RLVK_SWAPCHAIN_FNS(X)                                                                                        \
    X(vkCreateSwapchainKHR, rlvk_result, (rlvk_handle, const rlvk_swapchain_create_info *, const void *, rlvk_handle *)) \
    X(vkDestroySwapchainKHR, void, (rlvk_handle, rlvk_handle, const void *))                                        \
    X(vkGetSwapchainImagesKHR, rlvk_result, (rlvk_handle, rlvk_handle, uint32_t *, rlvk_handle *))                  \
    X(vkAcquireNextImageKHR, rlvk_result, (rlvk_handle, rlvk_handle, uint64_t, rlvk_handle, rlvk_handle, uint32_t *)) \
    X(vkQueuePresentKHR, rlvk_result, (rlvk_handle, const rlvk_present_info *))

/** @brief vkGetInstanceProcAddr and vkCreateInstance, which the loader gives before an instance exists. */
typedef void *(RLVK_CALL *rlvk_gipa_fn)(rlvk_handle instance, const char *name);
typedef rlvk_result(RLVK_CALL *rlvk_create_instance_fn)(const rlvk_instance_create_info *, const void *,
                                                         rlvk_handle *);
/** @brief vkEnumerateInstanceExtensionProperties. */
typedef rlvk_result(RLVK_CALL *rlvk_instance_extensions_fn)(const char *, uint32_t *, rlvk_extension_properties *);

#endif
