// rhi/task_guard.hpp
//
// RAII guard for a unit of RHI work running on a worker thread (Taskflow
// worker, render thread, future upload/compute task -- anything that isn't
// the main thread but touches the RHI). Construct one at the top of the
// task body; the destructor drains whatever per-task cleanup the backend
// needs.
//
// Today this just wraps Metal's NS::AutoreleasePool requirement (worker
// threads have no implicit pool; without one, autoreleased Cocoa/Metal
// objects accumulate per task and leak/crash). Vulkan/no-op platforms see
// a trivial empty type.
//
// Implementation split:
//   - src/rhi/metal/task_guard.cpp -- real Metal pool impl.
//   - src/rhi/task_guard_noop.cpp  -- empty body on non-Metal builds.
// Header is backend-neutral.

#pragma once

namespace cairns::rhi {

class TaskGuard {
public:
    TaskGuard();
    ~TaskGuard();

    TaskGuard(const TaskGuard&) = delete;
    TaskGuard& operator=(const TaskGuard&) = delete;
    TaskGuard(TaskGuard&&) = delete;
    TaskGuard& operator=(TaskGuard&&) = delete;

private:
    void* impl_ = nullptr;
};

}  // namespace cairns::rhi
