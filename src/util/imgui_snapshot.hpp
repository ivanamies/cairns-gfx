// src/util/imgui_snapshot.hpp
//
// Deep-copy ImDrawData into game-thread-owned storage so the render thread
// can read a stable frame while the game thread builds the next one.
// Internals use IM_NEW/IM_DELETE (imgui's allocator); the ImDrawData stored
// here is a valid input to cmd.DrawImGui(...). One snapshot per slot.

#pragma once

#include "imgui.h"

#include <vector>

namespace cairns {

struct ImDrawDataSnapshot {
    ImDrawData data{};               // header + CmdLists view consumed by DrawImGui
    std::vector<ImDrawList*> owned;  // for IM_DELETE on reset

    ImDrawDataSnapshot() = default;
    ~ImDrawDataSnapshot() { Clear(); }

    ImDrawDataSnapshot(const ImDrawDataSnapshot&) = delete;
    ImDrawDataSnapshot& operator=(const ImDrawDataSnapshot&) = delete;

    // Move-only. data.CmdLists is an ImVector (move-able). owned is move-able.
    // Destructor handles empty owned (moved-from is just empty).
    ImDrawDataSnapshot(ImDrawDataSnapshot&& o) noexcept
        : data(o.data), owned(std::move(o.owned)) {
        o.data = {};
    }
    ImDrawDataSnapshot& operator=(ImDrawDataSnapshot&& o) noexcept {
        if (this != &o) {
            Clear();
            data = o.data;
            owned = std::move(o.owned);
            o.data = {};
        }
        return *this;
    }

    void Clear();
};

// Deep-copy src into dst, replacing whatever dst held. Safe to call with
// src == nullptr (resets dst to empty). After this returns, &dst.data is a
// valid ImDrawData* for cmd.DrawImGui(...) and remains valid until the next
// SnapshotImDrawData or Clear on this dst.
void SnapshotImDrawData(const ImDrawData* src, ImDrawDataSnapshot& dst);

}  // namespace cairns
