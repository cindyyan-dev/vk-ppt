#pragma once

#include <vulkan/vulkan.h>

#define PPT_EXPORT extern "C" __attribute__((visibility("default")))

PPT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name);

namespace ppt {

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice device,
        const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers);
VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice device, VkCommandPool pool,
        uint32_t count, const VkCommandBuffer* buffers);
VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice device, VkCommandPool pool,
        const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer buffer,
        const VkCommandBufferBeginInfo* info);
VKAPI_ATTR VkResult VKAPI_CALL resetCommandBuffer(VkCommandBuffer buffer, VkCommandBufferResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL resetCommandPool(VkDevice handle, VkCommandPool pool,
        VkCommandPoolResetFlags flags);
VKAPI_ATTR void VKAPI_CALL cmdDraw(VkCommandBuffer buffer, uint32_t vertices, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance);
VKAPI_ATTR void VKAPI_CALL cmdDrawIndexed(VkCommandBuffer buffer, uint32_t indices, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
VKAPI_ATTR VkResult VKAPI_CALL createGraphicsPipelines(VkDevice handle, VkPipelineCache cache,
        uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
        VkPipeline* pipelines);
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice handle, VkPipeline pipeline, const VkAllocationCallbacks* allocator);
VKAPI_ATTR void VKAPI_CALL cmdBindPipeline(VkCommandBuffer buffer, VkPipelineBindPoint point, VkPipeline pipeline);
VKAPI_ATTR void VKAPI_CALL cmdSetPrimitiveTopology(VkCommandBuffer buffer, VkPrimitiveTopology topology);
VKAPI_ATTR void VKAPI_CALL cmdSetPrimitiveTopologyEXT(VkCommandBuffer buffer, VkPrimitiveTopology topology);
VKAPI_ATTR void VKAPI_CALL cmdBindShadersEXT(VkCommandBuffer buffer, uint32_t count,
        const VkShaderStageFlagBits* stages, const VkShaderEXT* shaders);

}
