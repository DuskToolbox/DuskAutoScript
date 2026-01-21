#ifndef DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWS_CAPTURE_IMPL_H
#define DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWS_CAPTURE_IMPL_H

#include <DAS/_autogen/idl/abi/IDasCapture.h>
#include <DAS/_autogen/idl/wrapper/Das.PluginInterface.IDasCapture.Implements.hpp>
#include <cstdint>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>

DAS_NS_BEGIN

// {383C85E9-BD34-8D85-AF24-6FE96FE9B} GUID 定义将在 WindowsCaptureImpl.cpp
// 中实现
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    WindowsCapture,
    0xeac73fd2,
    0x5674,
    0x4796,
    0x8298,
    0x82,
    0xb8,
    0xe7 0xfb0,
    0x91,
    0x82,
    0x85,
    0x98 0x97,
    0x9c,
    0xda,
    0x8b,
    0x9f 0x94,
    0x82,
    0x9b);

enum CaptureMode
{
    GDI,
    WindowsGraphicsCapture
};

class WindowsCapture final
    : public PluginInterface::DasCaptureImplBase<WindowsCapture>
{
private:
    // 配置存储
    CaptureMode mode_;
    std::string capture_mode_;
    std::string target_param_; // 存储
                               // window_title/window_handle/process_name/process_id/monitor_index
                               // 之一
    std::string window_title_;
    HWND        target_window_handle_;
    std::string process_name_;
    uint32_t    target_process_id_;
    uint32_t    target_monitor_index_;

    // Windows.Graphics.Capture 成员
    winrt::com_ptr<winrt::Windows::Graphics::Capture::CaptureItem>
        capture_item_;
    winrt::com_ptr<
        winrt::Windows::Graphics::Direct3D11::Direct3D11CaptureFramePool>
        frame_pool_;
    winrt::com_ptr<winrt::Windows::Graphics::Direct3D11::GraphicsCaptureSession>
                                 session_;
    winrt::com_ptr<ID3D11Device> d3d11_device_;

    // BitBlt 成员
    HDC     hdc_screen_;
    HDC     hdc_memory_;
    HBITMAP h_bitmap_;

    // 目标窗口矩形（GDI 方案）
    RECT target_rect_;

    // 指向所选捕获方法
    DasResult (WindowsCapture::*pInitializeGDICapture)();
    DasResult (WindowsCapture::*pInitializeGraphicsCapture)();

public:
    WindowsCapture() = default;
    virtual ~WindowsCapture() = default;

    // IDasCapture 接口
    DAS_IMPL DasResult Capture([out] IDasImage** pp_out_image) override;

    // IDasCaptureFactory 接口
    DAS_IMPL DasResult StartCapture() override;
    DAS_IMPL DasResult StopCapture() override;

private:
    // 解析配置并选择捕获模式
    bool ParseConfigAndSelectMode(const nlohmann::json& config);

    // 初始化选择的捕获方法
    DasResult InitializeGDICapture();
    DasResult InitializeGraphicsCapture();

    // 清理所有资源
    void CleanupGDICapture();
    void CleanupGraphicsCapture();
    void CleanupAll();
};

DAS_NS_END

#endif // DAS_PLUGINS_DASWINDOWSCAPTURE_WINDOWS_CAPTURE_IMPL_H
