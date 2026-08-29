#pragma once

struct ImDrawData;

namespace cairns {

// Deep-copy an ImDrawData so the render thread can read it after the game
// thread has moved on to ImGui::NewFrame() for frame N+1 (which would
// otherwise reset the source buffers). Copies only the fields read by our
// DrawImGui consumer: DisplayPos / DisplaySize / FramebufferScale and per
// ImDrawList CmdBuffer/VtxBuffer/IdxBuffer contents. The returned snapshot
// owns heap memory; pass it to FreeImGuiSnapshot when done.
ImDrawData* CloneImGuiDrawData(const ImDrawData* src);
void FreeImGuiSnapshot(ImDrawData* snap);

}  // namespace cairns
