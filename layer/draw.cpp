#include "hooks.h"
#include "state.h"
#include <new>
#include <utility>

namespace ppt {

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice device,
        const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    auto* record = findDevice(dispatchKey(device));
    if (!record) {
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) buffers[i] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult result = record->next.AllocateCommandBuffers(device, info, buffers);
    if (result != VK_SUCCESS || info->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return result;
    uint32_t registered = 0;
    try {
        std::lock_guard<std::mutex> guard(stateMutex);
        for (; registered < info->commandBufferCount; ++registered) {
            auto buffer = std::make_unique<CommandBufferWrapper>(info->commandPool);
            // Live command buffer handles are unique within the device.
            record->commandBuffers.emplace(buffers[registered], std::move(buffer));
        }
    } catch (const std::bad_alloc&) {
        {
            std::lock_guard<std::mutex> guard(stateMutex);
            for (uint32_t i = 0; i < registered; ++i) record->commandBuffers.erase(buffers[i]);
        }
        // Roll back this entire downstream batch, leaving older allocations intact.
        record->next.FreeCommandBuffers(device, info->commandPool, info->commandBufferCount, buffers);
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) buffers[i] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice device, VkCommandPool pool,
        uint32_t count, const VkCommandBuffer* buffers) {
    auto* record = findDevice(dispatchKey(device));
    if (!record) return;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        for (uint32_t i = 0; i < count; ++i) record->commandBuffers.erase(buffers[i]);
    }
    record->next.FreeCommandBuffers(device, pool, count, buffers);
}

VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice device, VkCommandPool pool,
        const VkAllocationCallbacks* allocator) {
    auto* record = findDevice(dispatchKey(device));
    if (!record) return;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        for (auto it = record->commandBuffers.begin(); it != record->commandBuffers.end();) {
            if (it->second->pool == pool) it = record->commandBuffers.erase(it);
            else ++it;
        }
    }
    record->next.DestroyCommandPool(device, pool, allocator);
}

}
