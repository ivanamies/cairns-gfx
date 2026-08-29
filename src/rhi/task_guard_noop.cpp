// rhi/task_guard_noop.cpp
//
// No-op TaskGuard for non-Metal builds. The Metal impl lives in
// src/rhi/metal/task_guard.cpp.

#include "util/define.hpp"

#if !CAIRNS_METAL

#include "rhi/task_guard.hpp"

namespace cairns::rhi {

TaskGuard::TaskGuard() = default;
TaskGuard::~TaskGuard() = default;

}  // namespace cairns::rhi

#endif
