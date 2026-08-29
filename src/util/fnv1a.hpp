// util/fnv1a.hpp
//
// #229 M0b determinism hash: streaming FNV-1a 64. Order-stable byte hashing for
// the per-frame state-hash (sim + render). Cheap, no allocation, no statics.
// Feed POD byte spans only -- never pointers, capacity tails, or padding that
// isn't deterministically initialized (the block is 0xCC-prefilled).

#pragma once

#include <cstddef>
#include <cstdint>

namespace cairns {

struct Fnv1a {
    static constexpr uint64_t kOffset = 1469598103934665603ull;
    static constexpr uint64_t kPrime = 1099511628211ull;

    uint64_t h = kOffset;

    void Write(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            h ^= p[i];
            h *= kPrime;
        }
    }
    template <typename T>
    void WritePod(const T& v) { Write(&v, sizeof(T)); }

    uint64_t Digest() const { return h; }
};

// One-shot convenience.
inline uint64_t Fnv1aHash(const void* data, size_t len) {
    Fnv1a f;
    f.Write(data, len);
    return f.Digest();
}

}  // namespace cairns
