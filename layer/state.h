#pragma once

#include "primitives.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ppt {

// Filled once when the instance or device is created,
// not looked up by name on every call.
#define PPT_INSTANCE_FUNCS(X) X(DestroyInstance) X(EnumerateDeviceExtensionProperties)
#define PPT_DEVICE_FUNCS(X) \
    X(DestroyDevice) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(DestroyCommandPool) \
    X(BeginCommandBuffer) X(ResetCommandBuffer) X(ResetCommandPool) \
    X(CreateGraphicsPipelines) X(DestroyPipeline) X(CmdBindPipeline) X(CmdDraw) X(CmdDrawIndexed) \
    X(CmdSetPrimitiveTopology) X(CmdSetPrimitiveTopologyEXT) X(CmdBindShadersEXT)

struct InstanceFns {
#define X(name) PFN_vk##name name = nullptr;
    PPT_INSTANCE_FUNCS(X)
#undef X
};
struct DeviceFns {
#define X(name) PFN_vk##name name = nullptr;
    PPT_DEVICE_FUNCS(X)
#undef X
};

struct InstanceWrapper {
    VkInstance handle;
    // Forward function lookups to the next layer.
    PFN_vkGetInstanceProcAddr getProcAddr;
    InstanceFns next;
    InstanceWrapper(VkInstance vkHandle, PFN_vkGetInstanceProcAddr vkGetProcAddr, const InstanceFns& vkNext)
        : handle(vkHandle), getProcAddr(vkGetProcAddr), next(vkNext) {}
};

struct CommandBufferWrapper {
    struct Recording {
        InputAssembly assembly;
        VkPrimitiveTopology dynamicTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        std::vector<std::optional<uint64_t>> drawPrimitiveCounts;
        bool drawCountsComplete = true;

        void clear() {
            assembly = {};
            dynamicTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
            drawPrimitiveCounts.clear();
            drawCountsComplete = true;
        }
    };

    VkCommandPool pool;
    Recording recording;

    explicit CommandBufferWrapper(VkCommandPool vkPool) : pool(vkPool) {}
};

struct DeviceWrapper {
    // Forward function lookups to the next layer.
    PFN_vkGetDeviceProcAddr getProcAddr;
    DeviceFns next;
    // Key by the full handle: command buffers share the device's dispatch table.
    std::unordered_map<VkCommandBuffer, std::unique_ptr<CommandBufferWrapper>> commandBuffers;
    std::unordered_map<VkPipeline, InputAssembly> pipelines;
    DeviceWrapper(PFN_vkGetDeviceProcAddr vkGetProcAddr, const DeviceFns& vkNext)
        : getProcAddr(vkGetProcAddr), next(vkNext) {}
};

// A physical device shares its instance's dispatch table. Key by that table so
// vkCreateDevice can find the next instance entry point from a physical device.
template <typename Handle> void* dispatchKey(Handle handle) {
    return *reinterpret_cast<void**>(handle);
}

extern std::mutex stateMutex;
DeviceWrapper* findDevice(void* key);

}
