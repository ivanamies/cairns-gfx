#include "util/define.hpp"

#include "render/render_graph.hpp"

#include "rhi/allocator.hpp"
#include "rhi/resources.hpp"
#include "rhi/swap_chain.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

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

void PassBuilder::ReadBuffer(GraphBuffer b) {
    graph_->passes_[pass_].buf_reads.push_back(b.id);
}

void PassBuilder::WriteBuffer(GraphBuffer b) {
    graph_->passes_[pass_].buf_writes.push_back(b.id);
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

static bool DescEq(const GraphTextureDesc& a, const GraphTextureDesc& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format &&
           a.samples == b.samples && a.usage == b.usage;
}

Handle<Texture> RenderGraph::AcquireTransientTex(const GraphTextureDesc& desc,
                                                 std::vector<uint8_t>& claimed) {
    for (size_t i = 0; i < tex_pool_.size(); ++i) {
        if (!claimed[i] && DescEq(tex_pool_[i].desc, desc)) {
            claimed[i] = 1;
            return tex_pool_[i].handle;
        }
    }
    TextureDesc td;
    td.dimensions = {static_cast<int32_t>(desc.width),
                     static_cast<int32_t>(desc.height), 1};
    td.format = desc.format;
    td.sample_count = desc.samples;
    td.usage = desc.usage;
    td.memory = Memory::kDefault;
    const Handle<Texture> h = resources_.CreateTexture(alloc_, td);
    tex_pool_.push_back({desc, h});
    claimed.push_back(1);
    return h;
}

bool RenderGraph::Bake() {
    topo_order_.clear();
    resolved_tex_.assign(textures_.size(), Handle<Texture>::Null);
    resolved_buf_.assign(buffers_.size(), Handle<Buffer>::Null);

    const uint32_t n_pass = static_cast<uint32_t>(passes_.size());
    if (n_pass == 0) {
        return true;
    }
    const bool log = std::getenv("CAIRNS_RG_LOG") != nullptr;

    std::vector<std::vector<uint32_t>> tex_writers(textures_.size());
    std::vector<std::vector<uint32_t>> buf_writers(buffers_.size());
    for (uint32_t p = 0; p < n_pass; ++p) {
        for (uint16_t w : passes_[p].writes) {
            tex_writers[w].push_back(p);
        }
        for (uint16_t w : passes_[p].buf_writes) {
            buf_writers[w].push_back(p);
        }
    }

    // Step 1: reachability prune. Roots = passes that write the output OR write any
    // imported resource (external side effect, e.g. the persistent particle SSBO).
    std::vector<uint8_t> alive(n_pass, 0);
    std::vector<uint32_t> work;
    auto mark = [&](uint32_t p) {
        if (!alive[p]) {
            alive[p] = 1;
            work.push_back(p);
        }
    };
    if (!output_.IsNull() && output_.id < textures_.size()) {
        for (uint32_t p : tex_writers[output_.id]) {
            mark(p);
        }
    }
    for (uint32_t p = 0; p < n_pass; ++p) {
        for (uint16_t w : passes_[p].writes) {
            if (textures_[w].kind == ResKind::kImported) {
                mark(p);
            }
        }
        for (uint16_t w : passes_[p].buf_writes) {
            if (buffers_[w].kind == ResKind::kImported) {
                mark(p);
            }
        }
    }
    if (output_.IsNull()) {
        for (uint32_t p = 0; p < n_pass; ++p) {
            mark(p);
        }
    }
    while (!work.empty()) {
        const uint32_t p = work.back();
        work.pop_back();
        for (uint16_t r : passes_[p].reads) {
            for (uint32_t producer : tex_writers[r]) {
                mark(producer);
            }
        }
        for (uint16_t r : passes_[p].buf_reads) {
            for (uint32_t producer : buf_writers[r]) {
                mark(producer);
            }
        }
    }

    // Step 2: topological sort (write -> read deps), stable in registration order.
    std::vector<uint32_t> indeg(n_pass, 0);
    std::vector<std::vector<uint32_t>> edges(n_pass);
    for (uint32_t q = 0; q < n_pass; ++q) {
        if (!alive[q]) {
            continue;
        }
        for (uint16_t r : passes_[q].reads) {
            for (uint32_t p : tex_writers[r]) {
                if (p == q || !alive[p]) {
                    continue;
                }
                edges[p].push_back(q);
                indeg[q]++;
            }
        }
        for (uint16_t r : passes_[q].buf_reads) {
            for (uint32_t p : buf_writers[r]) {
                if (p == q || !alive[p]) {
                    continue;
                }
                edges[p].push_back(q);
                indeg[q]++;
            }
        }
    }
    std::vector<uint8_t> done(n_pass, 0);
    for (uint32_t iter = 0; iter < n_pass; ++iter) {
        int picked = -1;
        for (uint32_t p = 0; p < n_pass; ++p) {
            if (alive[p] && !done[p] && indeg[p] == 0) {
                picked = static_cast<int>(p);
                break;
            }
        }
        if (picked < 0) {
            break;
        }
        done[picked] = 1;
        topo_order_.push_back(static_cast<uint32_t>(picked));
        for (uint32_t q : edges[picked]) {
            if (indeg[q] > 0) {
                indeg[q]--;
            }
        }
    }

    // Step 3: per-texture lifetimes in topo positions.
    std::vector<int> tex_first(textures_.size(), 0x7FFFFFFF);
    std::vector<int> tex_last(textures_.size(), -1);
    for (uint32_t idx = 0; idx < topo_order_.size(); ++idx) {
        const PassRecord& pass = passes_[topo_order_[idx]];
        const int pos = static_cast<int>(idx);
        for (uint16_t r : pass.reads) {
            tex_first[r] = std::min(tex_first[r], pos);
            tex_last[r] = std::max(tex_last[r], pos);
        }
        for (uint16_t w : pass.writes) {
            tex_first[w] = std::min(tex_first[w], pos);
            tex_last[w] = std::max(tex_last[w], pos);
        }
    }

    // Imports resolve to their external handle.
    for (uint16_t t = 0; t < textures_.size(); ++t) {
        if (textures_[t].kind == ResKind::kImported) {
            resolved_tex_[t] = textures_[t].imported;
        }
    }
    for (uint16_t b = 0; b < buffers_.size(); ++b) {
        if (buffers_[b].kind == ResKind::kImported) {
            resolved_buf_[b] = buffers_[b].imported;
        }
    }

    // Steps 4+5: conservative aliasing + physical alloc for created transients.
    std::vector<uint16_t> order;
    for (uint16_t t = 0; t < textures_.size(); ++t) {
        if (textures_[t].kind == ResKind::kCreated && tex_last[t] >= 0) {
            order.push_back(t);
        }
    }
    std::sort(order.begin(), order.end(),
              [&](uint16_t a, uint16_t b) { return tex_first[a] < tex_first[b]; });

    struct Slot {
        uint16_t desc_tex;
        int last;
        Handle<Texture> handle;
    };
    std::vector<Slot> slots;
    std::vector<uint8_t> pool_claimed(tex_pool_.size(), 0);
    for (uint16_t t : order) {
        Handle<Texture> chosen = Handle<Texture>::Null;
        for (Slot& s : slots) {
            if (s.last < tex_first[t] &&
                DescEq(textures_[s.desc_tex].desc, textures_[t].desc)) {
                chosen = s.handle;
                s.last = tex_last[t];
                s.desc_tex = t;
                if (log) {
                    fprintf(stderr, "[RG] alias tex %u -> reuse slot (lifetime %d..%d)\n",
                            t, tex_first[t], tex_last[t]);
                }
                break;
            }
        }
        if (chosen.IsNull()) {
            chosen = AcquireTransientTex(textures_[t].desc, pool_claimed);
            slots.push_back({t, tex_last[t], chosen});
        }
        resolved_tex_[t] = chosen;
    }

    // Step 6: bake per-pass attachments + barrier inputs.
    for (uint32_t p : topo_order_) {
        PassRecord& pass = passes_[p];
        pass.baked_color.clear();
        pass.baked_inputs.clear();
        for (const ColorOutput& co : pass.color_outputs) {
            ColorAttachment ca;
            ca.target = resolved_tex_[co.tex];
            ca.clear[0] = co.clear[0];
            ca.clear[1] = co.clear[1];
            ca.clear[2] = co.clear[2];
            ca.clear[3] = co.clear[3];
            ca.load = co.load;
            pass.baked_color.push_back(ca);
        }
        if (pass.has_depth) {
            pass.baked_depth = DepthAttachment{};
            pass.baked_depth.depth = resolved_tex_[pass.depth_output.tex];
            pass.baked_depth.clear_depth = pass.depth_output.clear_depth;
            pass.baked_depth.load = pass.depth_output.load;
        }
        for (uint16_t in : pass.attachment_inputs) {
            pass.baked_inputs.push_back(resolved_tex_[in]);
        }
        // Render-area extent: the size of this pass's first output target.
        pass.baked_width = 0;
        pass.baked_height = 0;
        if (!pass.color_outputs.empty()) {
            const GraphTextureDesc& d = textures_[pass.color_outputs[0].tex].desc;
            pass.baked_width = d.width;
            pass.baked_height = d.height;
        } else if (pass.has_depth) {
            const GraphTextureDesc& d = textures_[pass.depth_output.tex].desc;
            pass.baked_width = d.width;
            pass.baked_height = d.height;
        }
    }

    if (log) {
        fprintf(stderr, "[RG] baked %zu passes (of %u), topo:",
                topo_order_.size(), n_pass);
        for (uint32_t p : topo_order_) {
            fprintf(stderr, " %s", passes_[p].name.c_str());
        }
        fprintf(stderr, "\n");
        for (uint16_t t = 0; t < textures_.size(); ++t) {
            fprintf(stderr, "[RG]   tex %u %s lifetime %d..%d\n", t,
                    textures_[t].kind == ResKind::kImported ? "import" : "transient",
                    tex_first[t] == 0x7FFFFFFF ? -1 : tex_first[t], tex_last[t]);
        }
    }
    return true;
}

bool RenderGraph::Execute(FrameContext& fc, SwapChain& sc) {
    PassResources res(&resolved_tex_, &resolved_buf_);
    for (uint32_t p : topo_order_) {
        PassRecord& pass = passes_[p];
        if (pass.type == PassType::kCompute) {
            if (pass.execute) {
                pass.execute(fc.cmd, res);
            }
            continue;
        }
        RenderPassDesc rp{};
        rp.color = std::span<const ColorAttachment>(pass.baked_color.data(),
                                                    pass.baked_color.size());
        if (pass.has_depth) {
            rp.depth = pass.baked_depth;
        }
        rp.width = pass.baked_width ? pass.baked_width : sc.Width();
        rp.height = pass.baked_height ? pass.baked_height : sc.Height();
        rp.input_textures = std::span<const Handle<Texture>>(
            pass.baked_inputs.data(), pass.baked_inputs.size());
        fc.cmd.BeginRenderPass(resources_, sc, rp);
        if (pass.execute) {
            pass.execute(fc.cmd, res);
        }
        fc.cmd.EndRenderPass();
    }
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

void RenderGraphToyTest(Resources& resources, Allocator& alloc) {
    RenderGraph g(resources, alloc);
    GraphTexture t_off;
    GraphTexture t_out;
    g.AddPass("toyA", PassType::kGraphics,
              [&](PassBuilder& b) {
                  GraphTextureDesc d;
                  d.width = 256;
                  d.height = 256;
                  d.format = Format::kRgba8Unorm;
                  d.usage = kTexUsageColorTarget | kTexUsageSampled;
                  t_off = b.CreateColorTarget(d);
                  const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                  b.AddColorOutput("off", t_off, LoadOp::kClear, clear);
              },
              [](CommandRecorder&, const PassResources&) {});
    g.AddPass("toyB", PassType::kGraphics,
              [&](PassBuilder& b) {
                  b.AddAttachmentInput(t_off);
                  GraphTextureDesc d;
                  d.width = 256;
                  d.height = 256;
                  d.format = Format::kRgba8Unorm;
                  d.usage = kTexUsageColorTarget | kTexUsageSampled;
                  t_out = b.CreateColorTarget(d);
                  const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                  b.AddColorOutput("out", t_out, LoadOp::kClear, clear);
              },
              [](CommandRecorder&, const PassResources&) {});
    g.SetOutput(t_out);
    const bool ok = g.Bake();
    fprintf(stderr, "[RG] toy bake ok=%d\n", ok ? 1 : 0);
}

}  // namespace cairns::rhi
