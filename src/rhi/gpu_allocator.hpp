#pragma once

#include "rhi/gfx_api.hpp"
#include "rhi/misc.hpp"
#include "rhi/device.hpp"
#include "util/offset_allocator.hpp"
#include "util/misc.hpp"

namespace cairns::rhi {
    
// todo @iamies rename to GpuAllocatorLinear
struct GpuAllocator {
    
    enum class Category {
        kUnknown = 0,
        // distributes memory out of cpu/gpu unified virtual addresses, cpu does not need to flush.
        // use for UBOs, dynamic vertex buffers, NOT SSBOs you read from.
        // vulkan: DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT.
        // metal: MTLStorageModeShared.
        kCat1,
        // distributes memory out of cpu/gpu shared addresses, but cpu MUST CALL FLUSH.
        // unfortunately calling flush will also crash the application on Apple Silicon so I only pretend to call it on Apple Silicon :shrug:.
        // use for staging, static meshes, textures, large SSBOs.
        // vulkan: DEVICE_LOCAL_BIT | HOST_VISIBLE_BIT
        // metal: MTLStorageModeShared (we only support apple silicon)
        kCat2,
        // distributes memory out of gpu private virtual addresses.
        // use for render targets, depth buffers and compute UAVs (fancy r/w lookup tables).
        // vulkan: DEVICE_LOCAL
        // metal: MTLStorageModePrivate
        kCat3,
        // for screen shots
        // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
        kCat4,
        // we don't use push constants because I am too lazy to support them and they probably don't work properly anyways on mobile phones.
    };
    
    GpuAllocator() = default;
    GpuAllocator(std::string_view label, Device& device, Category category, int64_t size) : label_(label), device_(device), category_(category), size_(size), alloc_(static_cast<uint32_t>(size)) {
        MTL::ResourceOptions mode = MTL::ResourceCPUCacheModeDefaultCache;
        switch (category) {
            case (Category::kCat1): {
                mode = MTL::ResourceStorageModeShared;
                break;
            }
            case (Category::kCat2): {
                mode = MTL::ResourceStorageModeShared;
                break;
            }
            case (Category::kCat3): {
                mode = MTL::ResourceStorageModePrivate;
                break;
            }
            default:
                assert(false); // bad GpuAllocator category
                break;
        }
        buffer_ = device.get()->newBuffer(size, mode);
        setLabel(buffer_, label_);
    }
    
    bool Valid() {
        return buffer_ != nullptr;
    }
    
    void Deinit() {
        if ( buffer_ ) {
            buffer_->release();
            buffer_ = nullptr;
        }
    }
    
    OffsetAllocator::Allocation Alloc(int64_t size, int64_t align) {
        const uint64_t align_mask = align - 1;
        const int64_t aligned_size = (size + align_mask) & ~align_mask;
        OffsetAllocator::Allocation allocation = alloc_.allocate(static_cast<uint32_t>(aligned_size));
        if ( allocation == OffsetAllocator::BadAllocation ) {
            printf("throw std::bad_alloc >:P\n");
            std::terminate();
        }
        make_aligned(allocation.offset, align);
        return allocation;
    }
    
    void Dealloc(OffsetAllocator::Allocation d) {
        alloc_.free(d);
    }
    
    // used for CPU/GPU memory mapped IO.
    // works alot like mmap. probably IS just a flavor of mmap.
    uint8_t* GetCpuAddress(int64_t offset) {
        return reinterpret_cast<uint8_t*>(buffer_->contents()) + offset;
    }
    
    // prevents the GPU from working on memory being transferred from CPU to GPU
    // ONLY USED FOR STORAGEMODEMANAGED (corresponds to kCat2). will crash for StorageModeShared. because Apple.
    void Barrier(OffsetAllocator::Allocation mem, uint32_t size) {
        if ( category_ == Category::kCat2 && !cairns::is_metal()) {
            buffer_->didModifyRange(NS::Range(mem.offset, size));
        }
    }
    
    MTL::Buffer* GetBuffer() const {
        return buffer_;
    }
    
