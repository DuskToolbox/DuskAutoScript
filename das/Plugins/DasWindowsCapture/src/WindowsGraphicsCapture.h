#ifndef DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSGRAPHICSCAPTURE_H
#define DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSGRAPHICSCAPTURE_H

#include <cstdint>
#include <d3d11.h>
#include <das/IDasBase.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>

class WindowsGraphicsCapture
{
public:
    WindowsGraphicsCapture() = default;
    ~WindowsGraphicsCapture();

    DasResult Initialize(HWND hwnd);
    DasResult Capture(uint8_t** pp_data, int32_t* p_width, int32_t* p_height);
    void      Cleanup();

private:
    DasResult CreateD3DDevice();
    DasResult CreateCaptureItem(HWND hwnd);
    DasResult StartCaptureSession();
    DasResult WaitForFrame();

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item_{
        nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{
        nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::com_ptr<ID3D11Device>                              d3d_device_;
    winrt::com_ptr<ID3D11Texture2D>                           texture_;

    HWND     hwnd_{nullptr};
    int32_t  width_{0};
    int32_t  height_{0};
    bool     initialized_{false};
    uint8_t* frame_data_{nullptr};
    size_t   data_size_{0};
};

#endif // DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWSGRAPHICSCAPTURE_H