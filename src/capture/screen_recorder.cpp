#include "capture/screen_recorder.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <thread>

#include "common/logger.h"

namespace wcs::capture {

namespace {

constexpr int64_t kHundredNsPerSecond = 10000000LL;

uint32_t MakeEvenDimension(const uint32_t value) {
    if (value <= 1) {
        return value;
    }
    return (value % 2 == 0) ? value : (value - 1);
}

const char* SourceTypeJsonName(const CaptureSourceType type) {
    switch (type) {
        case CaptureSourceType::PrimaryMonitor:
            return "primary_monitor";
        case CaptureSourceType::Monitor:
            return "monitor";
        case CaptureSourceType::Window:
            return "window";
        default:
            return "unknown";
    }
}

const char* CodecJsonName(const wcs::encode::MfH264Encoder::VideoCodec codec) {
    return codec == wcs::encode::MfH264Encoder::VideoCodec::HEVC ? "hevc" : "h264";
}

}  // namespace

ScreenRecorder::~ScreenRecorder() {
    Stop();
}

bool ScreenRecorder::Start(const std::filesystem::path& video_path,
                           const std::filesystem::path& video_meta_path,
                           const wcs::time::UtcAnchor& utc_anchor,
                           const Options& options) {
    if (running_.load()) {
        return false;
    }
    try {
        wcs::common::log::Info("ScreenRecorder Start");

        video_path_ = video_path;
        video_meta_path_ = video_meta_path;
        anchor_ = utc_anchor;
        options_ = options;
        if (options_.sources.empty()) {
            options_.sources.push_back(options_.source);
        }
        frame_count_.store(0);
        dropped_frames_.store(0);
        start_qpc_.store(0);
        stop_requested_.store(false);

        uint32_t source_width = 0;
        uint32_t source_height = 0;
        if (!ResolveCompositeSize(options_.sources, &source_width, &source_height)) {
            wcs::common::log::Error("ScreenRecorder ResolveCompositeSize failed");
            return false;
        }

        width_ = options_.width > 0 ? options_.width : source_width;
        height_ = options_.height > 0 ? options_.height : source_height;
        width_ = MakeEvenDimension(width_);
        height_ = MakeEvenDimension(height_);
        if (width_ == 0 || height_ == 0) {
            wcs::common::log::Error("ScreenRecorder invalid output dimension");
            return false;
        }
        stride_ = width_ * 4;

        if (!InitGdi()) {
            wcs::common::log::Error("ScreenRecorder InitGdi failed");
            ReleaseGdi();
            return false;
        }

        use_wgc_ = false;
        wgc_grabber_.reset();
        if (options_.sources.size() == 1) {
            auto grabber = std::make_unique<WgcFrameGrabber>();
            if (grabber->Start(options_.sources.front(), width_, height_)) {
                use_wgc_ = true;
                wgc_grabber_ = std::move(grabber);
            }
        }

        wcs::encode::MfH264Encoder::Options encode_options{};
        encode_options.width = width_;
        encode_options.height = height_;
        encode_options.fps = options_.fps;
        encode_options.bitrate = options_.bitrate;
        encode_options.codec = options_.codec;
        if (!encoder_.Start(video_path_, encode_options)) {
            wcs::common::log::Error("ScreenRecorder encoder start failed");
            ReleaseGdi();
            return false;
        }

        running_.store(true);
        capture_thread_ = std::thread(&ScreenRecorder::CaptureThreadMain, this);
        wcs::common::log::Info("ScreenRecorder started");
        return true;
    } catch (const std::exception& ex) {
        wcs::common::log::Error(std::string("ScreenRecorder Start exception: ") + ex.what());
    } catch (...) {
        wcs::common::log::Error("ScreenRecorder Start unknown exception");
    }
    ReleaseGdi();
    return false;
}

void ScreenRecorder::Stop() {
    if (!running_.load()) {
        return;
    }
    wcs::common::log::Info("ScreenRecorder stopping");

    stop_requested_.store(true);
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    encoder_.Stop();
    if (wgc_grabber_) {
        wgc_grabber_->Stop();
        wgc_grabber_.reset();
    }
    ReleaseGdi();
    WriteVideoMeta();
    use_wgc_ = false;
    running_.store(false);
    wcs::common::log::Info("ScreenRecorder stopped");
}

bool ScreenRecorder::InitGdi() {
    screen_dc_ = GetDC(nullptr);
    if (screen_dc_ == nullptr) {
        return false;
    }

    mem_dc_ = CreateCompatibleDC(screen_dc_);
    if (mem_dc_ == nullptr) {
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width_);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height_);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dib_bitmap_ =
        CreateDIBSection(screen_dc_, &bmi, DIB_RGB_COLORS, &dib_bits_, nullptr, 0);
    if (dib_bitmap_ == nullptr || dib_bits_ == nullptr) {
        return false;
    }

