#include "hooks.h"
#include "state.h"
#include <new>

namespace ppt {

VKAPI_ATTR VkResult VKAPI_CALL createGraphicsPipelines(VkDevice handle, VkPipelineCache cache,
        uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
        VkPipeline* pipelines) {
    auto* device = findDevice(dispatchKey(handle));
    if (!device) {
        for (uint32_t i = 0; i < count; ++i) pipelines[i] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult result = device->next.CreateGraphicsPipelines(handle, cache, count, infos, allocator, pipelines);
    try {
        std::lock_guard<std::mutex> guard(stateMutex);
        for (uint32_t i = 0; i < count; ++i) {
            if (pipelines[i]) device->pipelines.emplace(pipelines[i], pipelineAssembly(infos[i]));
        }
    } catch (const std::bad_alloc&) {
        // Pipelines left out of the map count as unknown topology.
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice handle, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    auto* device = findDevice(dispatchKey(handle));
    if (!device) return;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        device->pipelines.erase(pipeline);
    }
    device->next.DestroyPipeline(handle, pipeline, allocator);
}

VKAPI_ATTR void VKAPI_CALL cmdBindPipeline(VkCommandBuffer buffer, VkPipelineBindPoint point, VkPipeline pipeline) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return;
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto cb = device->commandBuffers.find(buffer);
        if (cb != device->commandBuffers.end()) {
            auto it = device->pipelines.find(pipeline);
            cb->second->recording.assembly = it == device->pipelines.end() ? InputAssembly{} : it->second;
        }
    }
    device->next.CmdBindPipeline(buffer, point, pipeline);
}

static void recordPrimitiveTopology(DeviceWrapper* device, VkCommandBuffer buffer, VkPrimitiveTopology topology) {
    std::lock_guard<std::mutex> guard(stateMutex);
    auto cb = device->commandBuffers.find(buffer);
    if (cb != device->commandBuffers.end()) cb->second->recording.dynamicTopology = topology;
}

VKAPI_ATTR void VKAPI_CALL cmdSetPrimitiveTopology(VkCommandBuffer buffer, VkPrimitiveTopology topology) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return;
    recordPrimitiveTopology(device, buffer, topology);
    device->next.CmdSetPrimitiveTopology(buffer, topology);
}

VKAPI_ATTR void VKAPI_CALL cmdSetPrimitiveTopologyEXT(VkCommandBuffer buffer, VkPrimitiveTopology topology) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return;
    recordPrimitiveTopology(device, buffer, topology);
    device->next.CmdSetPrimitiveTopologyEXT(buffer, topology);
}

VKAPI_ATTR void VKAPI_CALL cmdBindShadersEXT(VkCommandBuffer buffer, uint32_t count,
        const VkShaderStageFlagBits* stages, const VkShaderEXT* shaders) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (stages[i] == VK_SHADER_STAGE_COMPUTE_BIT) continue;
        std::lock_guard<std::mutex> guard(stateMutex);
        auto cb = device->commandBuffers.find(buffer);
        if (cb != device->commandBuffers.end()) cb->second->recording.assembly = {};
        break;
    }
    device->next.CmdBindShadersEXT(buffer, count, stages, shaders);
}

}
