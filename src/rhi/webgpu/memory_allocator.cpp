// rhi/webgpu/memory_allocator.cpp -- WebGPU backend (W2 stubs; real bump
// ring + per-resource heaps in W3).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/webgpu/memory_allocator.hpp"

namespace cairns::rhi::webgpu {

MemoryAllocator::~MemoryAllocator() { Deinit(); }

bool MemoryAllocator::Init(WGPUDevice device) { device_ = device; initialized_ = true; return true; }
void MemoryAllocator::Deinit() { blocks_.clear(); initialized_ = false; }

AllocResult MemoryAllocator::AllocBuffer(uint32_t, BufferUsage, Memory, uint32_t) { return AllocResult{}; }
AllocResult MemoryAllocator::AllocImage(uint32_t, uint32_t, Memory) { return AllocResult{}; }
void MemoryAllocator::FreeBuffer(uint32_t, OffsetAllocator::Allocation, uint32_t) {}
void MemoryAllocator::FreeImage(uint32_t, OffsetAllocator::Allocation, WGPUTexture, uint32_t) {}

void* MemoryAllocator::BumpAllocate(uint32_t, uint32_t, Memory, uint32_t* out_offset) {
    if (out_offset) { *out_offset = 0; }
    return nullptr;
}
uint32_t MemoryAllocator::BumpMasterHeapIndex(Memory) const { return kBumpHeapIndex; }
uint32_t MemoryAllocator::BumpRingBytes(Memory) const { return 0; }
uint32_t MemoryAllocator::BumpSaveCursor(Memory) const { return 0; }
void MemoryAllocator::BumpRestoreCursor(Memory, uint32_t) {}
WGPUBuffer MemoryAllocator::HeapMasterBuffer(uint32_t) const { return nullptr; }
void* MemoryAllocator::HeapMappedPtr(uint32_t) const { return nullptr; }
void MemoryAllocator::BeginFrame(uint32_t) {}
void MemoryAllocator::RetireFrame(uint32_t) {}

}  // namespace cairns::rhi::webgpu
#endif  // CAIRNS_WEBGPU
