// rhi/metal/pipelines_plat.hpp
//
// Metal side of Pipelines::plat. Selected by rhi/pipelines_plat.hpp at the
// platform-resolver layer; the common header rhi/pipelines.hpp sees only
// PipelinesPlat as a fully-defined member type.

#pragma once

#include <Metal/Metal.hpp>

namespace cairns::rhi {

struct PipelinesPlat {
    MTL::Device* device_ = nullptr;  // mirrored from Device
};

}  // namespace cairns::rhi
