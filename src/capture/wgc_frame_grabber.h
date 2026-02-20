#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>

#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include "capture/capture_source.h"

namespace wcs::capture {

class WgcFrameGrabber {
public:
    WgcFrameGrabber() = default;
    ~WgcFrameGrabber();

    WgcFrameGrabber(const WgcFrameGrabber&) = delete;
    WgcFrameGrabber& operator=(const WgcFrameGrabber&) = delete;

    bool Start(const CaptureSource& source, uint32_t expected_width, uint32_t expected_height);
    void Stop();

    bool IsRunning() const { return running_; }
    bool CopyLatestFrame(uint8_t* dst_bgra, uint32_t dst_stride, uint32_t dst_width, uint32_t dst_height,
                         int wait_timeout_ms);

private:
    bool CreateCaptureItem(const CaptureSource& source);
    bool EnsureStagingTexture(uint32_t width, uint32_t height);

    bool running_ = false;
    uint32_t expected_width_ = 0;
    uint32_t expected_height_ = 0;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    winrt::com_ptr<ID3D11Device> d3d_device_{};
    winrt::com_ptr<ID3D11DeviceContext> d3d_context_{};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::event_token frame_arrived_token_{};
    HANDLE frame_event_ = nullptr;
    winrt::com_ptr<ID3D11Texture2D> staging_texture_{};
};

}  // namespace wcs::capture
