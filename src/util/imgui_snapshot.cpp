#include "util/imgui_snapshot.hpp"

#include "imgui.h"

#include <cstring>

namespace cairns {

ImDrawData* CloneImGuiDrawData(const ImDrawData* src) {
    if (src == nullptr) {
        return nullptr;
    }
    ImDrawData* snap = IM_NEW(ImDrawData)();
    snap->Valid = src->Valid;
    snap->CmdListsCount = src->CmdListsCount;
    snap->TotalIdxCount = src->TotalIdxCount;
    snap->TotalVtxCount = src->TotalVtxCount;
    snap->DisplayPos = src->DisplayPos;
    snap->DisplaySize = src->DisplaySize;
    snap->FramebufferScale = src->FramebufferScale;
    snap->OwnerViewport = src->OwnerViewport;
    snap->Textures = src->Textures;

    snap->CmdLists.reserve(src->CmdLists.Size);
    for (int n = 0; n < src->CmdLists.Size; ++n) {
        const ImDrawList* sl = src->CmdLists[n];
        ImDrawList* dl = IM_NEW(ImDrawList)(nullptr);
        dl->Flags = sl->Flags;
        dl->CmdBuffer.resize(sl->CmdBuffer.Size);
        if (sl->CmdBuffer.Size > 0) {
            std::memcpy(dl->CmdBuffer.Data, sl->CmdBuffer.Data,
                        sizeof(ImDrawCmd) * sl->CmdBuffer.Size);
        }
        dl->VtxBuffer.resize(sl->VtxBuffer.Size);
        if (sl->VtxBuffer.Size > 0) {
            std::memcpy(dl->VtxBuffer.Data, sl->VtxBuffer.Data,
                        sizeof(ImDrawVert) * sl->VtxBuffer.Size);
        }
        dl->IdxBuffer.resize(sl->IdxBuffer.Size);
        if (sl->IdxBuffer.Size > 0) {
            std::memcpy(dl->IdxBuffer.Data, sl->IdxBuffer.Data,
                        sizeof(ImDrawIdx) * sl->IdxBuffer.Size);
        }
        snap->CmdLists.push_back(dl);
    }
    return snap;
}

void FreeImGuiSnapshot(ImDrawData* snap) {
    if (snap == nullptr) {
        return;
    }
    for (int n = 0; n < snap->CmdLists.Size; ++n) {
        IM_DELETE(snap->CmdLists[n]);
    }
    IM_DELETE(snap);
}

}  // namespace cairns
