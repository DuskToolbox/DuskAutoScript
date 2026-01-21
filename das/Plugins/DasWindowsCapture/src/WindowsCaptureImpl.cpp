#include <DAS/_autogen/idl/abi/DasLogger.h>
#include <DAS/_autogen/idl/abi/IDasImage.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCapture.Implements.hpp>
#include <nlohmann/json.hpp>

#include "GDICapture.h"
#include "WindowsCaptureImpl.h"
#include "WindowsGraphicsCapture.h"

#include <dwmapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <memory>
#include <stdexcept>
#include <unordered_map>

DAS_NS_BEGIN

bool WindowsCapture::ParseConfigAndSelectMode(const nlohmann::json& config)
{
    if (!config.contains("capture_mode"))
    {
        DAS_CORE_LOG_ERROR("Missing capture_mode in config");
        return false;
    }

    std::string mode_str = config["capture_mode"];
    capture_mode_ = mode_str;

    if (mode_str == "windows_graphics_capture")
    {
        mode_ = CaptureMode::WindowsGraphicsCapture;
        pInitializeGDICapture_ = nullptr;
        pInitializeGraphicsCapture_ =
            &WindowsCapture::InitializeGraphicsCapture;
        DAS_CORE_LOG_INFO("Selected Windows.Graphics.Capture mode");
        return true;
    }
    else if (mode_str == "gdi_bitblt")
    {
        mode_ = CaptureMode::GDI;
        pInitializeGDICapture_ = &WindowsCapture::InitializeGDICapture;
        pInitializeGraphicsCapture_ = nullptr;
        DAS_CORE_LOG_INFO("Selected GDI BitBlt mode");
        return true;
    }
    else
    {
        DAS_CORE_LOG_ERROR(
            "Invalid capture_mode: {}. Expected 'windows_graphics_capture' or 'gdi_bitblt'",
            mode_str);
        return false;
    }
}

DasResult WindowsCapture::InitializeGDICapture()
{
    HWND target_hwnd = nullptr;

    if (config.contains("window_handle"))
    {
        std::string handle_str = config["window_handle"];
        if (handle_str.find("0x") == 0 || handle_str.find("0X") == 0)
        {
            target_hwnd =
                reinterpret_cast<HWND>(std::stoull(handle_str, nullptr, 16));
        }
        else
        {
            target_hwnd = reinterpret_cast<HWND>(std::stoull(handle_str));
        }
        DAS_CORE_LOG_INFO(
            "Target window handle: 0x{:X}",
            reinterpret_cast<uintptr_t>(target_hwnd));
    }
    else if (config.contains("window_title"))
    {
        std::string title = config["window_title"];
        target_hwnd = FindWindowByTitle(title.c_str());
        if (target_hwnd == nullptr)
        {
            DAS_CORE_LOG_ERROR("Window not found with title: {}", title);
            return DAS_E_NOT_FOUND;
        }
        DAS_CORE_LOG_INFO("Target window by title: {}", title);
    }
    else if (config.contains("process_name"))
    {
        std::string proc_name = config["process_name"];
        DWORD       pid = FindProcessByName(proc_name.c_str());
        if (pid == 0)
        {
            DAS_CORE_LOG_ERROR("Process not found: {}", proc_name);
            return DAS_E_NOT_FOUND;
        }
        target_hwnd = FindMainWindowForProcess(pid);
        if (target_hwnd == nullptr)
        {
            DAS_CORE_LOG_ERROR(
                "Main window not found for process: {}",
                proc_name);
            return DAS_E_NOT_FOUND;
        }
        DAS_CORE_LOG_INFO("Target process: {}", proc_name);
    }
    else if (config.contains("process_id"))
    {
        uint32_t pid = config["process_id"];
        target_hwnd = FindMainWindowForProcess(pid);
        if (target_hwnd == nullptr)
        {
            DAS_CORE_LOG_ERROR("Main window not found for PID: {}", pid);
            return DAS_E_NOT_FOUND;
        }
        DAS_CORE_LOG_INFO("Target PID: {}", pid);
    }
    else if (config.contains("monitor_index"))
    {
        target_hwnd = GetDesktopWindow();
        target_monitor_index_ = config["monitor_index"];
        DAS_CORE_LOG_INFO("Target monitor index: {}", target_monitor_index_);
    }
    else
    {
        DAS_CORE_LOG_ERROR("No valid target key in config");
        return DAS_E_INVALID_ARGUMENT;
    }

    target_window_handle_ = target_hwnd;

    auto hr = gdi_capture_.Initialize(target_hwnd);
    if (FAILED(hr))
    {
        DAS_CORE_LOG_ERROR("Failed to initialize GDI capture: 0x{:08X}", hr);
        return hr;
    }

    return DAS_S_OK;
}

