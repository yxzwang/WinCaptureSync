#include "encode/mf_h264_encoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <cstring>
#include <new>

namespace wcs::encode {

namespace {

constexpr int64_t kHundredNsPerSecond = 10000000LL;

bool HrOk(const HRESULT hr) {
    return SUCCEEDED(hr);
}

GUID CodecSubtype(const MfH264Encoder::VideoCodec codec) {
    return codec == MfH264Encoder::VideoCodec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

}  // namespace

MfH264Encoder::~MfH264Encoder() {
    Stop();
}

bool MfH264Encoder::Start(const std::filesystem::path& output_path, const Options& options) {
    if (started_) {
        return false;
    }
    options_ = options;
    active_codec_ = options.codec;
    written_frames_.store(0);

    if (HrOk(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        mf_started_ = true;
    } else {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (!HrOk(MFCreateAttributes(attributes.GetAddressOf(), 2))) {
        Stop();
        return false;
    }
    // Prefer stability over vendor-specific hardware color-conversion quirks.
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

    const std::wstring output_w = output_path.wstring();
    if (!HrOk(MFCreateSinkWriterFromURL(output_w.c_str(), nullptr, attributes.Get(),
                                        sink_writer_.GetAddressOf()))) {
        Stop();
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> output_type;
    if (!HrOk(MFCreateMediaType(output_type.GetAddressOf()))) {
        Stop();
        return false;
    }
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, CodecSubtype(active_codec_));
    output_type->SetUINT32(MF_MT_AVG_BITRATE, options_.bitrate);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, options_.width, options_.height);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, options_.fps, 1);
    MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (!HrOk(sink_writer_->AddStream(output_type.Get(), &stream_index_))) {
        // Some systems do not have HEVC encoder support installed.
        if (active_codec_ == VideoCodec::HEVC) {
            active_codec_ = VideoCodec::H264;
            output_type->SetGUID(MF_MT_SUBTYPE, CodecSubtype(active_codec_));
            if (!HrOk(sink_writer_->AddStream(output_type.Get(), &stream_index_))) {
                Stop();
                return false;
            }
        } else {
            Stop();
            return false;
        }
    }

    Microsoft::WRL::ComPtr<IMFMediaType> input_type;
    if (!HrOk(MFCreateMediaType(input_type.GetAddressOf()))) {
        Stop();
        return false;
    }
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, options_.width, options_.height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, options_.fps, 1);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, options_.width * 4);
    input_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    input_type->SetUINT32(MF_MT_SAMPLE_SIZE, options_.width * options_.height * 4);

    if (!HrOk(sink_writer_->SetInputMediaType(stream_index_, input_type.Get(), nullptr))) {
        Stop();
        return false;
    }

    if (!HrOk(sink_writer_->BeginWriting())) {
        Stop();
        return false;
    }

    started_ = true;
    return true;
}

bool MfH264Encoder::WriteFrame(const uint8_t* bgra,
                               const uint32_t stride_bytes,
                               const int64_t sample_time_100ns,
                               const int64_t sample_duration_100ns) {
    if (!started_ || bgra == nullptr || sink_writer_ == nullptr) {
        return false;
    }

    const uint32_t frame_bytes = stride_bytes * options_.height;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> media_buffer;
    if (!HrOk(MFCreateMemoryBuffer(frame_bytes, media_buffer.GetAddressOf()))) {
        return false;
    }

    BYTE* dst = nullptr;
    DWORD max_len = 0;
    if (!HrOk(media_buffer->Lock(&dst, &max_len, nullptr))) {
        return false;
    }

    if (max_len < frame_bytes) {
        media_buffer->Unlock();
        return false;
    }

    memcpy(dst, bgra, frame_bytes);
    // Some window capture paths produce undefined alpha; force opaque to avoid
    // downstream color-conversion artifacts (green frames on certain systems).
    for (uint32_t y = 0; y < options_.height; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * stride_bytes;
        for (uint32_t x = 0; x < options_.width; ++x) {
            row[static_cast<size_t>(x) * 4 + 3] = 0xFF;
        }
    }
    media_buffer->Unlock();
    media_buffer->SetCurrentLength(frame_bytes);

    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (!HrOk(MFCreateSample(sample.GetAddressOf()))) {
        return false;
    }
    if (!HrOk(sample->AddBuffer(media_buffer.Get()))) {
        return false;
    }
    sample->SetSampleTime(sample_time_100ns);
    sample->SetSampleDuration(sample_duration_100ns > 0 ? sample_duration_100ns
                                                         : (kHundredNsPerSecond / options_.fps));

    if (!HrOk(sink_writer_->WriteSample(stream_index_, sample.Get()))) {
        return false;
    }

    written_frames_.fetch_add(1);
    return true;
}

void MfH264Encoder::Stop() {
    if (sink_writer_ != nullptr) {
        sink_writer_->Finalize();
    }
    sink_writer_.Reset();
    started_ = false;

    if (mf_started_) {
        MFShutdown();
        mf_started_ = false;
    }
}

}  // namespace wcs::encode
