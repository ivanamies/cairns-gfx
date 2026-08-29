// rhi/frame_capture.hpp
//
// #222 Phase F.2: one-shot swap-image dump path. Extracted from Frames
// so the per-frame loop doesn't drag tools-grade stb_image_write into
// the hot path. Holds the pending dump request; the actual dump
// (vkCmdCopyImage / MTL blit + stbi_write_png) still lives in
// Frames::EndSubmit because it needs the backend's swap image handle.
// Frames reads the path via Rhi::frame_capture (Init-time pointer).

#pragma once

#include <filesystem>

namespace cairns::rhi {

class FrameCapture {
public:
    FrameCapture() = default;
    ~FrameCapture() = default;
    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    void SetDumpPath(const std::filesystem::path& path) { dump_path_ = path; }
    bool Pending() const { return !dump_path_.empty(); }
    const std::filesystem::path& Path() const { return dump_path_; }
    void Clear() { dump_path_.clear(); }

private:
    std::filesystem::path dump_path_;
};

}  // namespace cairns::rhi
