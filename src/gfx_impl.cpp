// gfx_impl.cpp -- one-and-only-one TU that emits the metal-cpp
// implementation symbols. Anything else that includes gfx_api.hpp picks up
// the type declarations alone. Splitting this out avoids duplicate-symbol
// link errors once engine.hpp started being included from more than one TU.

#include "util/define.hpp"

#if CAIRNS_METAL

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#endif  // CAIRNS_METAL
