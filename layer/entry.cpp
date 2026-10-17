// Android loads the exported functions directly from the shared library. The loader's
// create-info links supply the next layer's entry points; commands this layer does not
// hook go straight to them.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

#define PPT_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
constexpr const char* kLayerName = "VK_LAYER_PPT_perdraw";

// Filled once when the instance or device is created,
// not looked up by name on every call.
#define PPT_INSTANCE_FUNCS(X) X(DestroyInstance) X(EnumerateDeviceExtensionProperties)
#define PPT_DEVICE_FUNCS(X) \
    X(DestroyDevice) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(DestroyCommandPool)

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
    VkCommandPool pool;
    explicit CommandBufferWrapper(VkCommandPool vkPool) : pool(vkPool) {}
};

struct DeviceWrapper {
    // Forward function lookups to the next layer.
    PFN_vkGetDeviceProcAddr getProcAddr;
    DeviceFns next;
    // Key by the full handle: command buffers share the device's dispatch table.
    std::unordered_map<VkCommandBuffer, std::unique_ptr<CommandBufferWrapper>> commandBuffers;
    DeviceWrapper(PFN_vkGetDeviceProcAddr vkGetProcAddr, const DeviceFns& vkNext)
        : getProcAddr(vkGetProcAddr), next(vkNext) {}
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
    InstanceFns next;
#define X(name) next.name = reinterpret_cast<PFN_vk##name>(getProcAddr(*instance, "vk" #name));
    PPT_INSTANCE_FUNCS(X)
#undef X
    try {
        std::unique_ptr<InstanceWrapper> record = std::make_unique<InstanceWrapper>(*instance, getProcAddr, next);
        std::lock_guard<std::mutex> guard(stateMutex);
        // This key should not already be in the map.
        // If duplicate keys become possible, update this code to handle them.
        instances.emplace(dispatchKey(*instance), std::move(record));
    } catch (const std::bad_alloc&) {
        // The lock has unwound; roll back the downstream object as well as our record.
        next.DestroyInstance(*instance, allocator);
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
    record->next.DestroyInstance(instance, allocator);
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
    DeviceFns next;
#define X(name) next.name = reinterpret_cast<PFN_vk##name>(getDeviceProcAddr(*device, "vk" #name));
    PPT_DEVICE_FUNCS(X)
#undef X
    try {
        std::unique_ptr<DeviceWrapper> record = std::make_unique<DeviceWrapper>(getDeviceProcAddr, next);
        std::lock_guard<std::mutex> guard(stateMutex);
        // This key should not already be in the map.
        // If duplicate keys become possible, update this code to handle them.
        devices.emplace(dispatchKey(*device), std::move(record));
    } catch (const std::bad_alloc&) {
        next.DestroyDevice(*device, allocator);
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
    record->next.DestroyDevice(device, allocator);
}

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
    if (!record) return VK_ERROR_LAYER_NOT_PRESENT;
    return record->next.EnumerateDeviceExtensionProperties(physicalDevice, name, count, properties);
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
    return record->getProcAddr(device, name);
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
    ENTRY("vkGetDeviceProcAddr", vkGetDeviceProcAddr);
    ENTRY("vkEnumerateDeviceLayerProperties", vkEnumerateDeviceLayerProperties);
    ENTRY("vkEnumerateDeviceExtensionProperties", vkEnumerateDeviceExtensionProperties);
#undef ENTRY
    auto* record = findInstance(instance);
    return record ? record->getProcAddr(instance, name) : nullptr;
}
