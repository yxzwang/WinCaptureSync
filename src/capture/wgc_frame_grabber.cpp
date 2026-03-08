#include "capture/wgc_frame_grabber.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cstring>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>

namespace wcs::capture {

namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

bool ResolveCaptureHandle(const CaptureSource& source, HWND* hwnd, HMONITOR* monitor) {
    if (hwnd != nullptr) {
        *hwnd = nullptr;
    }
    if (monitor != nullptr) {
        *monitor = nullptr;
    }

    if (source.type == CaptureSourceType::Window) {
        if (source.window == nullptr || !IsWindow(source.window)) {
            return false;
        }
        if (hwnd != nullptr) {
            *hwnd = source.window;
        }
        return true;
    }

    if (source.type == CaptureSourceType::PrimaryMonitor || source.type == CaptureSourceType::Monitor) {
        HMONITOR resolved = source.monitor;
        if (resolved == nullptr) {
            if (source.type == CaptureSourceType::PrimaryMonitor) {
                const POINT origin{};
                resolved = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
            } else if (source.monitor_rect.right > source.monitor_rect.left &&
                       source.monitor_rect.bottom > source.monitor_rect.top) {
                const POINT center = {(source.monitor_rect.left + source.monitor_rect.right) / 2,
                                      (source.monitor_rect.top + source.monitor_rect.bottom) / 2};
                resolved = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
            }
        }
        if (resolved == nullptr) {
            return false;
        }
        if (monitor != nullptr) {
            *monitor = resolved;
        }
        return true;
    }

    return false;
}

}  // namespace

WgcFrameGrabber::~WgcFrameGrabber() {
    Stop();
}

bool WgcFrameGrabber::Start(const CaptureSource& source,
                            const uint32_t expected_width,
                            const uint32_t expected_height) {
    Stop();

    expected_width_ = expected_width;
    expected_height_ = expected_height;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        // Apartment may already be initialized in another mode.
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   d3d_device_.put(), &created, d3d_context_.put());
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                               d3d_device_.put(), &created, d3d_context_.put());
        if (FAILED(hr)) {
            return false;
        }
    }

    const winrt::com_ptr<IDXGIDevice> dxgi_device = d3d_device_.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable_device;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable_device.put()))) {
        Stop();
        return false;
    }
    winrt_device_ =
        inspectable_device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    if (!CreateCaptureItem(source)) {
        Stop();
        return false;
    }

    frame_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (frame_event_ == nullptr) {
        Stop();
        return false;
    }

    const auto size = item_.Size();
    frame_pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrt_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    session_ = frame_pool_.CreateCaptureSession(item_);

    try {
        session_.IsCursorCaptureEnabled(true);
    } catch (...) {
    }
    try {
        session_.IsBorderRequired(false);
    } catch (...) {
    }

    frame_arrived_token_ = frame_pool_.FrameArrived([this](const auto&, const auto&) {
        if (frame_event_ != nullptr) {
            SetEvent(frame_event_);
        }
    });

    session_.StartCapture();
    running_ = true;
    return true;
}

void WgcFrameGrabber::Stop() {
    running_ = false;

    if (frame_pool_ != nullptr) {
        try {
            frame_pool_.FrameArrived(frame_arrived_token_);
        } catch (...) {
        }
    }

    if (session_ != nullptr) {
        try {
            session_.Close();
        } catch (...) {
        }
        session_ = nullptr;
    }

    if (frame_pool_ != nullptr) {
        try {
            frame_pool_.Close();
        } catch (...) {
        }
        frame_pool_ = nullptr;
    }

    item_ = nullptr;
    winrt_device_ = nullptr;
    staging_texture_ = nullptr;
    d3d_context_ = nullptr;
    d3d_device_ = nullptr;

    if (frame_event_ != nullptr) {
        CloseHandle(frame_event_);
        frame_event_ = nullptr;
    }
}

