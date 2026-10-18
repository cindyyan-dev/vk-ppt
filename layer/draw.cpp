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

VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer buffer,
        const VkCommandBufferBeginInfo* info) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = device->next.BeginCommandBuffer(buffer, info);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto it = device->commandBuffers.find(buffer);
        if (it != device->commandBuffers.end()) {
            it->second->recording.clear();
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL resetCommandBuffer(VkCommandBuffer buffer, VkCommandBufferResetFlags flags) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkResult result = device->next.ResetCommandBuffer(buffer, flags);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto it = device->commandBuffers.find(buffer);
        if (it != device->commandBuffers.end()) it->second->recording.clear();
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL resetCommandPool(VkDevice handle, VkCommandPool pool,
        VkCommandPoolResetFlags flags) {
    auto* device = findDevice(dispatchKey(handle));
    if (!device) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkResult result = device->next.ResetCommandPool(handle, pool, flags);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        for (auto& entry : device->commandBuffers) {
            if (entry.second->pool == pool) {
                entry.second->recording.clear();
            }
        }
    }
    return result;
}

static void recordDraw(DeviceWrapper* device, VkCommandBuffer buffer, uint32_t count, uint32_t instanceCount, bool indexed) {
    std::lock_guard<std::mutex> guard(stateMutex);
    auto it = device->commandBuffers.find(buffer);
    if (it == device->commandBuffers.end()) return;
    auto& recording = it->second->recording;
    if (!recording.drawCountsComplete) return;
    auto topology = recording.assembly.topologyIsDynamic ? recording.dynamicTopology : recording.assembly.topology;
    try {
        recording.drawPrimitiveCounts.push_back(primitiveCount(topology, count, instanceCount, indexed && recording.assembly.mayRestart));
    } catch (const std::bad_alloc&) {
        recording.drawCountsComplete = false;
    }
}

VKAPI_ATTR void VKAPI_CALL cmdDraw(VkCommandBuffer buffer, uint32_t vertices, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return;
    recordDraw(device, buffer, vertices, instanceCount, false);
    device->next.CmdDraw(buffer, vertices, instanceCount, firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL cmdDrawIndexed(VkCommandBuffer buffer, uint32_t indices, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return;
    recordDraw(device, buffer, indices, instanceCount, true);
    device->next.CmdDrawIndexed(buffer, indices, instanceCount, firstIndex, vertexOffset, firstInstance);
}

}
