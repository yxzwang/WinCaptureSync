#pragma once

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "capture/capture_source.h"
#include "capture/wgc_frame_grabber.h"
#include "encode/mf_h264_encoder.h"
#include "time/time_utils.h"

namespace wcs::capture {

class ScreenRecorder {
public:
    struct Options {
        uint32_t fps = 60;
        uint32_t bitrate = 12000000;
        uint32_t width = 0;
        uint32_t height = 0;
        wcs::encode::MfH264Encoder::VideoCodec codec = wcs::encode::MfH264Encoder::VideoCodec::H264;
        CaptureSource source{};
        std::vector<CaptureSource> sources{};
    };

    ScreenRecorder() = default;
    ~ScreenRecorder();

    bool Start(const std::filesystem::path& video_path,
               const std::filesystem::path& video_meta_path,
               const wcs::time::UtcAnchor& utc_anchor,
               const Options& options = Options{});
    void Stop();

    bool IsRunning() const { return running_.load(); }
    int64_t StartQpc() const { return start_qpc_.load(); }
    uint64_t FrameCount() const { return frame_count_.load(); }
    uint64_t DroppedFrames() const { return dropped_frames_.load(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    void SetInputStartQpc(int64_t input_start_qpc) { input_start_qpc_ = input_start_qpc; }

private:
    bool InitGdi();
    void ReleaseGdi();
    void CaptureThreadMain();
    void WriteVideoMeta() const;

    std::filesystem::path video_path_{};
    std::filesystem::path video_meta_path_{};
    wcs::time::UtcAnchor anchor_{};
    Options options_{};
    wcs::encode::MfH264Encoder encoder_{};

    std::thread capture_thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<int64_t> start_qpc_{0};
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    int64_t input_start_qpc_ = 0;

    HDC screen_dc_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP dib_bitmap_ = nullptr;
    void* dib_bits_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t stride_ = 0;
    bool use_wgc_ = false;
    std::unique_ptr<WgcFrameGrabber> wgc_grabber_{};
};

}  // namespace wcs::capture
