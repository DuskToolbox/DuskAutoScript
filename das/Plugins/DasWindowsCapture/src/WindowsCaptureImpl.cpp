
#include "GDICapture.h"
#include "WindowsGraphicsCapture.h"
#include <das/Core/DasWindowsCapture/WindowsCaptureImpl.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/fmt.h>
#include <dwmapi.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>

namespace
{
    struct EnumWindowsData
    {
        const char* window_title;
        HWND        result{nullptr};
    };

    BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
    {
        auto* data = reinterpret_cast<EnumWindowsData*>(lParam);
        char  title[256];
        if (GetWindowTextA(hwnd, title, sizeof(title)) > 0)
        {
            if (std::strstr(title, data->window_title) != nullptr)
            {
                data->result = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }

    HWND FindWindowByTitle(const char* title)
    {
        EnumWindowsData data{title};
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
        return data.result;
    }

    DWORD FindProcessByName(const char* process_name)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        PROCESSENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32First(snapshot, &entry))
        {
            CloseHandle(snapshot);
            return 0;
        }

        DWORD result = 0;
        do
        {
            if (std::strcmp(entry.szExeFile, process_name) == 0)
            {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));

        CloseHandle(snapshot);
        return result;
    }

    struct FindMainWindowData
    {
        DWORD target_pid;
        HWND  result{nullptr};
    };

    BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lParam)
    {
        auto* data = reinterpret_cast<FindMainWindowData*>(lParam);
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == data->target_pid)
        {
            if (GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd))
            {
                data->result = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }

    HWND FindMainWindowForProcess(DWORD pid)
    {
        FindMainWindowData data{pid};
        EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&data));
        return data.result;
    }
}

DAS_NS_BEGIN

bool WindowsCapture::ParseConfigAndSelectMode(const nlohmann::json& config)
{
    if (!config_.contains("capture_mode"))
    {
        DAS_LOG_ERROR("Missing capture_mode in config");
        return false;
    }

    config_ = config;
    std::string mode_str = config_["capture_mode"];
    capture_mode_ = mode_str;

    if (mode_str == "windows_graphics_capture")
    {
        mode_ = CaptureMode::WindowsGraphicsCapture;
        pInitializeGDICapture = nullptr;
        pInitializeGraphicsCapture = &WindowsCapture::InitializeGraphicsCapture;
        DAS_LOG_INFO("Selected Windows.Graphics.Capture mode");
        return true;
    }
    else if (mode_str == "gdi_bitblt")
    {
        mode_ = CaptureMode::GDI;
        pInitializeGDICapture = &WindowsCapture::InitializeGDICapture;
        pInitializeGraphicsCapture = nullptr;
        DAS_LOG_INFO("Selected GDI BitBlt mode");
        return true;
    }
    else
    {
        auto error_msg = DAS::fmt::format(
            "Invalid capture_mode: {}. Expected 'windows_graphics_capture' or 'gdi_bitblt'",
            mode_str);
        DAS_LOG_ERROR(error_msg.c_str());
        return false;
    }
}

DasResult WindowsCapture::InitializeGDICapture()
{
    HWND target_hwnd = nullptr;

    if (config_.contains("window_handle"))
    {
        std::string handle_str = config_["window_handle"];
        if (handle_str.find("0x") == 0 || handle_str.find("0X") == 0)
        {
            target_hwnd =
                reinterpret_cast<HWND>(std::stoull(handle_str, nullptr, 16));
        }
        else
        {
            target_hwnd = reinterpret_cast<HWND>(std::stoull(handle_str));
        }
        {
            auto info_msg = DAS::fmt::format(
                "Target window handle: 0x{:X}",
                reinterpret_cast<uintptr_t>(target_hwnd));
            DAS_LOG_INFO(info_msg.c_str());
        }
    }
    else if (config_.contains("window_title"))
    {
        std::string title = config_["window_title"];
        target_hwnd = FindWindowByTitle(title.c_str());
        if (target_hwnd == nullptr)
        {
            DAS_LOG_ERROR(
                DAS::fmt::format("Window not found with title: {}", title)
                    .c_str());
            return DAS_E_NOT_FOUND;
        }
        DAS_LOG_INFO(
            DAS::fmt::format("Target window by title: {}", title).c_str());
    }
    else if (config_.contains("process_name"))
    {
        std::string proc_name = config_["process_name"];
        DWORD       pid = FindProcessByName(proc_name.c_str());
        if (pid == 0)
        {
            DAS_LOG_ERROR(
                DAS::fmt::format("Process not found: {}", proc_name).c_str());
            return DAS_E_NOT_FOUND;
        }
        target_hwnd = FindMainWindowForProcess(pid);
        if (target_hwnd == nullptr)
        {
            DAS_LOG_ERROR(
                DAS::fmt::format(
                    "Main window not found for process: {}",
                    proc_name)
                    .c_str());
            return DAS_E_NOT_FOUND;
        }
        DAS_LOG_INFO(DAS::fmt::format("Target process: {}", proc_name).c_str());
    }
    else if (config_.contains("process_id"))
    {
        uint32_t pid = config_["process_id"];
        target_hwnd = FindMainWindowForProcess(pid);
        if (target_hwnd == nullptr)
        {
            auto error_msg =
                DAS::fmt::format("Main window not found for PID: {}", pid);
            DAS_LOG_ERROR(error_msg.c_str());
            return DAS_E_NOT_FOUND;
        }
        DAS_LOG_INFO(DAS::fmt::format("Target PID: {}", pid).c_str());
    }
    else if (config_.contains("monitor_index"))
    {
        target_hwnd = GetDesktopWindow();
        target_monitor_index_ = config_["monitor_index"];
        DAS_LOG_INFO(
            DAS::fmt::format("Target monitor index: {}", target_monitor_index_)
                .c_str());
    }
    else
    {
        DAS_LOG_ERROR("No valid target key in config");
        return DAS_E_INVALID_ARGUMENT;
    }

    target_window_handle_ = target_hwnd;

    auto hr = gdi_capture_.Initialize(target_hwnd);
    if (FAILED(hr))
    {
        DAS_LOG_ERROR(
            DAS::fmt::format("Failed to initialize GDI capture: 0x{:08X}", hr)
                .c_str());
        return hr;
    }

    return DAS_S_OK;
}

