// util/task_guard.hpp
//
// RAII guard for a unit of work running on a worker thread that touches the
// graphics backend (Taskflow worker, render thread, future upload/compute
// task -- anything that isn't the main thread). Construct one at the top of
// the task body; the destructor drains whatever per-task cleanup the backend
// needs.
//
// Today this just wraps Metal's NS::AutoreleasePool requirement (worker
// threads have no implicit pool; without one, autoreleased Cocoa/Metal
// objects accumulate per task and leak/crash). Non-Metal platforms see a
// trivial empty type.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL
#include <Foundation/Foundation.hpp>
#endif

namespace cairns {

class TaskGuard {
public:
    TaskGuard(const TaskGuard&) = delete;
    TaskGuard& operator=(const TaskGuard&) = delete;
    TaskGuard(TaskGuard&&) = delete;
    TaskGuard& operator=(TaskGuard&&) = delete;

#if CAIRNS_METAL
    TaskGuard() : pool_(NS::AutoreleasePool::alloc()->init()) {}
    ~TaskGuard() { pool_->release(); }
private:
    NS::AutoreleasePool* pool_;
#else
    TaskGuard() = default;
    ~TaskGuard() = default;
#endif
};

}  // namespace cairns
