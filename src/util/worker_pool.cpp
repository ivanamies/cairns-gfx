// src/util/worker_pool.cpp
//
// Pimpl lives here so taskflow's headers don't leak into cairns_core.
// Same exception/warning posture as render_thread.cpp; CMake puts both
// TUs into the cairns_render_thread STATIC library.

#include "util/worker_pool.hpp"

#include <taskflow/taskflow.hpp>

#include <condition_variable>
#include <mutex>

namespace cairns {

struct WorkerPool::Impl {
    // Null => inline (single-threaded): the calling thread runs the whole
    // fan-out. Used where worker threads aren't wanted yet (the browser: WebGPU
    // + Web Worker pthreads is fragile, so W6a stays single-threaded).
    std::unique_ptr<tf::Executor> exec;
    explicit Impl(uint32_t n) {
        if (n > 0) {
            exec = std::make_unique<tf::Executor>(n);
        }
    }
};

WorkerPool::WorkerPool(uint32_t n_workers)
    : impl_(std::make_unique<Impl>(n_workers)) {}

WorkerPool::~WorkerPool() = default;

uint32_t WorkerPool::num_workers() const {
    // Inline mode still presents one logical worker (the caller) so callers that
    // partition by num_workers() run the whole range on this thread.
    return impl_->exec ? static_cast<uint32_t>(impl_->exec->num_workers()) : 1u;
}

void WorkerPool::RunIndices(uint32_t n_tasks,
                            const std::function<void(uint32_t)>& body) {
    if (n_tasks == 0) {
        return;
    }
    if (!impl_->exec || n_tasks == 1) {
        for (uint32_t i = 0; i < n_tasks; ++i) {
            body(i);
        }
        return;
    }
    // mutex + cv join. The workers (1..n_tasks-1) each decrement a count
    // under the lock; the caller waits on the cv until count hits 0.
    // No semaphore / latch / barrier per the threading-primitives memory
    // rule -- only std::mutex / std::condition_variable.
    std::mutex m;
    std::condition_variable cv;
    uint32_t remaining = n_tasks - 1;
    for (uint32_t i = 1; i < n_tasks; ++i) {
        impl_->exec->silent_async([&, i]() {
            body(i);
            std::lock_guard<std::mutex> lk(m);
            --remaining;
            if (remaining == 0) {
                cv.notify_one();
            }
        });
    }
    // The calling thread runs task 0 productively while workers run the rest.
    body(0);
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return remaining == 0; });
}

}  // namespace cairns
