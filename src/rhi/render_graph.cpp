#include "util/define.hpp"

#include "rhi/render_graph.hpp"

#include "rhi/allocator.hpp"
#include "rhi/resources.hpp"

#include <cstring>

namespace cairns::rhi {

Handle<Texture> PassResources::Resolve(GraphTexture t) const {
    if (!textures_ || t.id >= textures_->size()) {
        return Handle<Texture>::Null;
    }
    return (*textures_)[t.id];
}

Handle<Buffer> PassResources::Resolve(GraphBuffer b) const {
    if (!buffers_ || b.id >= buffers_->size()) {
        return Handle<Buffer>::Null;
    }
    return (*buffers_)[b.id];
}

GraphTexture RenderGraph::AddTexture(const TexRecord& rec) {
    const uint16_t id = static_cast<uint16_t>(textures_.size());
    textures_.push_back(rec);
    return GraphTexture{id};
}

GraphBuffer RenderGraph::AddBuffer(const BufRecord& rec) {
    const uint16_t id = static_cast<uint16_t>(buffers_.size());
    buffers_.push_back(rec);
    return GraphBuffer{id};
}

GraphTexture PassBuilder::CreateColorTarget(const GraphTextureDesc& desc) {
    RenderGraph::TexRecord rec;
    rec.desc = desc;
    rec.kind = RenderGraph::ResKind::kCreated;
    return graph_->AddTexture(rec);
}

GraphTexture PassBuilder::CreateDepthTarget(const GraphTextureDesc& desc) {
    RenderGraph::TexRecord rec;
    rec.desc = desc;
    rec.kind = RenderGraph::ResKind::kCreated;
    return graph_->AddTexture(rec);
}

GraphTexture PassBuilder::ImportTexture(Handle<Texture> handle,
                                        const GraphTextureDesc& meta) {
    RenderGraph::TexRecord rec;
    rec.desc = meta;
    rec.kind = RenderGraph::ResKind::kImported;
    rec.imported = handle;
    return graph_->AddTexture(rec);
}

GraphBuffer PassBuilder::CreateBuffer(const GraphBufferDesc& desc) {
    RenderGraph::BufRecord rec;
    rec.desc = desc;
    rec.kind = RenderGraph::ResKind::kCreated;
    return graph_->AddBuffer(rec);
}

GraphBuffer PassBuilder::ImportBuffer(Handle<Buffer> handle,
                                      const GraphBufferDesc& meta) {
    RenderGraph::BufRecord rec;
    rec.desc = meta;
    rec.kind = RenderGraph::ResKind::kImported;
    rec.imported = handle;
    return graph_->AddBuffer(rec);
}

void PassBuilder::Read(GraphTexture t) {
    graph_->passes_[pass_].reads.push_back(t.id);
}

void PassBuilder::Write(GraphTexture t) {
    graph_->passes_[pass_].writes.push_back(t.id);
}

void PassBuilder::ReadWrite(GraphTexture t) {
    graph_->passes_[pass_].reads.push_back(t.id);
    graph_->passes_[pass_].writes.push_back(t.id);
}

void PassBuilder::AddColorOutput(const char* name, GraphTexture t, LoadOp load,
                                 const float clear[4]) {
    (void)name;
    RenderGraph::ColorOutput out;
    out.tex = t.id;
    out.load = load;
    out.clear[0] = clear[0];
    out.clear[1] = clear[1];
    out.clear[2] = clear[2];
    out.clear[3] = clear[3];
    graph_->passes_[pass_].color_outputs.push_back(out);
    graph_->passes_[pass_].writes.push_back(t.id);
}

void PassBuilder::AddDepthOutput(const char* name, GraphTexture t, LoadOp load,
                                 float clear_depth) {
    (void)name;
    RenderGraph::PassRecord& p = graph_->passes_[pass_];
    p.has_depth = true;
    p.depth_output.tex = t.id;
    p.depth_output.load = load;
    p.depth_output.clear_depth = clear_depth;
    p.writes.push_back(t.id);
}

void PassBuilder::AddAttachmentInput(GraphTexture t) {
    graph_->passes_[pass_].attachment_inputs.push_back(t.id);
    graph_->passes_[pass_].reads.push_back(t.id);
}

RenderGraph::RenderGraph(Resources& resources, Allocator& alloc)
    : resources_(resources), alloc_(alloc) {}

RenderGraph::~RenderGraph() = default;

void RenderGraph::Reset() {
    passes_.clear();
    textures_.clear();
    buffers_.clear();
    output_ = GraphTexture{};
    topo_order_.clear();
    resolved_tex_.clear();
    resolved_buf_.clear();
}

void RenderGraph::AddPass(const char* name, PassType type, SetupFn setup,
                          ExecuteFn execute) {
    const uint32_t idx = static_cast<uint32_t>(passes_.size());
    PassRecord rec;
    rec.name = name ? name : "";
    rec.type = type;
    rec.setup = std::move(setup);
    rec.execute = std::move(execute);
    passes_.push_back(std::move(rec));
    if (passes_[idx].setup) {
        PassBuilder builder(this, idx);
        passes_[idx].setup(builder);
    }
}

void RenderGraph::SetOutput(GraphTexture t) {
    output_ = t;
}

bool RenderGraph::Bake() {
    (void)resources_;
    (void)alloc_;
    return true;
}

bool RenderGraph::Execute(FrameContext& fc) {
    (void)fc;
    return true;
}

Handle<Texture> RenderGraph::ResolveTexture(GraphTexture t) const {
    if (t.id >= resolved_tex_.size()) {
        return Handle<Texture>::Null;
    }
    return resolved_tex_[t.id];
}

Handle<Buffer> RenderGraph::ResolveBuffer(GraphBuffer b) const {
    if (b.id >= resolved_buf_.size()) {
        return Handle<Buffer>::Null;
    }
    return resolved_buf_[b.id];
}

}  // namespace cairns::rhi
