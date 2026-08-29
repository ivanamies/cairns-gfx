// src/util/texture_loader.hpp -- standalone (non-GLB) texture loading.
//  - Product: effect textures the NPR passes sample (paper grain, noise,
//    authored TAM mip chains) -- loaded by name, owned by the engine's
//    effect-texture table, not by any prefab.
//  - CPU mips: a deterministic integer box filter builds ALL levels into ONE
//    concatenated blob -- exactly the multi-level initial_data layout the
//    WebGPU backend already consumes; metal/vk grew the same size-walk.
//    (GLB textures keep the GPU-mipgen path: flipping them would rebake every
//    textured golden.)
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <stb_image.h>

#include "rhi/resources.hpp"
#include "util/texture_mips.hpp"

namespace cairns {

struct PngImage {
    std::vector<uint8_t> pixels;  // tightly packed RGBA8
    uint32_t width = 0;
    uint32_t height = 0;
    bool ok = false;
};

inline PngImage LoadPngRgba8(const char* path) {
    PngImage out;
    size_t n = 0;
    void* data = SDL_LoadFile(path, &n);
    if (data == nullptr) {
        return out;
    }
    int w = 0;
    int h = 0;
    int c = 0;
    unsigned char* raw = stbi_load_from_memory(
        static_cast<const unsigned char*>(data), static_cast<int>(n), &w, &h,
        &c, 4);
    SDL_free(data);
    if (raw == nullptr || w <= 0 || h <= 0) {
        return out;
    }
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.pixels.assign(raw, raw + (static_cast<size_t>(w) * h * 4));
    stbi_image_free(raw);
    out.ok = true;
    return out;
}

inline rhi::Handle<rhi::Texture> LoadTextureWithMips(rhi::Resources& res,
                                                     rhi::Allocator& alloc,
                                                     const char* path,
                                                     rhi::TextureUsage usage) {
    const PngImage img = LoadPngRgba8(path);
    if (!img.ok) {
        return rhi::Handle<rhi::Texture>::Null;
    }
    const std::vector<uint8_t> blob =
        BuildMipChainRgba8(img.pixels.data(), img.width, img.height);
    rhi::TextureDesc d;
    d.dimensions = {static_cast<int32_t>(img.width),
                    static_cast<int32_t>(img.height), 1};
    d.format = rhi::Format::kRgba8Unorm;
    d.mip_levels = FullMipLevels(img.width, img.height);
    d.array_layers = 1;
    d.usage = usage;
    d.memory = rhi::Memory::kDefault;
    d.initial_data = blob;
    d.debug_name = path;
    return res.CreateTexture(alloc, d);
}

// Explicit authored levels (the TAM case: each mip REDRAWN at its resolution,
// not filtered). Every file must be RGBA8 and exactly half the previous.
inline rhi::Handle<rhi::Texture> LoadTextureFromLevelFiles(
    rhi::Resources& res, rhi::Allocator& alloc,
    const char* const* level_paths, uint32_t level_count,
    rhi::TextureUsage usage) {
    if (level_count == 0) {
        return rhi::Handle<rhi::Texture>::Null;
    }
    std::vector<uint8_t> blob;
    uint32_t w0 = 0;
    uint32_t h0 = 0;
    uint32_t pw = 0;
    uint32_t ph = 0;
    for (uint32_t l = 0; l < level_count; ++l) {
        const PngImage img = LoadPngRgba8(level_paths[l]);
        if (!img.ok) {
            return rhi::Handle<rhi::Texture>::Null;
        }
        if (l == 0) {
            w0 = img.width;
            h0 = img.height;
        } else {
            const uint32_t ew = pw > 1 ? pw >> 1 : 1u;
            const uint32_t eh = ph > 1 ? ph >> 1 : 1u;
            if (img.width != ew || img.height != eh) {
                fprintf(stderr,
                        "[texture_loader] %s: level %u is %ux%u, expected "
                        "%ux%u\n",
                        level_paths[l], l, img.width, img.height, ew, eh);
                return rhi::Handle<rhi::Texture>::Null;
            }
        }
        blob.insert(blob.end(), img.pixels.begin(), img.pixels.end());
        pw = img.width;
        ph = img.height;
    }
    rhi::TextureDesc d;
    d.dimensions = {static_cast<int32_t>(w0), static_cast<int32_t>(h0), 1};
    d.format = rhi::Format::kRgba8Unorm;
    d.mip_levels = level_count;
    d.array_layers = 1;
    d.usage = usage;
    d.memory = rhi::Memory::kDefault;
    d.initial_data = blob;
    d.debug_name = level_paths[0];
    return res.CreateTexture(alloc, d);
}

}  // namespace cairns