DasResult WindowsCapture::InitializeGraphicsCapture()
{
    HWND target_hwnd = nullptr;

    if (config.contains("window_handle"))
    {
        std::string handle_str = config["window_handle"];
        if (handle_str.find("0x") == 0 || handle_str.find("0X") == 0)
        {
            target_hwnd =
                reinterpret_cast<HWND>(std::stoull(handle_str, nullptr, 16));
        }
        else
        {
            target_hwnd = reinterpret_cast<HWND>(std::stoull(handle_str));
        }
    }
    else if (config.contains("window_title"))
    {
        std::string title = config["window_title"];
        target_hwnd = FindWindowByTitle(title.c_str());
    }
    else if (config.contains("process_name"))
    {
        std::string proc_name = config["process_name"];
        DWORD       pid = FindProcessByName(proc_name.c_str());
        if (pid != 0)
        {
            target_hwnd = FindMainWindowForProcess(pid);
        }
    }
    else if (config.contains("process_id"))
    {
        uint32_t pid = config["process_id"];
        target_hwnd = FindMainWindowForProcess(pid);
    }
    else if (config.contains("monitor_index"))
    {
        target_hwnd = GetDesktopWindow();
        target_monitor_index_ = config["monitor_index"];
    }

    if (target_hwnd == nullptr)
    {
        DAS_CORE_LOG_ERROR("No valid target found");
        return DAS_E_NOT_FOUND;
    }

    target_window_handle_ = target_hwnd;

    auto hr = graphics_capture_.Initialize(target_hwnd);
    if (FAILED(hr))
    {
        DAS_CORE_LOG_ERROR(
            "Failed to initialize Windows.Graphics.Capture: 0x{:08X}",
            hr);
        return hr;
    }

    return DAS_S_OK;
}

DasResult WindowsCapture::Capture(IDasImage** pp_out_image)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_image);

    if (!initialized_)
    {
        auto hr = StartCapture();
        if (FAILED(hr))
        {
            DAS_CORE_LOG_ERROR("Failed to start capture: 0x{:08X}", hr);
            return hr;
        }
    }

    uint8_t* frame_data = nullptr;
    int32_t  width = 0;
    int32_t  height = 0;

    DasResult hr;
    if (mode_ == CaptureMode::WindowsGraphicsCapture)
    {
        hr = graphics_capture_.Capture(&frame_data, &width, &height);
    }
    else
    {
        hr = gdi_capture_.Capture(&frame_data, &width, &height);
    }

    if (FAILED(hr))
    {
        DAS_CORE_LOG_ERROR("Capture failed: 0x{:08X}", hr);
        return hr;
    }

    auto create_result =
        CreateIDasImageFromRgb888(frame_data, width, height, pp_out_image);
    if (FAILED(create_result))
    {
        DAS_CORE_LOG_ERROR(
            "Failed to create IDasImage: 0x{:08X}",
            create_result);
        return create_result;
    }

    return DAS_S_OK;
}

DasResult WindowsCapture::StartCapture()
{
    if (initialized_)
    {
        DAS_CORE_LOG_WARN("Capture already started");
        return DAS_S_OK;
    }

    if (!pInitializeGDICapture_ && !pInitializeGraphicsCapture_)
    {
        DAS_CORE_LOG_ERROR("No capture mode initialized");
        return DAS_E_INVALID_ARGUMENT;
    }

    DasResult hr = DAS_S_OK;
    if (pInitializeGDICapture_)
    {
        hr = (this->*pInitializeGDICapture_)();
    }
    else if (pInitializeGraphicsCapture_)
    {
        hr = (this->*pInitializeGraphicsCapture_)();
    }

    if (FAILED(hr))
    {
        return hr;
    }

    initialized_ = true;
    return DAS_S_OK;
}

DasResult WindowsCapture::StopCapture()
{
    if (!initialized_)
    {
        return DAS_S_OK;
    }

    CleanupAll();
    initialized_ = false;
    return DAS_S_OK;
}

void WindowsCapture::CleanupGDICapture() { gdi_capture_.Cleanup(); }

void WindowsCapture::CleanupGraphicsCapture() { graphics_capture_.Cleanup(); }

void WindowsCapture::CleanupAll()
{
    if (mode_ == CaptureMode::WindowsGraphicsCapture)
    {
        CleanupGraphicsCapture();
    }
    else if (mode_ == CaptureMode::GDI)
    {
        CleanupGDICapture();
    }
}

DAS_NS_END
