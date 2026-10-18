// Android loads the exported functions directly from the shared library. The loader's
// create-info links supply the next layer's entry points; ordinary commands keep using
// those entry points without an extra wrapper in this layer.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "primitives.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

#define PPT_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
constexpr const char* kLayerName = "VK_LAYER_PPT_perdraw";

struct InstanceWrapper {
    VkInstance handle;
    // Forward function lookups to the next layer.
    PFN_vkGetInstanceProcAddr getProcAddr;
    // Keep the next layer's destroy function so we can call it
    // after removing this wrapper from the map.
    PFN_vkDestroyInstance destroy;
    InstanceWrapper(VkInstance vkHandle, PFN_vkGetInstanceProcAddr vkGetProcAddr, PFN_vkDestroyInstance vkDestroy)
        : handle(vkHandle), getProcAddr(vkGetProcAddr), destroy(vkDestroy) {}
};
struct CommandBufferWrapper {
    enum class RecordingState { Initial, Recording, Executable, Invalid };
    VkCommandPool pool;
    RecordingState recordingState = RecordingState::Initial;
    InputAssembly assembly;
    VkPrimitiveTopology dynamicTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    explicit CommandBufferWrapper(VkCommandPool vkPool) : pool(vkPool) {}
};

struct DeviceWrapper {
    VkDevice handle;
    // Forward function lookups to the next layer.
    PFN_vkGetDeviceProcAddr getProcAddr;
    // Keep the next layer's destroy function so we can call it
    // after removing this wrapper from the map.
    PFN_vkDestroyDevice destroy;
    // Key by the full handle: command buffers share the device's dispatch table.
    std::unordered_map<VkCommandBuffer, std::unique_ptr<CommandBufferWrapper>> commandBuffers;
    std::unordered_map<VkPipeline, InputAssembly> pipelines;
    bool tracePrimitives;
    DeviceWrapper(VkDevice vkHandle, PFN_vkGetDeviceProcAddr vkGetProcAddr, PFN_vkDestroyDevice vkDestroy)
        : handle(vkHandle), getProcAddr(vkGetProcAddr), destroy(vkDestroy) {
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.ppt.primitives", value);
        tracePrimitives = std::strcmp(value, "1") == 0;
    }
};

// A physical device shares its instance's dispatch table. Key by that table so
// vkCreateDevice can find the next instance entry point from a physical device.
template <typename Handle> void* dispatchKey(Handle handle) {
    return *reinterpret_cast<void**>(handle);
}

std::mutex stateMutex;
std::unordered_map<void*, std::unique_ptr<InstanceWrapper>> instances;
std::unordered_map<void*, std::unique_ptr<DeviceWrapper>> devices;

template <typename Handle> InstanceWrapper* findInstance(Handle instance) {
    std::lock_guard<std::mutex> guard(stateMutex);
    auto it = instances.find(dispatchKey(instance));
    return it == instances.end() ? nullptr : it->second.get();
}
DeviceWrapper* findDevice(void* key) {
    std::lock_guard<std::mutex> guard(stateMutex);
    auto it = devices.find(key);
    return it == devices.end() ? nullptr : it->second.get();
}

