// src/render/render_thread.hpp
//
// Game/render thread handoff. The game thread Acquires a slot BEFORE writing
// any per-slot storage (this is the backpressure point); writes; Submits;
// returns to the loop. The single render worker waits for a Published slot,
// calls engine.RecordFrame(pkt), then Releases the slot.
//
// State machine (per slot):
//   kIdle              -> kProducerWriting     (game-thread Acquire)
//   kProducerWriting   -> kPublished           (game-thread Submit)
//   kPublished         -> kConsumerRecording   (render-worker pick)
//   kConsumerRecording -> kIdle                (render-worker Release)
//
// Backpressure: Acquire(S) blocks until slot_state_[S] == kIdle.
// Drain(): blocks until all slots are kIdle (CAIRNS_DUMP lockstep).
//
// Memory ordering: Submit's mutex release synchronizes-with the worker's
// mutex acquire -- every slot-S write the game thread did before Submit
// happens-before all the worker's slot-S reads. One lock covers it; no
// per-field atomics on the packet.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "rhi/resource_manager.hpp"  // cairns::rhi::kFramesInFlight

namespace cairns {

struct FramePacket;

class RenderThread {
public:
    // #222 Phase T.1: re-export of cairns::rhi::kFramesInFlight.
    static constexpr uint32_t kFramesInFlight = cairns::rhi::kFramesInFlight;

    // record_fn records each published packet. background=true runs it on a
    // dedicated worker thread (native, double-buffered); background=false runs
    // it inline at Submit on the calling thread (single-threaded, e.g. the
    // browser where Web Worker pthreads + WebGPU are deferred to W6b).
    RenderThread(std::function<void(FramePacket&)> record_fn, bool background);
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // Game-thread API. See state machine in render_thread.cpp.
    void Acquire(uint32_t slot);
    void Submit(uint32_t slot, FramePacket* pkt);
    void Drain();
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cairns
