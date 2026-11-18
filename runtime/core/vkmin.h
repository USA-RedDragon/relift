/**
 * @file vkmin.h
 * @brief The part of the Vulkan 1.0 API the core's picture pipeline (post.c)
 * uses, declared here so the core builds on every host without Vulkan
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
enum { RLVK_SUCCESS = 0, RLVK_INCOMPLETE = 5, RLVK_ERROR_EXTENSION_NOT_PRESENT = -7 };

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
    RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO = 33,
    RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    RLVK_ST_WRITE_DESCRIPTOR_SET = 35,
    RLVK_ST_COMMAND_POOL_CREATE_INFO = 39,
    RLVK_ST_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    RLVK_ST_COMMAND_BUFFER_BEGIN_INFO = 42,
    RLVK_ST_MEMORY_BARRIER = 46,
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
    RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
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

/** @brief Every function post.c calls, as X(name, return type, parameters); the first group from the instance. */
#define RLVK_INSTANCE_FNS(X)                                                                                         \
    X(vkDestroyInstance, void, (rlvk_handle, const void *))                                                         \
    X(vkEnumeratePhysicalDevices, rlvk_result, (rlvk_handle, uint32_t *, rlvk_handle *))                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties, void, (rlvk_handle, uint32_t *, rlvk_queue_family_properties *))    \
    X(vkGetPhysicalDeviceMemoryProperties, void, (rlvk_handle, rlvk_memory_properties *))                           \
    X(vkEnumerateDeviceExtensionProperties, rlvk_result,                                                            \
      (rlvk_handle, const char *, uint32_t *, rlvk_extension_properties *))                                         \
    X(vkCreateDevice, rlvk_result, (rlvk_handle, const rlvk_device_create_info *, const void *, rlvk_handle *))     \
    X(vkGetDeviceProcAddr, void *, (rlvk_handle, const char *))

/** @brief The functions post.c loads from the device. */
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
    X(vkResetFences, rlvk_result, (rlvk_handle, uint32_t, const rlvk_handle *))

/** @brief vkGetInstanceProcAddr and vkCreateInstance, which the loader gives before an instance exists. */
typedef void *(RLVK_CALL *rlvk_gipa_fn)(rlvk_handle instance, const char *name);
typedef rlvk_result(RLVK_CALL *rlvk_create_instance_fn)(const rlvk_instance_create_info *, const void *,
                                                         rlvk_handle *);

#endif
