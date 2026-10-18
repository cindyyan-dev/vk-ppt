#pragma once

#include <vulkan/vulkan.h>
#include <optional>

struct InputAssembly {
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    bool topologyIsDynamic = false;
    bool mayRestart = true;
};

inline InputAssembly pipelineAssembly(const VkGraphicsPipelineCreateInfo& info) {
    for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT ||
            node->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) return {};
    }
    for (uint32_t i = 0; i < info.stageCount; ++i) {
        if (info.pStages[i].stage == VK_SHADER_STAGE_MESH_BIT_EXT) return {};
    }
    InputAssembly assembly;
    bool dynamicRestart = false;
    if (auto* dynamic = info.pDynamicState) {
        for (uint32_t i = 0; i < dynamic->dynamicStateCount; ++i) {
            switch (dynamic->pDynamicStates[i]) {
                case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: assembly.topologyIsDynamic = true; break;
                case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: dynamicRestart = true; break;
                default: break;
            }
        }
    }
    if (auto* input = info.pInputAssemblyState) {
        if (!assembly.topologyIsDynamic) assembly.topology = input->topology;
        assembly.mayRestart = dynamicRestart || input->primitiveRestartEnable;
    }
    return assembly;
}

inline std::optional<uint64_t> primitiveCount(VkPrimitiveTopology topology, uint32_t count,
        uint32_t instances, bool mayRestart) {
    if (!count || !instances) return 0;
    if (mayRestart) return std::nullopt;
    uint64_t perInstance;
    switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: perInstance = count; break;
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: perInstance = count / 2; break;
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: perInstance = count - 1; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: perInstance = count / 3; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: perInstance = count > 2 ? count - 2 : 0; break;
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: perInstance = count / 4; break;
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: perInstance = count > 3 ? count - 3 : 0; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY: perInstance = count / 6; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: perInstance = count > 5 ? count / 2 - 2 : 0; break;
        default: return std::nullopt;
    }
    return perInstance * instances;
}
