#pragma once

#include <Windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace wcs::encode {

class MfH264Encoder {
public:
    enum class VideoCodec {
        H264,
        HEVC
    };

    struct Options {
        uint32_t width = 1920;
        uint32_t height = 1080;
        uint32_t fps = 60;
        uint32_t bitrate = 12000000;
        VideoCodec codec = VideoCodec::H264;
    };

    MfH264Encoder() = default;
    ~MfH264Encoder();

    bool Start(const std::filesystem::path& output_path, const Options& options);
    bool WriteFrame(const uint8_t* bgra,
                    uint32_t stride_bytes,
                    int64_t sample_time_100ns,
                    int64_t sample_duration_100ns);
    void Stop();

    uint64_t WrittenFrames() const { return written_frames_.load(); }
    uint32_t Fps() const { return options_.fps; }
    VideoCodec ActiveCodec() const { return active_codec_; }

private:
    bool started_ = false;
    bool mf_started_ = false;
    Options options_{};
    VideoCodec active_codec_ = VideoCodec::H264;
    DWORD stream_index_ = 0;
    Microsoft::WRL::ComPtr<IMFSinkWriter> sink_writer_{};
    std::atomic<uint64_t> written_frames_{0};
};

}  // namespace wcs::encode
