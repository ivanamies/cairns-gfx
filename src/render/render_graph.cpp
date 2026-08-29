#include "util/define.hpp"

#include "render/render_graph.hpp"

#include "rhi/allocator.hpp"
#include "rhi/resources.hpp"
#include "rhi/swap_resolve_target.hpp"

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

namespace {

[[noreturn]] void PassCapAbort(std::string_view pass_name, const char* field,
                               uint32_t cap) {
    fprintf(stderr,
            "RenderGraph: pass '%.*s' exceeded %s cap (%u) -- raise the "
            "cap or split the pass.\n",
            static_cast<int>(pass_name.size()), pass_name.data(), field, cap);
    std::abort();
}

}  // namespace

#define PUSH_OR_DIE(pass, arr, cnt, cap, field, val) \
    do {                                              \
        if ((pass).cnt >= (cap)) {                    \
            PassCapAbort((pass).name, (field), (cap)); \
        }                                             \
        (pass).arr[(pass).cnt++] = (val);             \
    } while (0)

void PassBuilder::Read(GraphTexture t) {
    auto& p = graph_->passes_[pass_];
    PUSH_OR_DIE(p, reads, reads_count, RenderGraph::kMaxPassReads, "reads",
                t.id);
}

void PassBuilder::Write(GraphTexture t) {
    auto& p = graph_->passes_[pass_];
    PUSH_OR_DIE(p, writes, writes_count, RenderGraph::kMaxPassWrites,
                "writes", t.id);
}

void PassBuilder::ReadWrite(GraphTexture t) {
    auto& p = graph_->passes_[pass_];
    PUSH_OR_DIE(p, reads, reads_count, RenderGraph::kMaxPassReads, "reads",
                t.id);
    PUSH_OR_DIE(p, writes, writes_count, RenderGraph::kMaxPassWrites,
                "writes", t.id);
}

void PassBuilder::ReadBuffer(GraphBuffer b) {
    auto& p = graph_->passes_[pass_];
    PUSH_OR_DIE(p, buf_reads, buf_reads_count, RenderGraph::kMaxPassBufReads,
                "buf_reads", b.id);
}

void PassBuilder::WriteBuffer(GraphBuffer b) {
    auto& p = graph_->passes_[pass_];
    PUSH_OR_DIE(p, buf_writes, buf_writes_count,
                RenderGraph::kMaxPassBufWrites, "buf_writes", b.id);
}

void PassBuilder::AddColorOutput(const char* name, GraphTexture t, LoadOp load,
                                 const float clear[4], StoreOp store) {
    (void)name;
    auto& p = graph_->passes_[pass_];
    if (p.color_outputs_count >= GraphicsPipelineDesc::kMaxColorFormats) {
        PassCapAbort(p.name, "color_outputs",
                     GraphicsPipelineDesc::kMaxColorFormats);
    }
    RenderGraph::ColorOutput& out = p.color_outputs[p.color_outputs_count++];
    out.tex = t.id;
    out.load = load;
    out.store = store;
    out.clear[0] = clear[0];
    out.clear[1] = clear[1];
    out.clear[2] = clear[2];
    out.clear[3] = clear[3];
    PUSH_OR_DIE(p, writes, writes_count, RenderGraph::kMaxPassWrites,
                "writes", t.id);
}

void PassBuilder::AddDepthOutput(const char* name, GraphTexture t, LoadOp load,
                                 float clear_depth, StoreOp store) {
    (void)name;
    RenderGraph::PassRecord& p = graph_->passes_[pass_];
    p.has_depth = true;
    p.depth_output.tex = t.id;
    p.depth_output.load = load;
    p.depth_output.store = store;
    p.depth_output.clear_depth = clear_depth;
    PUSH_OR_DIE(p, writes, writes_count, RenderGraph::kMaxPassWrites,
                "writes", t.id);
}

