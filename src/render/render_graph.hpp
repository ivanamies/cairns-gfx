#pragma once

// Render graph -- a partial copy of Themaister's Granite render graph.
//  - Passes declare reads/writes; Bake prunes unreachable passes,
//    topo-sorts, aliases transients by lifetime, and resolves physical
//    textures.
//  - Execute emits Granite-style invalidate/flush barriers; the backend
//    executes them (vk pipeline barriers / metal fences).

#include "render/worker_context.hpp"
#include "rhi/command_recorder.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/swap_resolve_target.hpp"
#include "util/inplace_function.hpp"  // No per-frame std::function malloc.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cairns::rhi {

class Resources;
class Allocator;
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

// InplaceFunction (fixed inline buffer) instead of std::function so the
// per-frame pass rebuild never heap-allocates the closures. 128 B fits all
// current pass closures; the ctor static_asserts fit.
using SetupFn = cairns::InplaceFunction<void(class PassBuilder&)>;
using ExecuteFn =
    cairns::InplaceFunction<void(CommandRecorder&, const PassResources&)>;

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
    void ReadBuffer(GraphBuffer b);
    void WriteBuffer(GraphBuffer b);

    // StoreOp is part of pass identity (hashed into RpKey) so
    // store-incompatible passes cannot alias a render pass object.
    void AddColorOutput(const char* name, GraphTexture t, LoadOp load,
                        const float clear[4],
                        StoreOp store = StoreOp::kStore);
    void AddDepthOutput(const char* name, GraphTexture t, LoadOp load,
                        float clear_depth,
                        StoreOp store = StoreOp::kStore);
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
    // #210: caller passes ITS SLOT INDEX. RenderGraph looks up that slot's
    // arena from its pre-registered slot table (BindSlotArena). Slot index
    // is the lock -- no two callers can ever hand the same slot to Bake
    // concurrently because the slot was Acquire'd exclusively.
    void BindSlotArena(uint32_t slot, BumpArena& arena);
    // #229 GPU-determinism: when set, Bake gives every created transient its own
    // physical texture instead of intra-frame aliasing a slot whose lifetime
    // ended. Aliased memory is UNDEFINED until written -> a read-before-write is
    // bistable + sync-sensitive (the top suspect for the three_champ flake).
    // Behaviour-preserving (pure memory layout); the pool auto-grows. Set in
    // golden mode.
    void SetDisableTransientAliasing(bool on) { disable_aliasing_ = on; }
    bool Bake(uint32_t slot);
    bool Execute(FrameContext& fc, const SwapResolveTarget& target);

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
        StoreOp store = StoreOp::kStore;  // #222 Phase A.2
        float clear[4] = {0, 0, 0, 1};
    };

    struct DepthOutput {
        uint16_t tex = 0xFFFF;
        LoadOp load = LoadOp::kClear;
        StoreOp store = StoreOp::kStore;  // #222 Phase A.2
        float clear_depth = 1.0f;
    };

    // Fixed-cap stacks on every PassRecord so AddPass/Bake never
    // allocate per-frame. Caps sized for the swap pass at kNumViewports
    // = 8 (today's = 4 + 2x headroom): reads/attachment_inputs = 2*N.
    // Overflow is push_or_die (printed diagnostic + abort), matching
    // baked_color and render_extract's DFS scratch stack.
    static constexpr uint32_t kMaxPassReads = 16;
    static constexpr uint32_t kMaxPassWrites = 8;
    static constexpr uint32_t kMaxPassBufReads = 4;
    static constexpr uint32_t kMaxPassBufWrites = 4;
    static constexpr uint32_t kMaxPassAttachmentInputs = 16;
    static constexpr uint32_t kMaxPassBakedInputs = 16;
    static_assert(kMaxPassAttachmentInputs == kMaxPassBakedInputs,
                  "baked_inputs cap must match attachment_inputs so Bake "
                  "can copy without a second push_or_die");

    struct PassRecord {
        std::string_view name;  // #214 callers pass string literals
        PassType type = PassType::kGraphics;
        SetupFn setup;
        ExecuteFn execute;
        std::array<uint16_t, kMaxPassReads> reads{};
        uint8_t reads_count = 0;
        std::array<uint16_t, kMaxPassWrites> writes{};
        uint8_t writes_count = 0;
        std::array<uint16_t, kMaxPassBufReads> buf_reads{};
        uint8_t buf_reads_count = 0;
        std::array<uint16_t, kMaxPassBufWrites> buf_writes{};
        uint8_t buf_writes_count = 0;
        std::array<uint16_t, kMaxPassAttachmentInputs> attachment_inputs{};
        uint8_t attachment_inputs_count = 0;
        std::array<ColorOutput, GraphicsPipelineDesc::kMaxColorFormats>
            color_outputs{};
        uint8_t color_outputs_count = 0;
        bool has_depth = false;
        DepthOutput depth_output;
        // Bounded by GraphicsPipelineDesc::kMaxColorFormats (=4, #206 MRT).
        // Fixed cap on the stack -> no per-frame std::vector reallocation
        // (was ~77 KB / 1849 grows in the 3300-hero trace).
        std::array<ColorAttachment, GraphicsPipelineDesc::kMaxColorFormats>
            baked_color{};
        uint8_t baked_color_count = 0;
        DepthAttachment baked_depth;
        std::array<Handle<Texture>, kMaxPassBakedInputs> baked_inputs{};
        uint8_t baked_inputs_count = 0;
    };

    struct PooledTex {
        GraphTextureDesc desc;
        Handle<Texture> handle;
    };

    struct PooledBuf {
        GraphBufferDesc desc;
        Handle<Buffer> handle;
    };

    GraphTexture AddTexture(const TexRecord& rec);
    GraphBuffer AddBuffer(const BufRecord& rec);
    Handle<Texture> AcquireTransientTexFlat(const GraphTextureDesc& desc,
                                             uint8_t* claimed, size_t claimed_n);
    Handle<Texture> AcquireTransientTex(const GraphTextureDesc& desc,
                                        std::vector<uint8_t>& claimed);

    Resources& resources_;
    Allocator& alloc_;

    std::vector<PassRecord> passes_;
    std::vector<TexRecord> textures_;
    std::vector<BufRecord> buffers_;
    GraphTexture output_;

    std::vector<uint32_t> topo_order_;
    std::vector<Handle<Texture>> resolved_tex_;
    std::vector<Handle<Buffer>> resolved_buf_;
    std::vector<PooledTex> tex_pool_;
    std::vector<PooledBuf> buf_pool_;
    bool disable_aliasing_ = false;  // #229 golden-mode determinism

    // #210 per-slot arena table. Engine binds once at init; Bake(slot)
    // resolves slot_arenas_[slot] -> the BumpArena that owns Bake's
    // scratch. nullptr until BindSlotArena fires for that slot.
    static constexpr uint32_t kMaxBoundSlots = 4;
    BumpArena* slot_arenas_[kMaxBoundSlots] = {nullptr, nullptr, nullptr, nullptr};
};

}  // namespace cairns::rhi