DasResult WindowsCapture::InitializeGraphicsCapture()
{
    HWND target_hwnd = nullptr;

    if (config_.contains("window_handle"))
    {
        std::string handle_str = config_["window_handle"];
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
    else if (config_.contains("window_title"))
    {
        std::string title = config_["window_title"];
        target_hwnd = FindWindowByTitle(title.c_str());
    }
    else if (config_.contains("process_name"))
    {
        std::string proc_name = config_["process_name"];
        DWORD       pid = FindProcessByName(proc_name.c_str());
        if (pid != 0)
        {
            target_hwnd = FindMainWindowForProcess(pid);
        }
    }
    else if (config_.contains("process_id"))
    {
        uint32_t pid = config_["process_id"];
        target_hwnd = FindMainWindowForProcess(pid);
    }
    else if (config_.contains("monitor_index"))
    {
        target_hwnd = GetDesktopWindow();
        target_monitor_index_ = config_["monitor_index"];
    }

    if (target_hwnd == nullptr)
    {
        DAS_LOG_ERROR("No valid target found");
        return DAS_E_NOT_FOUND;
    }

    target_window_handle_ = target_hwnd;

    auto hr = graphics_capture_.Initialize(target_hwnd);
    if (FAILED(hr))
    {
        DAS_LOG_ERROR(
            DAS::fmt::format(
                "Failed to initialize Windows.Graphics.Capture: 0x{:08X}",
                hr)
                .c_str());
        return hr;
    }

    return DAS_S_OK;
}

DasResult WindowsCapture::Capture(
    Das::ExportInterface::IDasImage** pp_out_image)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_image);

    if (!initialized_)
    {
        auto hr = StartCapture();
        if (FAILED(hr))
        {
            DAS_LOG_ERROR(
                DAS::fmt::format("Failed to start capture: 0x{:08X}", hr)
                    .c_str());
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
        auto error_msg = DAS::fmt::format("Capture failed: 0x{:08X}", hr);
        DAS_LOG_ERROR(error_msg.c_str());
        return hr;
    }

    DasImageDesc desc{};
    desc.p_data = reinterpret_cast<char*>(frame_data);
    desc.data_size = width * height * 3;
    desc.data_format =
        Das::ExportInterface::DasImageFormat::DAS_IMAGE_FORMAT_RGB_888;

    DAS::ExportInterface::DasSize size{};
    size.width = width;
    size.height = height;

    auto create_result =
        CreateIDasImageFromDecodedData(&desc, &size, pp_out_image);
    if (FAILED(create_result))
    {
        auto error_msg = DAS::fmt::format(
            "Failed to create IDasImage: 0x{:08X}",
            create_result);
        DAS_LOG_ERROR(error_msg.c_str());
        return create_result;
    }

    return DAS_S_OK;
}

DasResult WindowsCapture::StartCapture()
{
    if (initialized_)
    {
        DAS_LOG_WARNING("Capture already started");
        return DAS_S_OK;
    }

    if (!pInitializeGDICapture && !pInitializeGraphicsCapture)
    {
        DAS_LOG_ERROR("No capture mode initialized");
        return DAS_E_INVALID_ARGUMENT;
    }

    DasResult hr = DAS_S_OK;
    if (pInitializeGDICapture)
    {
        hr = (this->*pInitializeGDICapture)();
    }
    else if (pInitializeGraphicsCapture)
    {
        hr = (this->*pInitializeGraphicsCapture)();
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

DasResult WindowsCapture::GetGuid(DasGuid* p_out_guid)
{
    if (p_out_guid == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = DasIidOf<WindowsCapture>();
    return DAS_S_OK;
}

DasResult WindowsCapture::GetRuntimeClassName(IDasReadOnlyString** pp_out_name)
{
    if (pp_out_name == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    const char*             class_name = "Das.WindowsCapture";
    DAS::DasPtr<IDasString> p_name;
    auto hr = CreateIDasStringFromUtf8(class_name, p_name.Put());
    if (FAILED(hr))
    {
        return hr;
    }
    *pp_out_name = p_name.Reset();
    return DAS_S_OK;
}

DAS_NS_END
