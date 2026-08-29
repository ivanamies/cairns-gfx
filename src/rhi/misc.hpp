#pragma once

#include "rhi/gfx_api.hpp"
#include "util/define.hpp"

#include <cassert>

namespace cairns::rhi {

template<typename MetalResourcePtr>
void setLabel(MetalResourcePtr ptr, std::string_view label) {
    NS::String* labelScuffed = NS::String::string(label.data(), NS::StringEncoding::UTF8StringEncoding );
    ptr->setLabel(labelScuffed);
    labelScuffed->release();
}

} // namespace cairns::rhi
