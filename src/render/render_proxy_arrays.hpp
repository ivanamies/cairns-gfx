#pragma once

#include "render/render_proxy.hpp"

#include <cstdint>
#include <vector>

namespace cairns {

template <typename T>
struct ProxyArray {
    std::vector<T> data;
    std::vector<uint32_t> free_list;

    uint32_t Add(const T& value) {
        if (!free_list.empty()) {
            const uint32_t idx = free_list.back();
            free_list.pop_back();
            data[idx] = value;
            return idx;
        }
        data.push_back(value);
        return static_cast<uint32_t>(data.size() - 1);
    }

    void Update(uint32_t idx, const T& value) {
        data[idx] = value;
    }

    void Remove(uint32_t idx) {
        free_list.push_back(idx);
    }

    void Clear() {
        data.clear();
        free_list.clear();
    }

    size_t size() const {
        return data.size();
    }
};

struct RenderProxyArrays {
    ProxyArray<MeshProxy> meshes;
    std::vector<PrimitiveProxy> primitives;
    ProxyArray<LineProxy> lines;
    ProxyArray<PointProxy> points;
    ProxyArray<SkinnedAttachment> skins;
    ProxyArray<LightProxy> lights;
    ProxyArray<CameraProxy> cameras;
    ProxyArray<LayerProxy> layers;

    void Clear() {
        meshes.Clear();
        primitives.clear();
        lines.Clear();
        points.Clear();
        skins.Clear();
        lights.Clear();
        cameras.Clear();
        layers.Clear();
    }
};

}  // namespace cairns
