// src/render/render_graph_schedule.hpp
//
// PURE render-graph scheduling: topo-sort the passes by resource read/write
// dependency and prune passes that don't reach the declared output. No
// Resources, no Allocator, no GPU -- just the dependency DAG.

#ifndef CAIRNS_RENDER_GRAPH_SCHEDULE_HPP
#define CAIRNS_RENDER_GRAPH_SCHEDULE_HPP

#include <cstdint>
#include <vector>

namespace cairns {

// One pass as the scheduler sees it: which resource ids it reads / writes.
// Resource ids are opaque small integers (the graph's GraphTexture/GraphBuffer
// ids). A pass depends on every pass that writes a resource it reads.
struct SchedPass {
    std::vector<uint16_t> reads;
    std::vector<uint16_t> writes;
};

struct SchedResult {
    std::vector<uint32_t> order;  // pass indices, dependency-respecting
    bool cycle = false;           // true => a write/read cycle was detected
};

// Topo-sort passes so every writer precedes every reader of a resource, keeping
// only passes that (transitively) feed a pass writing `output_resource`. Passes
// are visited in submission order among otherwise-independent passes, so the
// result is deterministic across platforms.
inline SchedResult SchedulePasses(const std::vector<SchedPass>& passes,
                                  uint16_t output_resource) {
    const size_t n = passes.size();
    SchedResult res;

    auto find_writers = [&](uint16_t r, std::vector<uint32_t>& out) {
        for (uint32_t i = 0; i < n; ++i) {
            for (uint16_t w : passes[i].writes) {
                if (w == r) {
                    out.push_back(i);
                    break;
                }
            }
        }
    };

    // 1) reachability prune: DFS back from any pass writing the output.
    std::vector<uint8_t> keep(n, 0);
    std::vector<uint8_t> seen(n, 0);
    std::vector<uint32_t> stack;
    {
        std::vector<uint32_t> seed;
        find_writers(output_resource, seed);
        for (uint32_t s : seed) {
            stack.push_back(s);
        }
    }
    while (!stack.empty()) {
        const uint32_t p = stack.back();
        stack.pop_back();
        if (seen[p]) {
            continue;
        }
        seen[p] = 1;
        keep[p] = 1;
        for (uint16_t r : passes[p].reads) {
            std::vector<uint32_t> ws;
            find_writers(r, ws);
            for (uint32_t w : ws) {
                if (!seen[w]) {
                    stack.push_back(w);
                }
            }
        }
    }

    // 2) Kahn topo-sort over kept passes; ties broken by submission index for
    // determinism. Edge p_writer -> p_reader for each read.
    std::vector<uint32_t> indeg(n, 0);
    std::vector<std::vector<uint32_t>> succ(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!keep[i]) {
            continue;
        }
        for (uint16_t r : passes[i].reads) {
            std::vector<uint32_t> ws;
            find_writers(r, ws);
            for (uint32_t w : ws) {
                if (!keep[w] || w == i) {
                    continue;
                }
                succ[w].push_back(i);
                ++indeg[i];
            }
        }
    }
    std::vector<uint32_t> ready;
    for (uint32_t i = 0; i < n; ++i) {
        if (keep[i] && indeg[i] == 0) {
            ready.push_back(i);
        }
    }
    // ready is already in ascending submission order.
    size_t cursor = 0;
    while (cursor < ready.size()) {
        const uint32_t p = ready[cursor++];
        res.order.push_back(p);
        for (uint32_t s : succ[p]) {
            if (--indeg[s] == 0) {
                // insert keeping ascending order among the newly-ready
                auto it = ready.begin() + static_cast<long>(cursor);
                while (it != ready.end() && *it < s) {
                    ++it;
                }
                ready.insert(it, s);
            }
        }
    }

    size_t kept = 0;
    for (uint8_t k : keep) {
        kept += k;
    }
    res.cycle = (res.order.size() != kept);
    return res;
}

}  // namespace cairns

#endif  // CAIRNS_RENDER_GRAPH_SCHEDULE_HPP
