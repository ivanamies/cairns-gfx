#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cstdint>
#include <vector>

#include "rhi/resource.hpp"
#include "rhi/tag.hpp"

namespace cairns {

struct DynamicBuffersAssoc {
    rhi::Handle<rhi::Buffer> buf = rhi::Handle<rhi::Buffer>::Null;
};

template <typename T>
class FrameTransientCache {
public:
    uint32_t Acquire() {
        if (cursor_ < entries_.size()) {
            const uint32_t i = cursor_++;
            entries_[i] = T{};
            return i;
        }
        entries_.emplace_back();
        const uint32_t i = cursor_;
        ++cursor_;
        return i;
    }

    T& At(uint32_t i) { return entries_[i]; }

    void Reset() { cursor_ = 0; }

private:
    std::vector<T> entries_;
    uint32_t cursor_ = 0;
};

}  // namespace cairns

#endif  // CAIRNS_METAL