    OffsetAllocator::Allocator& DebugGetImpl() {
        return alloc_;
    }
    
private:
    std::string_view label_;
    Device device_;
    Category category_;
    [[maybe_unused]] int64_t size_ = 0;
    MTL::Buffer* buffer_ = nullptr;
    OffsetAllocator::Allocator alloc_;
};

struct GpuAllocatorHeap {
    GpuAllocatorHeap() = default;
    GpuAllocatorHeap(std::string_view label, Device& device, uint32_t storage, uint32_t size) : label_(label), device_(device), size_(size), alloc_(static_cast<uint32_t>(size)) {
        MTL::HeapDescriptor* desc = MTL::HeapDescriptor::alloc()->init();
        desc->setSize(size);
        desc->setStorageMode(static_cast<MTL::StorageMode>(storage));
        desc->setType(MTL::HeapTypePlacement);
        heap_ = device.get()->newHeap(desc);
        if ( heap_ ) {
            setLabel(heap_, label_);
        }
        desc->release();
    }
    
    OffsetAllocator::Allocation Alloc(int64_t size, int64_t align) {
        const uint64_t align_mask = align - 1;
        const int64_t aligned_size = (size + align_mask) & ~align_mask;
        OffsetAllocator::Allocation allocation = alloc_.allocate(static_cast<uint32_t>(aligned_size));
        if ( allocation == OffsetAllocator::BadAllocation ) {
            printf("throw std::bad_alloc >:P\n");
            std::terminate();
        }
        return allocation;
    }
    
    void Dealloc(OffsetAllocator::Allocation d) {
        assert(d != OffsetAllocator::BadAllocation);
        alloc_.free(d);
    }
    
    bool AllocTexture(ResourceObject<Texture>& obj, ResourceDescriptor<Texture>& desc, std::string_view label = "") {
        const MTL::TextureType texture_type = static_cast<MTL::TextureType>(desc.type);
        const MTL::PixelFormat pixel_format = static_cast<MTL::PixelFormat>(desc.format);
        const MTL::StorageMode storage_mode = static_cast<MTL::StorageMode>(desc.storage);
        const uint32_t width = static_cast<uint32_t>(desc.width);
        const uint32_t height = static_cast<uint32_t>(desc.height);
        const uint32_t sample_count = static_cast<uint32_t>(desc.sample_count);
        const uint32_t levels = static_cast<uint32_t>(desc.levels);
        const MTL::TextureUsage texture_usage = static_cast<MTL::TextureUsage>(desc.usage);
        
        MTL::TextureDescriptor* texture_desc = MTL::TextureDescriptor::alloc()->init();
        texture_desc->setTextureType(texture_type);
        texture_desc->setPixelFormat(pixel_format);
        texture_desc->setWidth(width);
        texture_desc->setHeight(height);
        texture_desc->setSampleCount(sample_count);
        texture_desc->setUsage(texture_usage);
        texture_desc->setMipmapLevelCount(levels);
        texture_desc->setStorageMode(storage_mode);
        
        MTL::Texture* tex = nullptr;
        
        const MTL::SizeAndAlign size_and_align = device_.get()->heapTextureSizeAndAlign(texture_desc);
        // todo @iamies save this allocation in case you need to deallocate it...
        // todo @iamies this is not an aligned alloc.
        // first: size += align - 1;
        // then: allocate this new larger size
        // lastly: size = (size + align - 1) & ~(align - 1);
        OffsetAllocator::Allocation mem = Alloc(size_and_align.size, size_and_align.align);
        if ( mem != OffsetAllocator::BadAllocation ) {
            tex = heap_->newTexture(texture_desc, mem.offset);
        }
        
        obj.texture = tex;
        if (tex) {
            setLabel(tex, label);
        }
        texture_desc->release();
        return tex != nullptr;
    }
    
    bool Valid() {
        return heap_ != nullptr;
    }
    
    MTL::Heap* GetHeap() {
        return heap_;
    }
    
private:
    std::string_view label_;
    Device device_;
    int64_t size_ = 0;
    MTL::Heap* heap_ = nullptr;
    OffsetAllocator::Allocator alloc_;
};

} // cairns::rhi