void PassBuilder::AddAttachmentInput(GraphTexture t) {
    auto& p = graph_->passes_[pass_];
    PUSH_OR_DIE(p, attachment_inputs, attachment_inputs_count,
                RenderGraph::kMaxPassAttachmentInputs, "attachment_inputs",
                t.id);
    PUSH_OR_DIE(p, reads, reads_count, RenderGraph::kMaxPassReads, "reads",
                t.id);
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

// Flat-array variant: `claimed` is a fixed-size arena allocation, so pool
// entries pushed past claimed_n stay implicitly unclaimed for THIS Bake.
// tex_pool_ grow via push_back is rare (only on new-shape transients).
Handle<Texture> RenderGraph::AcquireTransientTexFlat(const GraphTextureDesc& desc,
                                                     uint8_t* claimed,
                                                     size_t claimed_n) {
    for (size_t i = 0; i < tex_pool_.size() && i < claimed_n; ++i) {
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
    return h;
}

void RenderGraph::BindSlotArena(uint32_t slot, cairns::BumpArena& arena) {
    if (slot < kMaxBoundSlots) {
        slot_arenas_[slot] = &arena;
    }
}

bool RenderGraph::Bake(uint32_t slot) {
    topo_order_.clear();
    resolved_tex_.assign(textures_.size(), Handle<Texture>::Null);
    resolved_buf_.assign(buffers_.size(), Handle<Buffer>::Null);

    const uint32_t n_pass = static_cast<uint32_t>(passes_.size());
    if (n_pass == 0) {
        return true;
    }
    const bool log = std::getenv("CAIRNS_RG_LOG") != nullptr;

    // All Bake() scratch on this slot's arena. Slot is the lock.
    assert(slot < kMaxBoundSlots && slot_arenas_[slot] != nullptr &&
           "RenderGraph::Bake: slot arena not bound -- call BindSlotArena");
    cairns::BumpArena& arena = *slot_arenas_[slot];
    const auto mark0 = arena.Mark();
    auto rewind_on_exit = [&arena, mark0]() { arena.Rewind(mark0); };

    // tex_writers / buf_writers as flat arena arrays + per-resource
    // {offset, count}. Two passes: count -> prefix-sum offsets -> fill.
    const size_t n_tex = textures_.size();
    const size_t n_buf = buffers_.size();
    uint32_t* tex_w_off = arena.AllocateArray<uint32_t>(n_tex ? n_tex : 1);
    uint32_t* tex_w_cnt = arena.AllocateArray<uint32_t>(n_tex ? n_tex : 1);
    uint32_t* buf_w_off = arena.AllocateArray<uint32_t>(n_buf ? n_buf : 1);
    uint32_t* buf_w_cnt = arena.AllocateArray<uint32_t>(n_buf ? n_buf : 1);
    for (size_t i = 0; i < n_tex; ++i) tex_w_cnt[i] = 0;
    for (size_t i = 0; i < n_buf; ++i) buf_w_cnt[i] = 0;
    for (uint32_t p = 0; p < n_pass; ++p) {
        const PassRecord& pr = passes_[p];
        for (uint8_t i = 0; i < pr.writes_count; ++i) ++tex_w_cnt[pr.writes[i]];
        for (uint8_t i = 0; i < pr.buf_writes_count; ++i)
            ++buf_w_cnt[pr.buf_writes[i]];
    }
    uint32_t tex_total = 0;
    for (size_t i = 0; i < n_tex; ++i) { tex_w_off[i] = tex_total; tex_total += tex_w_cnt[i]; }
    uint32_t buf_total = 0;
    for (size_t i = 0; i < n_buf; ++i) { buf_w_off[i] = buf_total; buf_total += buf_w_cnt[i]; }
    uint32_t* tex_w_data = arena.AllocateArray<uint32_t>(tex_total ? tex_total : 1);
    uint32_t* buf_w_data = arena.AllocateArray<uint32_t>(buf_total ? buf_total : 1);
    uint32_t* tex_head = arena.AllocateArray<uint32_t>(n_tex ? n_tex : 1);
    uint32_t* buf_head = arena.AllocateArray<uint32_t>(n_buf ? n_buf : 1);
    for (size_t i = 0; i < n_tex; ++i) tex_head[i] = 0;
    for (size_t i = 0; i < n_buf; ++i) buf_head[i] = 0;
    for (uint32_t p = 0; p < n_pass; ++p) {
        const PassRecord& pr = passes_[p];
        for (uint8_t i = 0; i < pr.writes_count; ++i) {
            const uint16_t w = pr.writes[i];
            tex_w_data[tex_w_off[w] + tex_head[w]++] = p;
        }
        for (uint8_t i = 0; i < pr.buf_writes_count; ++i) {
            const uint16_t w = pr.buf_writes[i];
            buf_w_data[buf_w_off[w] + buf_head[w]++] = p;
        }
    }
    auto tex_writers_span = [&](uint16_t t) {
        return std::span<const uint32_t>(tex_w_data + tex_w_off[t], tex_w_cnt[t]);
    };
    auto buf_writers_span = [&](uint16_t b) {
        return std::span<const uint32_t>(buf_w_data + buf_w_off[b], buf_w_cnt[b]);
    };

    // Step 1: reachability prune.
    uint8_t* alive = arena.AllocateArray<uint8_t>(n_pass);
    uint32_t* work_buf = arena.AllocateArray<uint32_t>(n_pass);
    for (uint32_t i = 0; i < n_pass; ++i) alive[i] = 0;
    uint32_t work_top = 0;
    auto mark = [&](uint32_t p) {
        if (!alive[p]) {
            alive[p] = 1;
            work_buf[work_top++] = p;
        }
    };
    if (!output_.IsNull() && output_.id < textures_.size()) {
        for (uint32_t p : tex_writers_span(output_.id)) mark(p);
    }
    for (uint32_t p = 0; p < n_pass; ++p) {
        const PassRecord& pr = passes_[p];
        for (uint8_t i = 0; i < pr.writes_count; ++i) {
            if (textures_[pr.writes[i]].kind == ResKind::kImported) mark(p);
        }
        for (uint8_t i = 0; i < pr.buf_writes_count; ++i) {
            if (buffers_[pr.buf_writes[i]].kind == ResKind::kImported)
                mark(p);
        }
    }
    if (output_.IsNull()) {
        for (uint32_t p = 0; p < n_pass; ++p) mark(p);
    }
    while (work_top > 0) {
        const uint32_t p = work_buf[--work_top];
        const PassRecord& pr = passes_[p];
        for (uint8_t i = 0; i < pr.reads_count; ++i) {
            for (uint32_t producer : tex_writers_span(pr.reads[i]))
                mark(producer);
        }
        for (uint8_t i = 0; i < pr.buf_reads_count; ++i) {
            for (uint32_t producer : buf_writers_span(pr.buf_reads[i]))
                mark(producer);
        }
    }

    // Step 2: topological sort. edges as flat arena array with {off,cnt}.
    uint32_t* indeg = arena.AllocateArray<uint32_t>(n_pass);
    uint32_t* edge_cnt = arena.AllocateArray<uint32_t>(n_pass);
    for (uint32_t i = 0; i < n_pass; ++i) { indeg[i] = 0; edge_cnt[i] = 0; }
    // Pass 1: count edges per source.
    for (uint32_t q = 0; q < n_pass; ++q) {
        if (!alive[q]) continue;
        const PassRecord& pr = passes_[q];
        for (uint8_t i = 0; i < pr.reads_count; ++i) {
            for (uint32_t p : tex_writers_span(pr.reads[i])) {
                if (p == q || !alive[p]) continue;
                ++edge_cnt[p];
                ++indeg[q];
            }
        }
        for (uint8_t i = 0; i < pr.buf_reads_count; ++i) {
            for (uint32_t p : buf_writers_span(pr.buf_reads[i])) {
                if (p == q || !alive[p]) continue;
                ++edge_cnt[p];
                ++indeg[q];
            }
        }
    }
    uint32_t* edge_off = arena.AllocateArray<uint32_t>(n_pass);
    uint32_t edge_total = 0;
    for (uint32_t i = 0; i < n_pass; ++i) { edge_off[i] = edge_total; edge_total += edge_cnt[i]; }
    uint32_t* edge_data = arena.AllocateArray<uint32_t>(edge_total ? edge_total : 1);
    uint32_t* edge_head = arena.AllocateArray<uint32_t>(n_pass);
    for (uint32_t i = 0; i < n_pass; ++i) edge_head[i] = 0;
    // indeg recomputed during fill; reset.
    for (uint32_t i = 0; i < n_pass; ++i) indeg[i] = 0;
    for (uint32_t q = 0; q < n_pass; ++q) {
        if (!alive[q]) continue;
        const PassRecord& pr = passes_[q];
        for (uint8_t i = 0; i < pr.reads_count; ++i) {
            for (uint32_t p : tex_writers_span(pr.reads[i])) {
                if (p == q || !alive[p]) continue;
                edge_data[edge_off[p] + edge_head[p]++] = q;
                ++indeg[q];
            }
        }
        for (uint8_t i = 0; i < pr.buf_reads_count; ++i) {
            for (uint32_t p : buf_writers_span(pr.buf_reads[i])) {
                if (p == q || !alive[p]) continue;
                edge_data[edge_off[p] + edge_head[p]++] = q;
                ++indeg[q];
            }
        }
    }
    uint8_t* done = arena.AllocateArray<uint8_t>(n_pass);
    for (uint32_t i = 0; i < n_pass; ++i) done[i] = 0;
    for (uint32_t iter = 0; iter < n_pass; ++iter) {
        int picked = -1;
        for (uint32_t p = 0; p < n_pass; ++p) {
            if (alive[p] && !done[p] && indeg[p] == 0) { picked = static_cast<int>(p); break; }
        }
        if (picked < 0) break;
        done[picked] = 1;
        topo_order_.push_back(static_cast<uint32_t>(picked));
        const uint32_t e_off = edge_off[picked];
        const uint32_t e_n = edge_cnt[picked];
        for (uint32_t k = 0; k < e_n; ++k) {
            const uint32_t q = edge_data[e_off + k];
            if (indeg[q] > 0) --indeg[q];
        }
    }

    // Step 3: per-texture lifetimes in topo positions.
    int* tex_first = arena.AllocateArray<int>(n_tex ? n_tex : 1);
    int* tex_last = arena.AllocateArray<int>(n_tex ? n_tex : 1);
    for (size_t i = 0; i < n_tex; ++i) { tex_first[i] = 0x7FFFFFFF; tex_last[i] = -1; }
    for (uint32_t idx = 0; idx < topo_order_.size(); ++idx) {
        const PassRecord& pass = passes_[topo_order_[idx]];
        const int pos = static_cast<int>(idx);
        for (uint8_t i = 0; i < pass.reads_count; ++i) {
            const uint16_t r = pass.reads[i];
            tex_first[r] = std::min(tex_first[r], pos);
            tex_last[r] = std::max(tex_last[r], pos);
        }
        for (uint8_t i = 0; i < pass.writes_count; ++i) {
            const uint16_t w = pass.writes[i];
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
    uint16_t* order = arena.AllocateArray<uint16_t>(n_tex ? n_tex : 1);
    uint32_t order_n = 0;
    for (uint16_t t = 0; t < n_tex; ++t) {
        if (textures_[t].kind == ResKind::kCreated && tex_last[t] >= 0) {
            order[order_n++] = t;
        }
    }
    std::sort(order, order + order_n,
              [&](uint16_t a, uint16_t b) { return tex_first[a] < tex_first[b]; });

    struct Slot {
        uint16_t desc_tex;
        int last;
        Handle<Texture> handle;
    };
    Slot* slots = arena.AllocateArray<Slot>(order_n ? order_n : 1);
    uint32_t slots_n = 0;
    const size_t n_pool = tex_pool_.size();
    uint8_t* pool_claimed = arena.AllocateArray<uint8_t>(n_pool ? n_pool : 1);
    for (size_t i = 0; i < n_pool; ++i) pool_claimed[i] = 0;
    for (uint32_t oi = 0; oi < order_n; ++oi) {
        const uint16_t t = order[oi];
        Handle<Texture> chosen = Handle<Texture>::Null;
        // Golden mode skips intra-frame slot aliasing so a transient never
        // reuses physical memory whose prior occupant's lifetime ended this
        // frame (the read-before-write hazard). Every transient gets its own
        // physical texture (AcquireTransientTexFlat; pool auto-grows).
        if (!disable_aliasing_) {
            for (uint32_t si = 0; si < slots_n; ++si) {
                Slot& s = slots[si];
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
        }
        if (chosen.IsNull()) {
            chosen = AcquireTransientTexFlat(textures_[t].desc, pool_claimed, n_pool);
            slots[slots_n++] = Slot{t, tex_last[t], chosen};
        }
        resolved_tex_[t] = chosen;
    }

    // Step 6: bake per-pass attachments + barrier inputs.
    for (uint32_t p : topo_order_) {
        PassRecord& pass = passes_[p];
        pass.baked_color_count = 0;
        pass.baked_inputs_count = 0;
        for (uint8_t i = 0; i < pass.color_outputs_count; ++i) {
            const ColorOutput& co = pass.color_outputs[i];
            // PassRecord::color_outputs is already capped at
            // kMaxColorFormats in AddColorOutput; baked_color shares the
            // same cap so we can drop the secondary check here.
            ColorAttachment& ca = pass.baked_color[pass.baked_color_count++];
            ca.target = resolved_tex_[co.tex];
            ca.clear[0] = co.clear[0];
            ca.clear[1] = co.clear[1];
            ca.clear[2] = co.clear[2];
            ca.clear[3] = co.clear[3];
            ca.load = co.load;
            ca.store = co.store;
        }
        if (pass.has_depth) {
            pass.baked_depth = DepthAttachment{};
            pass.baked_depth.depth = resolved_tex_[pass.depth_output.tex];
            pass.baked_depth.clear_depth = pass.depth_output.clear_depth;
            pass.baked_depth.load = pass.depth_output.load;
            pass.baked_depth.store = pass.depth_output.store;
        }
        for (uint8_t i = 0; i < pass.attachment_inputs_count; ++i) {
            // attachment_inputs cap == baked_inputs cap, so no need for a
            // second push_or_die here -- the AddAttachmentInput cap is the
            // gate. Asserted via static_assert in the header.
            pass.baked_inputs[pass.baked_inputs_count++] =
                resolved_tex_[pass.attachment_inputs[i]];
        }
    }

    if (log) {
        fprintf(stderr, "[RG] baked %zu passes (of %u), topo:",
                topo_order_.size(), n_pass);
        for (uint32_t p : topo_order_) {
            fprintf(stderr, " %.*s", static_cast<int>(passes_[p].name.size()),
                    passes_[p].name.data());
        }
        fprintf(stderr, "\n");
        for (uint16_t t = 0; t < textures_.size(); ++t) {
            fprintf(stderr, "[RG]   tex %u %s lifetime %d..%d\n", t,
                    textures_[t].kind == ResKind::kImported ? "import" : "transient",
                    tex_first[t] == 0x7FFFFFFF ? -1 : tex_first[t], tex_last[t]);
        }
    }
    rewind_on_exit();
    return true;
}

bool RenderGraph::Execute(FrameContext& fc, const SwapResolveTarget& target) {
    PassResources res(&resolved_tex_, &resolved_buf_);
    for (uint32_t p : topo_order_) {
        PassRecord& pass = passes_[p];
        if (pass.type == PassType::kCompute) {
            fc.cmd.PassTimerBegin(pass.name.data(), pass.type == PassType::kCompute);
            if (pass.execute) {
                pass.execute(fc.cmd, res);
            }
            fc.cmd.PassTimerEnd();
            continue;
        }
        RenderPassDesc rp{};
        rp.color = std::span<const ColorAttachment>(pass.baked_color.data(),
                                                    pass.baked_color_count);
        if (pass.has_depth) {
            rp.depth = pass.baked_depth;
        }
        rp.width = target.width;
        rp.height = target.height;
        rp.input_textures = std::span<const Handle<Texture>>(
            pass.baked_inputs.data(), pass.baked_inputs_count);
        // Granite §3.8 invalidate/flush, coarse-stage. Each accessed texture's
        // persistent PipelineEvent (Texture::Cold.sync) is the producer's last
        // state; emit an invalidate barrier on a pending flush (RAW/WAW -- the
        // cross-frame final_target_ WAW lives here) or a layout change, then
        // record the new flush state. The list is handed to the backend, which
        // executes it (vk: vkCmdPipelineBarrier; metal: per-resource MTLFence).
        ResourceBarrier invalidate[24];
        uint8_t inv_n = 0;
        Handle<Texture> flush[8];
        uint8_t flush_n = 0;
        auto access_tex = [&](Handle<Texture> h, uint32_t dst_access,
                              uint32_t dst_stage, BarrierLayout new_layout,
                              bool is_write) {
            if (h.IsNull()) {
                return;
            }
            Texture::Cold* cold = resources_.textures.GetCold(h);
            if (cold == nullptr) {
                return;
            }
            PipelineEvent& pe = cold->sync;
            const bool need =
                (pe.to_flush_access != 0) || (pe.layout != new_layout);
            if (need && inv_n < 24) {
                ResourceBarrier b{};
                b.texture = h;
                b.src_access = pe.to_flush_access;
                b.src_stage = pe.src_stages != 0
                                  ? pe.src_stages
                                  : static_cast<uint32_t>(kPipeAllCommands);
                b.dst_access = dst_access;
                b.dst_stage = dst_stage;
                b.old_layout = pe.layout;
                b.new_layout = new_layout;
                invalidate[inv_n++] = b;
            }
            pe.layout = new_layout;
            if (is_write) {
                pe.to_flush_access = dst_access;
                pe.src_stages = dst_stage;
                if (flush_n < 8) {
                    flush[flush_n++] = h;
                }
            } else {
                pe.to_flush_access = 0;  // a read consumes the pending flush
            }
        };
        for (uint8_t i = 0; i < pass.baked_inputs_count; ++i) {
            access_tex(pass.baked_inputs[i], kAccessShaderRead, kPipeFragment,
                       BarrierLayout::kShaderRead, false);
        }
        for (uint8_t i = 0; i < pass.baked_color_count; ++i) {
            access_tex(pass.baked_color[i].target, kAccessColorWrite,
                       kPipeColorOutput, BarrierLayout::kColorAttachment, true);
        }
        if (pass.has_depth) {
            access_tex(pass.baked_depth.depth, kAccessDepthWrite, kPipeDepth,
                       BarrierLayout::kDepthAttachment, true);
        }

        fc.cmd.PassTimerBegin(pass.name.data());
        fc.cmd.BeginRenderPass(
            resources_, target, rp,
            std::span<const ResourceBarrier>(invalidate, inv_n));
        if (pass.execute) {
            pass.execute(fc.cmd, res);
        }
        fc.cmd.EndRenderPass(resources_,
                             std::span<const Handle<Texture>>(flush, flush_n));
        fc.cmd.PassTimerEnd();
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

}  // namespace cairns::rhi
