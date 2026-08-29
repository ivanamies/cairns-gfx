// rhi/frame_capture.hpp
//
// One-shot swap-image dump request. Holds only the pending path; the
// actual dump (vkCmdCopyImage / MTL blit + stbi_write_png) lives in
// Frames::EndSubmit / Present because it needs the backend's swap image
// handle. Passed to those calls per-frame; kept out of Frames so the
// per-frame loop doesn't drag stb_image_write into the hot path.

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
