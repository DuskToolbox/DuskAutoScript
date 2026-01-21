#include "WindowsGraphicsCapture.h"
#include <DAS/_autogen/idl/abi/DasLogger.h>
#include <das/IDasBase.h>
#include <das/IDasBase.h>
#include <das/Logger/Logger.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

WindowsGraphicsCapture::~WindowsGraphicsCapture() { Cleanup(); }

DasResult WindowsGraphicsCapture::CreateD3DDevice()
{
    D3D_FEATURE_LEVEL        feature_level{};
    D3D11_CREATE_DEVICE_FLAG flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    auto hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        0,
        &feature_level,
        flags,
        0,
        d3d_device_.put());

    if (FAILED(hr))
    {
        DAS_CORE_LOG_ERROR("Failed to create D3D11 device: 0x{:08X}", hr);
        return DAS_E_CAPTURE_FAILED;
    }

    return DAS_S_OK;
}

DasResult WindowsGraphicsCapture::CreateCaptureItem(HWND hwnd)
{
    try
    {
        winrt::init_apartment(apartment_type::multi_threaded);

        auto interop = capture_item_.as<IGraphicsCaptureItemInterop>();
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};

        auto hr = interop->CreateForWindow(hwnd, &item);
        if (FAILED(hr))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to create capture item for window: 0x{:08X}",
                hr);
            return DAS_E_CAPTURE_FAILED;
        }

        capture_item_ = item;
        auto size = item.Size();
        width_ = static_cast<int32_t>(size.Width);
        height_ = static_cast<int32_t>(size.Height);
        hwnd_ = hwnd;

        return DAS_S_OK;
    }
    catch (const winrt::hresult_error& ex)
    {
        DAS_CORE_LOG_ERROR(
            "WinRT error creating capture item: 0x{:08X}",
            ex.code());
        return DAS_E_CAPTURE_FAILED;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR("Exception creating capture item: {}", ex.what());
        return DAS_E_CAPTURE_FAILED;
    }
}

DasResult WindowsGraphicsCapture::StartCaptureSession()
{
    try
    {
        auto                           device = d3d_device_.get();
        winrt::com_ptr<IDXGISwapChain> swapchain;

        frame_pool_ = winrt::Windows::Graphics::Direct3D11::
            Direct3D11CaptureFramePool::Create(
                capture_item_,
                static_cast<float>(width_),
                static_cast<float>(height_),
                static_cast<float>(
                    winrt::Windows::Graphics::Direct3D11::Direct3DPixelFormat::
                        B8G8R8A8UIntNormalized),
                2);

        session_ = frame_pool_.CreateSession(capture_item_);

        session_.StartCapture();

        return DAS_S_OK;
    }
    catch (const winrt::hresult_error& ex)
    {
        DAS_CORE_LOG_ERROR(
            "WinRT error starting capture session: 0x{:08X}",
            ex.code());
        return DAS_E_CAPTURE_FAILED;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR("Exception starting capture session: {}", ex.what());
        return DAS_E_CAPTURE_FAILED;
    }
}

DasResult WindowsGraphicsCapture::WaitForFrame() { return DAS_S_OK; }

DasResult WindowsGraphicsCapture::Initialize(HWND hwnd)
{
    if (hwnd == nullptr)
    {
        DAS_CORE_LOG_ERROR("Invalid HWND for Windows.Graphics.Capture");
        return DAS_E_INVALID_ARGUMENT;
    }

    auto hr = CreateD3DDevice();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateCaptureItem(hwnd);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = StartCaptureSession();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = WaitForFrame();
    if (FAILED(hr))
    {
        return hr;
    }

    initialized_ = true;
    return DAS_S_OK;
}

DasResult WindowsGraphicsCapture::Capture(
    uint8_t** pp_data,
    int32_t*  p_width,
    int32_t*  p_height)
{
    if (!initialized_)
    {
        DAS_CORE_LOG_ERROR("WindowsGraphicsCapture not initialized");
        return DAS_E_CAPTURE_FAILED;
    }

    *p_width = width_;
    *p_height = height_;

    return DAS_E_CAPTURE_FAILED;
}

void WindowsGraphicsCapture::Cleanup()
{
    if (!initialized_)
    {
        return;
    }

    session_ = nullptr;
    frame_pool_ = nullptr;
    capture_item_ = nullptr;
    texture_ = nullptr;
    d3d_device_ = nullptr;

    if (frame_data_ != nullptr)
    {
        delete[] frame_data_;
        frame_data_ = nullptr;
    }

    data_size_ = 0;
    initialized_ = false;
}
