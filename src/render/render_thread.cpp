// src/render/render_thread.cpp
//
// Implementation lives behind a pimpl so engine.hpp (and every TU that
// includes it) doesn't drag taskflow + atomics + condition_variable into
// its translation unit -- taskflow needs -fexceptions and has narrowing
// warnings the rest of the project compiles with -Werror against.

#include "render/render_thread.hpp"

#include "render/frame_packet.hpp"

#include <taskflow/taskflow.hpp>

#include <array>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace cairns {

namespace {

enum SlotState : uint8_t {
    kIdle = 0,
    kProducerWriting = 1,
    kPublished = 2,
    kConsumerRecording = 3,
};

}  // namespace

// State machine (per slot):
//   kIdle              -> kProducerWriting     (game-thread Acquire)
//   kProducerWriting   -> kPublished           (game-thread Submit)
//   kPublished         -> kConsumerRecording   (worker pick)
//   kConsumerRecording -> kIdle                (worker release)
//
// Memory ordering: Submit's mutex release synchronizes-with the worker's
// mutex acquire -- every slot-S write done by the game thread before Submit
// happens-before all the worker's slot-S reads. One lock covers it.
struct RenderThread::Impl {
    std::function<void(FramePacket&)> record_fn;
    // Null in inline mode: no worker thread, record_fn runs synchronously at
    // Submit on the calling thread (single-threaded builds, e.g. the browser).
    std::unique_ptr<tf::Executor> exec;
    tf::Taskflow taskflow;

    std::mutex m;
    std::condition_variable cv;
    std::array<SlotState, RenderThread::kFramesInFlight> slot_state{};
    std::array<FramePacket*, RenderThread::kFramesInFlight> published_pkt{};
    bool shutting_down = false;

    Impl(std::function<void(FramePacket&)> fn, bool background)
        : record_fn(std::move(fn)) {
        for (uint32_t i = 0; i < RenderThread::kFramesInFlight; ++i) {
            slot_state[i] = kIdle;
            published_pkt[i] = nullptr;
        }
        if (background) {
            exec = std::make_unique<tf::Executor>(1);
            taskflow.emplace([this]() { WorkerLoop(); });
            exec->run(taskflow);
        }
    }

    void WorkerLoop() {
        while (true) {
            FramePacket* pkt = nullptr;
            uint32_t picked_slot = 0;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] {
                    if (shutting_down) {
                        return true;
                    }
                    for (uint32_t i = 0; i < RenderThread::kFramesInFlight; ++i) {
                        if (slot_state[i] == kPublished) {
                            return true;
                        }
                    }
                    return false;
                });
                if (shutting_down) {
                    return;
                }
                for (uint32_t i = 0; i < RenderThread::kFramesInFlight; ++i) {
                    if (slot_state[i] == kPublished) {
                        picked_slot = i;
                        pkt = published_pkt[i];
                        slot_state[i] = kConsumerRecording;
                        break;
                    }
                }
            }
            if (pkt && record_fn) {
                record_fn(*pkt);
            }
            {
                std::lock_guard<std::mutex> lk(m);
                published_pkt[picked_slot] = nullptr;
                slot_state[picked_slot] = kIdle;
            }
            cv.notify_all();
        }
    }
};

RenderThread::RenderThread(std::function<void(FramePacket&)> record_fn,
                           bool background)
    : impl_(std::make_unique<Impl>(std::move(record_fn), background)) {}

RenderThread::~RenderThread() {
    Shutdown();
}

void RenderThread::Acquire(uint32_t slot) {
    // Inline: no consumer thread, so the slot is free as soon as the prior
    // Submit returned (it ran record_fn synchronously). No wait.
    if (!impl_->exec) {
        impl_->slot_state[slot] = kProducerWriting;
        return;
    }
    std::unique_lock<std::mutex> lk(impl_->m);
    impl_->cv.wait(lk, [&] {
        return impl_->shutting_down || impl_->slot_state[slot] == kIdle;
    });
    if (impl_->shutting_down) {
        return;
    }
    impl_->slot_state[slot] = kProducerWriting;
}

void RenderThread::Submit(uint32_t slot, FramePacket* pkt) {
    if (!impl_->exec) {
        if (pkt && impl_->record_fn) {
            impl_->record_fn(*pkt);
        }
        impl_->slot_state[slot] = kIdle;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->published_pkt[slot] = pkt;
        impl_->slot_state[slot] = kPublished;
    }
    impl_->cv.notify_all();
}

void RenderThread::Drain() {
    if (!impl_->exec) {
        return;  // inline: nothing is ever in flight
    }
    std::unique_lock<std::mutex> lk(impl_->m);
    impl_->cv.wait(lk, [&] {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (impl_->slot_state[i] != kIdle) {
                return false;
            }
        }
        return true;
    });
}

void RenderThread::Shutdown() {
    if (!impl_ || !impl_->exec) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        if (impl_->shutting_down) {
            return;
        }
        impl_->shutting_down = true;
    }
    impl_->cv.notify_all();
    impl_->exec->wait_for_all();
}

}  // namespace cairns
