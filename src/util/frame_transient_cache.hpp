#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cstdint>
#include <limits>
#include <vector>

namespace cairns {

struct DynamicBuffersAssoc {
    uint32_t offset = 0;
};

struct BindGroupAssoc {
    uint32_t material_offset = 0;
    uint32_t material = std::numeric_limits<uint32_t>::max();
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
