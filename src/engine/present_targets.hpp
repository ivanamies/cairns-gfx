// engine/present_targets.hpp
//
// Swap / present / final-target state: the swapchain, the cross-thread
// present handshake (mutex/cv/queue), the surfaceless offscreen final
// target, and the resize lifecycle. Engine owns one, declared AFTER rhi_ so
// its swapchain tears down before the device.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include "rhi/resource_manager.hpp"
#include "rhi/swap_chain.hpp"

namespace cairns {

struct PresentTargets {
    rhi::SwapChain swapchain;

    // Present handoff: the render thread runs Frames::EndSubmit and stores
    // (fc, target) on the slot under m; the main thread waits on the SAME slot
    // it submitted to and runs Frames::Present. One frame of present lag. Every
    // backend shares the contract (MoltenVK reaches into CALayer).
    std::mutex m;
    std::condition_variable cv;
    [[maybe_unused]] int32_t prev_slot = -1;
    std::deque<int32_t> queue;

    // One-shot dump latch (golden / CAIRNS_DUMP path).
    bool dump_emitted = false;
    uint32_t dump_emit_frame = 0;

    // Headless / surfaceless (cairns_serve): the swap pass writes into this
    // offscreen target instead of a swapchain drawable. Null unless surfaceless.
    rhi::Handle<rhi::Texture> final_target = rhi::Handle<rhi::Texture>::Null;
    uint32_t final_target_w = 0;
    uint32_t final_target_h = 0;

    // Resize lifecycle: record intent + dims on the event thread, settle on the
    // next draw() with no in-flight frames. last_seen_swap catches Vulkan WSI
    // auto-recreate drift (OUT_OF_DATE without our intent path firing).
    bool resize_pending = false;
    uint32_t resize_pending_w = 0;
    uint32_t resize_pending_h = 0;
    uint32_t last_seen_swap_w = 0;
    uint32_t last_seen_swap_h = 0;
};

}  // namespace cairns