    if (SelectObject(mem_dc_, dib_bitmap_) == nullptr) {
        return false;
    }

    return true;
}

void ScreenRecorder::ReleaseGdi() {
    if (dib_bitmap_ != nullptr) {
        DeleteObject(dib_bitmap_);
        dib_bitmap_ = nullptr;
    }
    dib_bits_ = nullptr;

    if (mem_dc_ != nullptr) {
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (screen_dc_ != nullptr) {
        ReleaseDC(nullptr, screen_dc_);
        screen_dc_ = nullptr;
    }
}

void ScreenRecorder::CaptureThreadMain() {
    try {
        const int64_t qpc_freq = anchor_.qpc_freq;
        const int64_t frame_interval_qpc =
            options_.fps > 0 ? (qpc_freq / static_cast<int64_t>(options_.fps)) : 0;
        const int64_t default_frame_duration_100ns =
            options_.fps > 0 ? (kHundredNsPerSecond / static_cast<int64_t>(options_.fps))
                             : (kHundredNsPerSecond / 60);

        const int64_t local_start_qpc = wcs::time::QpcClock::NowTicks();
        start_qpc_.store(local_start_qpc);
        int64_t previous_sample_time = 0;
        int64_t next_tick = local_start_qpc;
        uint32_t consecutive_capture_failures = 0;

        while (!stop_requested_.load()) {
            bool captured = false;
            if (use_wgc_ && wgc_grabber_) {
                const int wait_timeout_ms =
                    options_.fps > 0 ? (std::max)(1, static_cast<int>(1000 / options_.fps)) : 16;
                captured = wgc_grabber_->CopyLatestFrame(static_cast<uint8_t*>(dib_bits_), stride_,
                                                         width_, height_, wait_timeout_ms);
            } else {
                captured = CaptureSourcesToDc(options_.sources, mem_dc_, static_cast<int>(width_),
                                              static_cast<int>(height_));
            }

            if (!captured) {
                dropped_frames_.fetch_add(1);
                ++consecutive_capture_failures;
                if (consecutive_capture_failures == 120) {
                    wcs::common::log::Warning(
                        "ScreenRecorder frame capture failed 120 consecutive times");
                    consecutive_capture_failures = 0;
                }
                continue;
            }
            consecutive_capture_failures = 0;

            const int64_t frame_qpc = wcs::time::QpcClock::NowTicks();
            const int64_t sample_time_100ns =
                (frame_qpc - local_start_qpc) * kHundredNsPerSecond / qpc_freq;
            int64_t sample_duration_100ns = sample_time_100ns - previous_sample_time;
            if (sample_duration_100ns <= 0) {
                sample_duration_100ns = default_frame_duration_100ns;
            }
            previous_sample_time = sample_time_100ns;

            const bool ok = encoder_.WriteFrame(static_cast<const uint8_t*>(dib_bits_), stride_,
                                                sample_time_100ns, sample_duration_100ns);
            if (ok) {
                frame_count_.fetch_add(1);
            } else {
                dropped_frames_.fetch_add(1);
            }

            if (frame_interval_qpc <= 0) {
                continue;
            }

            next_tick += frame_interval_qpc;
            int64_t now = wcs::time::QpcClock::NowTicks();
            int64_t remaining_ticks = next_tick - now;
            if (remaining_ticks > 0) {
                const int64_t remaining_ms = (remaining_ticks * 1000) / qpc_freq;
                if (remaining_ms > 1) {
                    Sleep(static_cast<DWORD>(remaining_ms - 1));
                }
                while (wcs::time::QpcClock::NowTicks() < next_tick) {
                    std::this_thread::yield();
                }
            } else {
                next_tick = now;
            }
        }
    } catch (const std::exception& ex) {
        wcs::common::log::Error(std::string("ScreenRecorder capture thread exception: ") + ex.what());
        stop_requested_.store(true);
    } catch (...) {
        wcs::common::log::Error("ScreenRecorder capture thread unknown exception");
        stop_requested_.store(true);
    }
}

void ScreenRecorder::WriteVideoMeta() const {
    std::ofstream out(video_meta_path_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }

    const int64_t screen_start_qpc = start_qpc_.load();
    const int64_t screen_start_utc = wcs::time::QpcClock::QpcToUtcEpochNs(screen_start_qpc, anchor_);
    const int64_t input_start_utc =
        input_start_qpc_ > 0 ? wcs::time::QpcClock::QpcToUtcEpochNs(input_start_qpc_, anchor_) : 0;

    const auto video_file_name = video_path_.filename().u8string();

    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"video_file\": \"" << video_file_name << "\",\n";
    out << "  \"capture\": {\n";
    out << "    \"width\": " << width_ << ",\n";
    out << "    \"height\": " << height_ << ",\n";
    out << "    \"fps\": " << options_.fps << ",\n";
    out << "    \"bitrate\": " << options_.bitrate << ",\n";
    out << "    \"backend\": \"" << (use_wgc_ ? "wgc" : "gdi") << "\",\n";
    out << "    \"codec\": \"" << CodecJsonName(encoder_.ActiveCodec()) << "\",\n";
    const CaptureSource default_source{};
    const CaptureSource& primary_source =
        options_.sources.empty() ? default_source : options_.sources.front();
    out << "    \"source_type\": \"" << SourceTypeJsonName(primary_source.type) << "\",\n";
    out << "    \"source_hwnd\": " << reinterpret_cast<uintptr_t>(primary_source.window) << ",\n";
    out << "    \"source_hmonitor\": " << reinterpret_cast<uintptr_t>(primary_source.monitor)
        << ",\n";
    out << "    \"source_rect\": {\"left\": " << primary_source.monitor_rect.left
        << ", \"top\": " << primary_source.monitor_rect.top
        << ", \"right\": " << primary_source.monitor_rect.right
        << ", \"bottom\": " << primary_source.monitor_rect.bottom << "},\n";
    out << "    \"source_count\": " << options_.sources.size() << ",\n";
    out << "    \"sources\": [\n";
    for (size_t i = 0; i < options_.sources.size(); ++i) {
        const auto& src = options_.sources[i];
        out << "      {\"type\": \"" << SourceTypeJsonName(src.type)
            << "\", \"hwnd\": " << reinterpret_cast<uintptr_t>(src.window)
            << ", \"hmonitor\": " << reinterpret_cast<uintptr_t>(src.monitor)
            << ", \"rect\": {\"left\": " << src.monitor_rect.left
            << ", \"top\": " << src.monitor_rect.top << ", \"right\": "
            << src.monitor_rect.right << ", \"bottom\": " << src.monitor_rect.bottom << "}}";
        if (i + 1 != options_.sources.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "    ]\n";
    out << "  },\n";
    out << "  \"screen_start_qpc\": " << screen_start_qpc << ",\n";
    out << "  \"input_start_qpc\": " << input_start_qpc_ << ",\n";
    out << "  \"screen_start_utc_epoch_ns\": " << screen_start_utc << ",\n";
    out << "  \"input_start_utc_epoch_ns\": " << input_start_utc << ",\n";
    out << "  \"qpc_freq\": " << anchor_.qpc_freq << ",\n";
    out << "  \"utc_anchor\": {\n";
    out << "    \"qpc_ticks\": " << anchor_.qpc_ticks << ",\n";
    out << "    \"utc_epoch_ns\": " << anchor_.utc_epoch_ns << "\n";
    out << "  },\n";
    out << "  \"stats\": {\n";
    out << "    \"written_frames\": " << frame_count_.load() << ",\n";
    out << "    \"dropped_frames\": " << dropped_frames_.load() << "\n";
    out << "  }\n";
    out << "}\n";
}

}  // namespace wcs::capture
