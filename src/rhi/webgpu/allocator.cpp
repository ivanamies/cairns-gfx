// rhi/webgpu/allocator.cpp -- WebGPU backend (W2 stubs; real bump ring in W3).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/allocator.hpp"
#include "rhi/device.hpp"

namespace cairns::rhi {

Allocator::~Allocator() {}

bool Allocator::Init(Device& device) {
    inited_ = plat.memory_.Init(device.plat.device);
    return inited_;
}
void Allocator::Deinit() { plat.memory_.Deinit(); inited_ = false; }

void* Allocator::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                              uint32_t* out_offset) {
    return plat.memory_.BumpAllocate(bytes, align, mem, out_offset);
}
Handle<Buffer> Allocator::BumpMasterBuffer(Memory mem) const {
    (void)mem; return Handle<Buffer>::Null;
}
uint32_t Allocator::UboAlign() const { return 256; }
uint32_t Allocator::StorageAlign() const { return 256; }
void Allocator::AdvanceFrame(uint32_t frame_index) {
    plat.memory_.RetireFrame(frame_index % kFramesInFlight);
    plat.memory_.BeginFrame(frame_index);
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
