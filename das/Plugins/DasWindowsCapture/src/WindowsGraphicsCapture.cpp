#include "WindowsGraphicsCapture.h"
#include <das/DasApi.h>
#include <das/IDasBase.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>
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
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL feature_level{};
    UINT              flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    auto hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        0,
        flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        d3d_device_.put(),
        &feature_level,
        nullptr);

    if (FAILED(hr))
    {
        DAS_LOG_ERROR("Failed to create D3D11 device");
        return DAS_E_CAPTURE_FAILED;
    }

    return DAS_S_OK;
}

DasResult WindowsGraphicsCapture::CreateCaptureItem(HWND hwnd)
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        winrt::com_ptr<IGraphicsCaptureItemInterop>            interop;
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};

        auto hr = interop->CreateForWindow(
            hwnd,
            winrt::guid_of<
                winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(item));
        if (FAILED(hr))
        {
            DAS_LOG_ERROR("Failed to create capture item for window");
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
        (void)ex;
        DAS_LOG_ERROR("WinRT error creating capture item");
        return DAS_E_CAPTURE_FAILED;
    }
    catch (const std::exception& ex)
    {
        (void)ex;
        DAS_LOG_ERROR("Exception creating capture item");
        return DAS_E_CAPTURE_FAILED;
    }
}

DasResult WindowsGraphicsCapture::StartCaptureSession()
{
    try
    {
        auto                        d3d_device = d3d_device_.get();
        winrt::com_ptr<IDXGIDevice> dxgi_device;
        d3d_device->QueryInterface(dxgi_device.put());

        winrt::com_ptr<IInspectable> inspectable_device;
        auto                         hr = CreateDirect3D11DeviceFromDXGIDevice(
            dxgi_device.get(),
            inspectable_device.put());

        if (FAILED(hr))
        {
            DAS_LOG_ERROR("Failed to create Direct3D device");
            return DAS_E_CAPTURE_FAILED;
        }

        auto device = inspectable_device.as<
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        frame_pool_ = winrt::Windows::Graphics::Capture::
            Direct3D11CaptureFramePool::Create(
                device,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::
                    B8G8R8A8UIntNormalized,
                2,
                {width_, height_});

        session_ = frame_pool_.CreateCaptureSession(capture_item_);

        session_.StartCapture();

        return DAS_S_OK;
    }
    catch (const winrt::hresult_error& ex)
    {
        (void)ex;
        DAS_LOG_ERROR("WinRT error starting capture session");
        return DAS_E_CAPTURE_FAILED;
    }
    catch (const std::exception& ex)
    {
        (void)ex;
        DAS_LOG_ERROR("Exception starting capture session");
        return DAS_E_CAPTURE_FAILED;
    }
}

DasResult WindowsGraphicsCapture::WaitForFrame() { return DAS_S_OK; }

DasResult WindowsGraphicsCapture::Initialize(HWND hwnd)
{
    if (hwnd == nullptr)
    {
        DAS_LOG_ERROR("Invalid HWND for Windows.Graphics.Capture");
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
    (void)pp_data; // Unused parameter

    if (!initialized_)
    {
        DAS_LOG_ERROR("WindowsGraphicsCapture not initialized");
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
