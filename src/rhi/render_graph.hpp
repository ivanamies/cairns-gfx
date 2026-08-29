#pragma once

#include "rhi/command_recorder.hpp"
#include "rhi/resource_manager.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cairns::rhi {

class Resources;
class Allocator;
struct SwapChain;
class RenderGraph;

struct GraphTexture {
    uint16_t id = 0xFFFF;
    bool IsNull() const { return id == 0xFFFF; }
};

struct GraphBuffer {
    uint16_t id = 0xFFFF;
    bool IsNull() const { return id == 0xFFFF; }
};

enum class PassType : uint8_t {
    kGraphics,
    kCompute,
};

struct GraphTextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::kRgba8Unorm;
    uint32_t samples = 1;
    TextureUsage usage = kTexUsageSampled;
};

struct GraphBufferDesc {
    uint32_t byte_size = 0;
    BufferUsage usage = kUsageNone;
};

class PassResources {
public:
    PassResources() = default;
    PassResources(const std::vector<Handle<Texture>>* textures,
                  const std::vector<Handle<Buffer>>* buffers)
        : textures_(textures), buffers_(buffers) {}

    Handle<Texture> Resolve(GraphTexture t) const;
    Handle<Buffer> Resolve(GraphBuffer b) const;

private:
    const std::vector<Handle<Texture>>* textures_ = nullptr;
    const std::vector<Handle<Buffer>>* buffers_ = nullptr;
};

using SetupFn = std::function<void(class PassBuilder&)>;
using ExecuteFn = std::function<void(CommandRecorder&, const PassResources&)>;

class PassBuilder {
public:
    PassBuilder(RenderGraph* graph, uint32_t pass) : graph_(graph), pass_(pass) {}

    GraphTexture CreateColorTarget(const GraphTextureDesc& desc);
    GraphTexture CreateDepthTarget(const GraphTextureDesc& desc);
    GraphTexture ImportTexture(Handle<Texture> handle, const GraphTextureDesc& meta);
    GraphBuffer CreateBuffer(const GraphBufferDesc& desc);
    GraphBuffer ImportBuffer(Handle<Buffer> handle, const GraphBufferDesc& meta);

    void Read(GraphTexture t);
    void Write(GraphTexture t);
    void ReadWrite(GraphTexture t);

    void AddColorOutput(const char* name, GraphTexture t, LoadOp load,
                        const float clear[4]);
    void AddDepthOutput(const char* name, GraphTexture t, LoadOp load,
                        float clear_depth);
    void AddAttachmentInput(GraphTexture t);

private:
    RenderGraph* graph_ = nullptr;
    uint32_t pass_ = 0;
};

class RenderGraph {
public:
    RenderGraph(Resources& resources, Allocator& alloc);
    ~RenderGraph();

    void Reset();
    void AddPass(const char* name, PassType type, SetupFn setup, ExecuteFn execute);
    void SetOutput(GraphTexture t);
    bool Bake();
    bool Execute(FrameContext& fc);

    Handle<Texture> ResolveTexture(GraphTexture t) const;
    Handle<Buffer> ResolveBuffer(GraphBuffer b) const;

private:
    friend class PassBuilder;

    enum class ResKind : uint8_t { kCreated, kImported };

    struct TexRecord {
        GraphTextureDesc desc;
        ResKind kind = ResKind::kCreated;
        Handle<Texture> imported;
    };

    struct BufRecord {
        GraphBufferDesc desc;
        ResKind kind = ResKind::kCreated;
        Handle<Buffer> imported;
    };

    struct ColorOutput {
        uint16_t tex = 0xFFFF;
        LoadOp load = LoadOp::kClear;
        float clear[4] = {0, 0, 0, 1};
    };

    struct DepthOutput {
        uint16_t tex = 0xFFFF;
        LoadOp load = LoadOp::kClear;
        float clear_depth = 1.0f;
    };

    struct PassRecord {
        std::string name;
        PassType type = PassType::kGraphics;
        SetupFn setup;
        ExecuteFn execute;
        std::vector<uint16_t> reads;
        std::vector<uint16_t> writes;
        std::vector<uint16_t> attachment_inputs;
        std::vector<ColorOutput> color_outputs;
        bool has_depth = false;
        DepthOutput depth_output;
    };

    GraphTexture AddTexture(const TexRecord& rec);
    GraphBuffer AddBuffer(const BufRecord& rec);

    Resources& resources_;
    Allocator& alloc_;

    std::vector<PassRecord> passes_;
    std::vector<TexRecord> textures_;
    std::vector<BufRecord> buffers_;
    GraphTexture output_;

    std::vector<uint32_t> topo_order_;
    std::vector<Handle<Texture>> resolved_tex_;
    std::vector<Handle<Buffer>> resolved_buf_;
};

}  // namespace cairns::rhi
