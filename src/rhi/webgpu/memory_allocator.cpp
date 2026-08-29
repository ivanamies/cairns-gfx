// rhi/webgpu/memory_allocator.cpp -- WebGPU backend.
// Persistent buffers are individual WGPUBuffers (wgpu owns the memory; no manual
// heaps). The per-frame bump ring is a CPU staging block (WebGPU has no
// persistent host-visible mapping); EndSubmit uploads it via queueWriteBuffer.
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include <cstdlib>
#include <cstring>

#include "rhi/webgpu/memory_allocator.hpp"

namespace cairns::rhi::webgpu {

namespace {
WGPUBufferUsage ToWgpuBufferUsage(BufferUsage u) {
    uint64_t out = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    if (u & kUsageVertex) out |= WGPUBufferUsage_Vertex;
    if (u & kUsageIndex) out |= WGPUBufferUsage_Index;
    if (u & kUsageUniform) out |= WGPUBufferUsage_Uniform;
    if (u & kUsageStorage) out |= WGPUBufferUsage_Storage;
    if (u & kUsageIndirect) out |= WGPUBufferUsage_Indirect;
    return static_cast<WGPUBufferUsage>(out);
}
constexpr uint32_t kBumpBytesPerMem = 16u * 1024u * 1024u;  // per Memory type
inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }
}  // namespace

MemoryAllocator::~MemoryAllocator() { Deinit(); }

bool MemoryAllocator::Init(WGPUDevice device) {
    device_ = device;
    // blocks_[0] = the bump heap: one GPU buffer + a CPU mirror per Memory type
    // laid out side by side, kFramesInFlight slots each.
    HeapBlock bump;
    const uint32_t per_slot = kBumpBytesPerMem;
    const uint32_t total = static_cast<uint32_t>(kMemoryCount) * kFramesInFlight * per_slot;
    WGPUBufferDescriptor bd = {};
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform |
               WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex | WGPUBufferUsage_Index;
    bd.size = total;
    bump.master_buffer = wgpuDeviceCreateBuffer(device_, &bd);
    bump.mapped_ptr = std::calloc(total, 1);
    bump.size_bytes = total;
    blocks_.push_back(bump);
    for (size_t m = 0; m < kMemoryCount; ++m) {
        bump_.region_base[m] = static_cast<uint32_t>(m) * kFramesInFlight * per_slot;
        bump_.slot_size[m] = per_slot;
    }
    bump_.current_slot = 0;
    initialized_ = true;
    return true;
}

void MemoryAllocator::Deinit() {
    for (auto& b : blocks_) {
        if (b.master_buffer) { wgpuBufferRelease(b.master_buffer); }
        if (b.mapped_ptr) { std::free(b.mapped_ptr); }
    }
    blocks_.clear();
    initialized_ = false;
}

AllocResult MemoryAllocator::AllocBuffer(uint32_t bytes, BufferUsage usage, Memory mem,
                                         uint32_t align) {
    (void)mem; (void)align;
    WGPUBufferDescriptor bd = {};
    bd.usage = ToWgpuBufferUsage(usage);
    bd.size = bytes ? bytes : 4;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(device_, &bd);
    if (!buf) { return AllocResult{}; }
    HeapBlock blk;
    blk.master_buffer = buf;
    blk.size_bytes = bytes;
    blk.mem_type = mem;
    AllocResult r;
    r.heap_index = static_cast<uint32_t>(blocks_.size());
    r.offset = 0;
    r.ok = true;
    blocks_.push_back(blk);
    return r;
}

AllocResult MemoryAllocator::AllocImage(uint32_t, uint32_t, Memory) {
    // WebGPU textures are created directly (resources.cpp); no heap suballoc.
    AllocResult r; r.ok = true; r.heap_index = kInvalidBlock; return r;
}

void MemoryAllocator::FreeBuffer(uint32_t heap_index, OffsetAllocator::Allocation, uint32_t) {
    if (heap_index < blocks_.size() && blocks_[heap_index].master_buffer) {
        wgpuBufferRelease(blocks_[heap_index].master_buffer);
        blocks_[heap_index].master_buffer = nullptr;
    }
}
void MemoryAllocator::FreeImage(uint32_t, OffsetAllocator::Allocation, WGPUTexture texture, uint32_t) {
    if (texture) { wgpuTextureRelease(texture); }
}

void* MemoryAllocator::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                                    uint32_t* out_offset) {
    if (blocks_.empty() || !blocks_[0].mapped_ptr) { if (out_offset) *out_offset = 0; return nullptr; }
    const size_t mi = mem_index(mem);
    const uint32_t slot = bump_.current_slot % kFramesInFlight;
    uint32_t cur = align_up(bump_.cursors[mi][slot], align ? align : 16u);
    if (cur + bytes > bump_.slot_size[mi]) { if (out_offset) *out_offset = 0; return nullptr; }
    const uint32_t abs = bump_.region_base[mi] + slot * bump_.slot_size[mi] + cur;
    bump_.cursors[mi][slot] = cur + bytes;
    if (out_offset) { *out_offset = abs; }
    return static_cast<uint8_t*>(blocks_[0].mapped_ptr) + abs;
}
uint32_t MemoryAllocator::BumpMasterHeapIndex(Memory) const { return kBumpHeapIndex; }
uint32_t MemoryAllocator::BumpRingBytes(Memory mem) const { return bump_.slot_size[mem_index(mem)]; }
uint32_t MemoryAllocator::BumpSaveCursor(Memory mem) const {
    return bump_.cursors[mem_index(mem)][bump_.current_slot % kFramesInFlight];
}
void MemoryAllocator::BumpRestoreCursor(Memory mem, uint32_t cursor) {
    bump_.cursors[mem_index(mem)][bump_.current_slot % kFramesInFlight] = cursor;
}

WGPUBuffer MemoryAllocator::HeapMasterBuffer(uint32_t heap_index) const {
    return heap_index < blocks_.size() ? blocks_[heap_index].master_buffer : nullptr;
}
void* MemoryAllocator::HeapMappedPtr(uint32_t heap_index) const {
    return heap_index < blocks_.size() ? blocks_[heap_index].mapped_ptr : nullptr;
}

void MemoryAllocator::FlushBumpRing(WGPUQueue queue) {
    if (blocks_.empty() || !blocks_[0].master_buffer || !blocks_[0].mapped_ptr) {
        return;
    }
    const uint32_t slot = bump_.current_slot % kFramesInFlight;
    for (size_t m = 0; m < kMemoryCount; ++m) {
        const uint32_t written = bump_.cursors[m][slot];
        if (!written) { continue; }
        const uint32_t abs = bump_.region_base[m] + slot * bump_.slot_size[m];
        // queueWriteBuffer requires a 4-multiple size; the mirror is calloc'd so
        // the padding bytes are zero and within the slot.
        const uint32_t bytes = align_up(written, 4u);
        wgpuQueueWriteBuffer(queue, blocks_[0].master_buffer, abs,
                             static_cast<uint8_t*>(blocks_[0].mapped_ptr) + abs,
                             bytes);
    }
}

void MemoryAllocator::BeginFrame(uint32_t frame_index) {
    bump_.current_slot = frame_index % kFramesInFlight;
    const uint32_t slot = bump_.current_slot;
    for (size_t m = 0; m < kMemoryCount; ++m) { bump_.cursors[m][slot] = 0; }
}
void MemoryAllocator::RetireFrame(uint32_t) {}

}  // namespace cairns::rhi::webgpu
#endif  // CAIRNS_WEBGPU
