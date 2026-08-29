// src/util/imgui_snapshot.cpp

#include "util/imgui_snapshot.hpp"

#include "imgui_internal.h"

namespace cairns {

void ImDrawDataSnapshot::Clear() {
    for (ImDrawList* l : owned) {
        IM_DELETE(l);
    }
    owned.clear();
    data.CmdLists.clear();
    data.Valid = false;
    data.CmdListsCount = 0;
    data.TotalIdxCount = 0;
    data.TotalVtxCount = 0;
    data.DisplayPos = ImVec2(0.0f, 0.0f);
    data.DisplaySize = ImVec2(0.0f, 0.0f);
    data.FramebufferScale = ImVec2(1.0f, 1.0f);
}

void SnapshotImDrawData(const ImDrawData* src, ImDrawDataSnapshot& dst) {
    dst.Clear();
    if (!src) {
        return;
    }
    dst.data.Valid = src->Valid;
    dst.data.TotalIdxCount = src->TotalIdxCount;
    dst.data.TotalVtxCount = src->TotalVtxCount;
    dst.data.DisplayPos = src->DisplayPos;
    dst.data.DisplaySize = src->DisplaySize;
    dst.data.FramebufferScale = src->FramebufferScale;

    const int n = src->CmdListsCount;
    dst.owned.reserve(static_cast<size_t>(n));
    dst.data.CmdLists.reserve(n);
    for (int i = 0; i < n; ++i) {
        ImDrawList* clone = src->CmdLists[i]->CloneOutput();
        dst.owned.push_back(clone);
        dst.data.CmdLists.push_back(clone);
    }
    dst.data.CmdListsCount = static_cast<int>(dst.data.CmdLists.size());
}

}  // namespace cairns
