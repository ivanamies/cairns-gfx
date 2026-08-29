#pragma once

// This file used to host an `Arena` whose enabled path keyed allocations
// through a std::map<void*, OffsetAllocator::Allocation> (the author flagged it
// "stupid") and whose default path was just malloc/free. Both are retired per
// ALLOCATOR_HANDOFF.md -- the substance now lives in `chunk_allocator.hpp`
// (allocator B). The aliases below keep existing call sites
// (cairns::Allocator<T>(hot_arena_) / std::vector<T, cairns::Allocator<T>>)
// compiling unchanged; they now route through B.

#include "util/chunk_allocator.hpp"

namespace cairns {

using Arena = ChunkAllocator;

template <typename T>
using Allocator = ChunkStdAllocator<T>;

}  // namespace cairns
