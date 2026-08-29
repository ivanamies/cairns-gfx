#pragma once

#include "util/gltf_loader.hpp"
#include "rhi/resource_manager.hpp"

namespace cairns::rhi {

inline bool LoadMeshGpu(Mesh& mesh, ResourceManager& rm) {
    auto process = [&](Handle<Buffer>& h, const void* srcData,
                       size_t srcSize) -> bool {
        if (srcSize == 0) {
            return true;
        }
        BufferDesc d;
        d.byte_size = static_cast<uint32_t>(srcSize);
        d.usage = kUsageVertex | kUsageIndex;
        d.memory = Memory::kDefault;
        d.initial_data = Span<const uint8_t>(
            static_cast<const uint8_t*>(srcData), srcSize);
        h = rm.CreateBuffer(d);
        return !h.IsNull();
    };

    if (!process(mesh.posHandle, mesh.cpuPositions.data(),
                 mesh.cpuPositions.size() * sizeof(glm::vec4))) {
        return false;
    }
    if (!process(mesh.attrHandle, mesh.cpuAttrs.data(),
                 mesh.cpuAttrs.size() * sizeof(VertexAttribute))) {
        return false;
    }
    if (!process(mesh.indexHandle, mesh.cpuIndices.data(),
                 mesh.cpuIndices.size() * sizeof(uint32_t))) {
        return false;
    }
    return true;
}

inline bool LoadSceneGpu(Scene& scene, ResourceManager& rm) {
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        if (!LoadMeshGpu(scene.meshes[i], rm)) {
            return false;
        }
    }
    return true;
}

}  // namespace cairns::rhi