template <typename Info> Info* findLink(const void* next, VkStructureType type) {
    auto* node = reinterpret_cast<Info*>(const_cast<void*>(next));
    while (node) {
        if (node->sType == type && node->function == VK_LAYER_LINK_INFO) return node;
        node = reinterpret_cast<Info*>(const_cast<void*>(node->pNext));
    }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkInstance* instance) {
    auto* link = findLink<VkLayerInstanceCreateInfo>(info->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto getProcAddr = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(getProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    // move chain on for next layer
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS) return result;
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(getProcAddr(*instance, "vkDestroyInstance"));
    try {
        std::unique_ptr<InstanceWrapper> record = std::make_unique<InstanceWrapper>(*instance, getProcAddr, destroy);
        std::lock_guard<std::mutex> guard(stateMutex);
        // This key should not already be in the map.
        // If duplicate keys become possible, update this code to handle them.
        instances.emplace(dispatchKey(*instance), std::move(record));
    } catch (const std::bad_alloc&) {
        // The lock has unwound; roll back the downstream object as well as our record.
        destroy(*instance, allocator);
        *instance = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    if (!instance) return;
    std::unique_ptr<InstanceWrapper> record;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto it = instances.find(dispatchKey(instance));
        if (it == instances.end()) return;
        record = std::move(it->second);
        instances.erase(it);
    }
    record->destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* link = findLink<VkLayerDeviceCreateInfo>(info->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto getInstanceProcAddr = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto getDeviceProcAddr = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto* parent = findInstance(physicalDevice);
    if (!parent) return VK_ERROR_INITIALIZATION_FAILED;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(parent->handle, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(physicalDevice, info, allocator, device);
    if (result != VK_SUCCESS) return result;
    auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(getDeviceProcAddr(*device, "vkDestroyDevice"));
    try {
        std::unique_ptr<DeviceWrapper> record = std::make_unique<DeviceWrapper>(*device, getDeviceProcAddr, destroy);
        std::lock_guard<std::mutex> guard(stateMutex);
        // This key should not already be in the map.
        // If duplicate keys become possible, update this code to handle them.
        devices.emplace(dispatchKey(*device), std::move(record));
    } catch (const std::bad_alloc&) {
        destroy(*device, allocator);
        *device = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    if (!device) return;
    std::unique_ptr<DeviceWrapper> record;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto it = devices.find(dispatchKey(device));
        if (it == devices.end()) return;
        record = std::move(it->second);
        devices.erase(it);
    }
    record->commandBuffers.clear();
    record->destroy(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice device,
        const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    auto* record = findDevice(dispatchKey(device));
    if (!record) return VK_ERROR_INITIALIZATION_FAILED;
    auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(record->getProcAddr(device, "vkAllocateCommandBuffers"));
    VkResult result = allocate(device, info, buffers);
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
        auto free = reinterpret_cast<PFN_vkFreeCommandBuffers>(record->getProcAddr(device, "vkFreeCommandBuffers"));
        free(device, info->commandPool, info->commandBufferCount, buffers);
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
    auto free = reinterpret_cast<PFN_vkFreeCommandBuffers>(record->getProcAddr(device, "vkFreeCommandBuffers"));
    free(device, pool, count, buffers);
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
    auto destroy = reinterpret_cast<PFN_vkDestroyCommandPool>(record->getProcAddr(device, "vkDestroyCommandPool"));
    destroy(device, pool, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer buffer,
        const VkCommandBufferBeginInfo* info) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(device->getProcAddr(device->handle, "vkBeginCommandBuffer"));
    VkResult result = begin(buffer, info);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto it = device->commandBuffers.find(buffer);
        if (it != device->commandBuffers.end()) {
            it->second->assembly = {};
            it->second->dynamicTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
            it->second->recordingState = CommandBufferWrapper::RecordingState::Recording;
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL endCommandBuffer(VkCommandBuffer buffer) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(device->getProcAddr(device->handle, "vkEndCommandBuffer"));
    VkResult result = end(buffer);
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto it = device->commandBuffers.find(buffer);
        if (it != device->commandBuffers.end()) {
            it->second->recordingState = result == VK_SUCCESS
                ? CommandBufferWrapper::RecordingState::Executable : CommandBufferWrapper::RecordingState::Invalid;
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL resetCommandBuffer(VkCommandBuffer buffer, VkCommandBufferResetFlags flags) {
    auto* device = findDevice(dispatchKey(buffer));
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    auto reset = reinterpret_cast<PFN_vkResetCommandBuffer>(device->getProcAddr(device->handle, "vkResetCommandBuffer"));
    VkResult result = reset(buffer, flags);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto it = device->commandBuffers.find(buffer);
        if (it != device->commandBuffers.end()) it->second->recordingState = CommandBufferWrapper::RecordingState::Initial;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL resetCommandPool(VkDevice handle, VkCommandPool pool,
        VkCommandPoolResetFlags flags) {
    auto* device = findDevice(dispatchKey(handle));
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    auto reset = reinterpret_cast<PFN_vkResetCommandPool>(device->getProcAddr(handle, "vkResetCommandPool"));
    VkResult result = reset(handle, pool, flags);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        for (auto& entry : device->commandBuffers) {
            if (entry.second->pool == pool) {
                entry.second->recordingState = CommandBufferWrapper::RecordingState::Initial;
            }
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL createGraphicsPipelines(VkDevice handle, VkPipelineCache cache,
        uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
        VkPipeline* pipelines) {
    auto* device = findDevice(dispatchKey(handle));
    auto create = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(device->getProcAddr(handle, "vkCreateGraphicsPipelines"));
    VkResult result = create(handle, cache, count, infos, allocator, pipelines);
    if (device->tracePrimitives) {
        try {
            std::lock_guard<std::mutex> guard(stateMutex);
            for (uint32_t i = 0; i < count; ++i) {
                if (pipelines[i]) device->pipelines.emplace(pipelines[i], pipelineAssembly(infos[i]));
            }
        } catch (const std::bad_alloc&) {
            __android_log_print(ANDROID_LOG_WARN, "vk-ppt", "Could not record pipeline input assembly");
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice handle, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    auto* device = findDevice(dispatchKey(handle));
    if (device->tracePrimitives) {
        std::lock_guard<std::mutex> guard(stateMutex);
        device->pipelines.erase(pipeline);
    }
    auto destroy = reinterpret_cast<PFN_vkDestroyPipeline>(device->getProcAddr(handle, "vkDestroyPipeline"));
    destroy(handle, pipeline, allocator);
}

VKAPI_ATTR void VKAPI_CALL cmdBindPipeline(VkCommandBuffer buffer, VkPipelineBindPoint point, VkPipeline pipeline) {
    auto* device = findDevice(dispatchKey(buffer));
    if (device->tracePrimitives && point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto cb = device->commandBuffers.find(buffer);
        if (cb != device->commandBuffers.end()) {
            auto it = device->pipelines.find(pipeline);
            cb->second->assembly = it == device->pipelines.end() ? InputAssembly{} : it->second;
        }
    }
    auto bind = reinterpret_cast<PFN_vkCmdBindPipeline>(device->getProcAddr(device->handle, "vkCmdBindPipeline"));
    bind(buffer, point, pipeline);
}

template <bool ext>
VKAPI_ATTR void VKAPI_CALL cmdSetPrimitiveTopology(VkCommandBuffer buffer, VkPrimitiveTopology topology) {
    auto* device = findDevice(dispatchKey(buffer));
    if (device->tracePrimitives) {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto cb = device->commandBuffers.find(buffer);
        if (cb != device->commandBuffers.end()) cb->second->dynamicTopology = topology;
    }
    auto set = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(device->getProcAddr(device->handle,
        ext ? "vkCmdSetPrimitiveTopologyEXT" : "vkCmdSetPrimitiveTopology"));
    set(buffer, topology);
}

VKAPI_ATTR void VKAPI_CALL cmdBindShadersEXT(VkCommandBuffer buffer, uint32_t count,
        const VkShaderStageFlagBits* stages, const VkShaderEXT* shaders) {
    auto* device = findDevice(dispatchKey(buffer));
    if (device->tracePrimitives) {
        for (uint32_t i = 0; i < count; ++i) {
            if (stages[i] == VK_SHADER_STAGE_COMPUTE_BIT) continue;
            std::lock_guard<std::mutex> guard(stateMutex);
            auto cb = device->commandBuffers.find(buffer);
            if (cb != device->commandBuffers.end()) cb->second->assembly = {};
            break;
        }
    }
    auto bind = reinterpret_cast<PFN_vkCmdBindShadersEXT>(device->getProcAddr(device->handle, "vkCmdBindShadersEXT"));
    bind(buffer, count, stages, shaders);
}

void traceDraw(DeviceWrapper* device, VkCommandBuffer buffer, uint32_t count, uint32_t instancesCount, bool indexed) {
    std::optional<uint64_t> primitives;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        auto cb = device->commandBuffers.find(buffer);
        if (cb == device->commandBuffers.end() ||
            cb->second->recordingState != CommandBufferWrapper::RecordingState::Recording) return;
        const auto& assembly = cb->second->assembly;
        auto topology = assembly.dynamicTopology ? cb->second->dynamicTopology : assembly.topology;
        primitives = primitiveCount(topology, count, instancesCount, indexed && assembly.mayRestart);
    }
    const char* command = indexed ? "vkCmdDrawIndexed" : "vkCmdDraw";
    if (primitives) __android_log_print(ANDROID_LOG_INFO, "vk-ppt", "%s input primitives=%" PRIu64, command, *primitives);
    else __android_log_print(ANDROID_LOG_INFO, "vk-ppt", "%s input primitives=unknown", command);
}

VKAPI_ATTR void VKAPI_CALL cmdDraw(VkCommandBuffer buffer, uint32_t vertices, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance) {
    auto* device = findDevice(dispatchKey(buffer));
    if (device->tracePrimitives) traceDraw(device, buffer, vertices, instanceCount, false);
    auto draw = reinterpret_cast<PFN_vkCmdDraw>(device->getProcAddr(device->handle, "vkCmdDraw"));
    draw(buffer, vertices, instanceCount, firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL cmdDrawIndexed(VkCommandBuffer buffer, uint32_t indices, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    auto* device = findDevice(dispatchKey(buffer));
    if (device->tracePrimitives) traceDraw(device, buffer, indices, instanceCount, true);
    auto draw = reinterpret_cast<PFN_vkCmdDrawIndexed>(device->getProcAddr(device->handle, "vkCmdDrawIndexed"));
    draw(buffer, indices, instanceCount, firstIndex, vertexOffset, firstInstance);
}

PFN_vkVoidFunction drawHook(const char* name, PFN_vkVoidFunction next) {
    if (!next) return nullptr;
    if (!std::strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(createGraphicsPipelines);
    if (!std::strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(destroyPipeline);
    if (!std::strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(cmdBindPipeline);
    if (!std::strcmp(name, "vkCmdDraw")) return reinterpret_cast<PFN_vkVoidFunction>(cmdDraw);
    if (!std::strcmp(name, "vkCmdDrawIndexed")) return reinterpret_cast<PFN_vkVoidFunction>(cmdDrawIndexed);
    if (!std::strcmp(name, "vkCmdSetPrimitiveTopology")) return reinterpret_cast<PFN_vkVoidFunction>(cmdSetPrimitiveTopology<false>);
    if (!std::strcmp(name, "vkCmdSetPrimitiveTopologyEXT")) return reinterpret_cast<PFN_vkVoidFunction>(cmdSetPrimitiveTopology<true>);
    if (!std::strcmp(name, "vkCmdBindShadersEXT")) return reinterpret_cast<PFN_vkVoidFunction>(cmdBindShadersEXT);
    return next;
}

VkResult enumerateLayer(uint32_t* count, VkLayerProperties* properties) {
    if (!properties) { *count = 1; return VK_SUCCESS; }
    if (*count == 0) return VK_INCOMPLETE;
    VkLayerProperties layer{};
    std::strcpy(layer.layerName, kLayerName);
    layer.specVersion = VK_API_VERSION_1_1;
    layer.implementationVersion = 1;
    std::strcpy(layer.description, "Android Vulkan forwarding layer");
    *properties = layer;
    *count = 1;
    return VK_SUCCESS;
}
} // namespace

PPT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties* properties) {
    return enumerateLayer(count, properties);
}
PPT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count, VkLayerProperties* properties) {
    return enumerateLayer(count, properties);
}
PPT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* name,
        uint32_t* count, VkExtensionProperties*) {
    if (!name || std::strcmp(name, kLayerName)) return VK_ERROR_LAYER_NOT_PRESENT;
    *count = 0;
    return VK_SUCCESS;
}

PPT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
        const char* name, uint32_t* count, VkExtensionProperties* properties) {
    if (name) {
        if (std::strcmp(name, kLayerName)) return VK_ERROR_LAYER_NOT_PRESENT;
        *count = 0;
        return VK_SUCCESS;
    }
    auto* record = findInstance(physicalDevice);
    if (!record) return VK_ERROR_INITIALIZATION_FAILED;
    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        record->getProcAddr(record->handle, "vkEnumerateDeviceExtensionProperties"));
    return enumerate(physicalDevice, name, count, properties);
}

PPT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!device || !name) return nullptr;
    auto* record = findDevice(dispatchKey(device));
    if (!record) return nullptr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    if (!std::strcmp(name, "vkAllocateCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(allocateCommandBuffers);
    if (!std::strcmp(name, "vkFreeCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(freeCommandBuffers);
    if (!std::strcmp(name, "vkDestroyCommandPool")) return reinterpret_cast<PFN_vkVoidFunction>(destroyCommandPool);
    if (!std::strcmp(name, "vkBeginCommandBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(beginCommandBuffer);
    if (!std::strcmp(name, "vkEndCommandBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(endCommandBuffer);
    if (!std::strcmp(name, "vkResetCommandBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(resetCommandBuffer);
    if (!std::strcmp(name, "vkResetCommandPool")) return reinterpret_cast<PFN_vkVoidFunction>(resetCommandPool);
    return drawHook(name, record->getProcAddr(device, name));
}

PPT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
#define ENTRY(vkName, function) if (!std::strcmp(name, vkName)) return reinterpret_cast<PFN_vkVoidFunction>(function)
    ENTRY("vkCreateInstance", createInstance);
    ENTRY("vkGetInstanceProcAddr", vkGetInstanceProcAddr);
    ENTRY("vkEnumerateInstanceLayerProperties", vkEnumerateInstanceLayerProperties);
    ENTRY("vkEnumerateInstanceExtensionProperties", vkEnumerateInstanceExtensionProperties);
    if (!instance) return nullptr;
    ENTRY("vkCreateDevice", createDevice);
    ENTRY("vkDestroyInstance", destroyInstance);
    ENTRY("vkDestroyDevice", destroyDevice);
    ENTRY("vkAllocateCommandBuffers", allocateCommandBuffers);
    ENTRY("vkFreeCommandBuffers", freeCommandBuffers);
    ENTRY("vkDestroyCommandPool", destroyCommandPool);
    ENTRY("vkBeginCommandBuffer", beginCommandBuffer);
    ENTRY("vkEndCommandBuffer", endCommandBuffer);
    ENTRY("vkResetCommandBuffer", resetCommandBuffer);
    ENTRY("vkResetCommandPool", resetCommandPool);
    ENTRY("vkGetDeviceProcAddr", vkGetDeviceProcAddr);
    ENTRY("vkEnumerateDeviceLayerProperties", vkEnumerateDeviceLayerProperties);
    ENTRY("vkEnumerateDeviceExtensionProperties", vkEnumerateDeviceExtensionProperties);
#undef ENTRY
    auto* record = findInstance(instance);
    return record ? drawHook(name, record->getProcAddr(instance, name)) : nullptr;
}
