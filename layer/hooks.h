#pragma once

#include <vulkan/vulkan.h>

namespace ppt {

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice device,
        const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers);
VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice device, VkCommandPool pool,
        uint32_t count, const VkCommandBuffer* buffers);
VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice device, VkCommandPool pool,
        const VkAllocationCallbacks* allocator);

}