bool WgcFrameGrabber::CreateCaptureItem(const CaptureSource& source) {
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;
    if (!ResolveCaptureHandle(source, &hwnd, &monitor)) {
        return false;
    }

    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (interop == nullptr) {
        return false;
    }

    if (hwnd != nullptr) {
        return SUCCEEDED(
            interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_)));
    }
    if (monitor != nullptr) {
        return SUCCEEDED(interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                                   winrt::put_abi(item_)));
    }
    return false;
}

bool WgcFrameGrabber::EnsureStagingTexture(const uint32_t width, const uint32_t height) {
    if (staging_texture_ != nullptr) {
        D3D11_TEXTURE2D_DESC existing{};
        staging_texture_->GetDesc(&existing);
        if (existing.Width == width && existing.Height == height) {
            return true;
        }
        staging_texture_ = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    return SUCCEEDED(d3d_device_->CreateTexture2D(&desc, nullptr, staging_texture_.put()));
}

bool WgcFrameGrabber::CopyLatestFrame(uint8_t* dst_bgra,
                                      const uint32_t dst_stride,
                                      const uint32_t dst_width,
                                      const uint32_t dst_height,
                                      const int wait_timeout_ms) {
    if (!running_ || frame_pool_ == nullptr || d3d_context_ == nullptr || dst_bgra == nullptr) {
        return false;
    }

    if (wait_timeout_ms > 0 && frame_event_ != nullptr) {
        const DWORD wait = WaitForSingleObject(frame_event_, static_cast<DWORD>(wait_timeout_ms));
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            return false;
        }
    }

    Direct3D11CaptureFrame frame{nullptr};
    try {
        frame = frame_pool_.TryGetNextFrame();
    } catch (...) {
        return false;
    }
    if (frame == nullptr) {
        return false;
    }

    const auto size = frame.ContentSize();
    if (size.Width <= 0 || size.Height <= 0) {
        return false;
    }

    const uint32_t src_width = static_cast<uint32_t>(size.Width);
    const uint32_t src_height = static_cast<uint32_t>(size.Height);

    if (dst_width == 0 || dst_height == 0) {
        return false;
    }

    using DxgiInterfaceAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
    const auto access = frame.Surface().as<DxgiInterfaceAccess>();
    if (access == nullptr) {
        return false;
    }

    winrt::com_ptr<ID3D11Texture2D> src_texture;
    if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), src_texture.put_void()))) {
        return false;
    }

    if (!EnsureStagingTexture(src_width, src_height)) {
        return false;
    }

    d3d_context_->CopyResource(staging_texture_.get(), src_texture.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d_context_->Map(staging_texture_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const auto* src_base = static_cast<const uint8_t*>(mapped.pData);
    if (src_width == dst_width && src_height == dst_height) {
        const uint32_t row_bytes = src_width * 4;
        for (uint32_t y = 0; y < src_height; ++y) {
            const auto* src_row = src_base + static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(y);
            auto* dst_row = dst_bgra + static_cast<size_t>(dst_stride) * static_cast<size_t>(y);
            std::memcpy(dst_row, src_row, row_bytes);
        }
    } else {
        // Scale during capture so the encoder only sees the target resolution.
        for (uint32_t y = 0; y < dst_height; ++y) {
            const uint32_t src_y =
                static_cast<uint32_t>((static_cast<uint64_t>(y) * src_height) / dst_height);
            const auto* src_row =
                src_base + static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(src_y);
            auto* dst_row = dst_bgra + static_cast<size_t>(dst_stride) * static_cast<size_t>(y);
            for (uint32_t x = 0; x < dst_width; ++x) {
                const uint32_t src_x =
                    static_cast<uint32_t>((static_cast<uint64_t>(x) * src_width) / dst_width);
                const auto* src_px = src_row + static_cast<size_t>(src_x) * 4;
                auto* dst_px = dst_row + static_cast<size_t>(x) * 4;
                std::memcpy(dst_px, src_px, 4);
            }
        }
    }

    d3d_context_->Unmap(staging_texture_.get(), 0);
    return true;
}

}  // namespace wcs::capture
