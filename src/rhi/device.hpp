#pragma once

#include "gfx_api.hpp"

namespace cairns::rhi {

class Device {
public:
    Device() = default;
    explicit Device(MTL::Device* device) : device_(device) {}
    ~Device() {
        if (device_) device_->release();
    }
    
    MTL::Device* get() const { return device_; }
    
    int64_t GetGpuAlignUboOffset() const {
        return gpuAlignUboOffset_;
    }
    
private:
    MTL::Device* device_ = nullptr;
    // UBO offsets are not provided programatically on Metal. We have to consult the feature table's `Minimum constant buffer offset alignment`
    // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
    // this is minUniformBufferOffsetAlignment in Vulkan... 80% certainity.
    int64_t gpuAlignUboOffset_ = 32; // valid for all possible hardware
};

} // namespace cairns::rhi
