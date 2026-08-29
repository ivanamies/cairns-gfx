// src/util/worker_pool.hpp
//
// Persistent worker pool for parallel-for fan-out. Backed by taskflow's
// tf::Executor under a pimpl so the public header doesn't drag taskflow
// (exceptions + narrowing warnings) into the cairns_core TUs that include
// it. Same split as render_thread.cpp: the impl lives in the
// cairns_render_thread static library where the relaxed compile flags
// already allow taskflow.
//
// Why pool: pthread_create on macOS is ~50us per spawn, ~20us on linux/
// android. Fan-out at 60Hz with a fresh-thread-per-frame model burns
// hundreds of microseconds in spawn alone before doing any work. A
// persistent pool wakes workers via condition_variable instead.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace cairns {

class WorkerPool {
public:
    // n_workers = 0 collapses to 1; the calling thread is always the one
    // that runs task 0, so n_workers also bounds RunIndices' fan-out.
    explicit WorkerPool(uint32_t n_workers);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    uint32_t num_workers() const;

    // Run body(i) for i in [0, n_tasks). Task 0 runs on the calling
    // thread (productive use of the caller while the pool runs the rest);
    // tasks 1..n_tasks-1 run on pool workers. Blocks until all complete.
    void RunIndices(uint32_t n_tasks,
                    const std::function<void(uint32_t)>& body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cairns
