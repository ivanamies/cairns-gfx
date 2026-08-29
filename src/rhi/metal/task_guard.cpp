// rhi/metal/task_guard.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/task_guard.hpp"

#include <Foundation/Foundation.hpp>

namespace cairns::rhi {

TaskGuard::TaskGuard() {
    impl_ = NS::AutoreleasePool::alloc()->init();
}

TaskGuard::~TaskGuard() {
    if (impl_ != nullptr) {
        static_cast<NS::AutoreleasePool*>(impl_)->release();
        impl_ = nullptr;
    }
}

}  // namespace cairns::rhi

#endif
