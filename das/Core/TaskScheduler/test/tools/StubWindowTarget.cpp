// 窗口替身测试目标（FormSequenceLaunch E2E 用）：纯 Win32 小程序，
// 以命令行参数确定性控制窗口类名 / 标题 / 延迟出现毫秒数 / 生命周期毫秒数。
// 模拟「目标程序 spawn 到窗口出现有数秒延迟」的起源痛点场景：
// 窗口先以隐藏状态创建，延迟期满后再显示，launch 组件的可见窗口轮询
// 只有在延迟期满后才能匹配到它。WM_CLOSE 与 lifetime-ms 定时器任一触发
// 即销毁窗口退出，保证失败路径不遗留僵尸窗口。
#include <windows.h>

#include <charconv>
#include <string>
#include <string_view>

namespace
{
    constexpr wchar_t  DEFAULT_WINDOW_CLASS[] = L"DasStubWindowTarget";
    constexpr wchar_t  DEFAULT_WINDOW_TITLE[] = L"DasStubWindowTarget";
    constexpr DWORD    DEFAULT_DELAY_MS = 0;
    constexpr DWORD    DEFAULT_LIFETIME_MS = 60000;
    constexpr UINT_PTR LIFETIME_TIMER_ID = 1;

    struct StubWindowConfig
    {
        std::wstring window_class = DEFAULT_WINDOW_CLASS;
        std::wstring window_title = DEFAULT_WINDOW_TITLE;
        DWORD        delay_ms = DEFAULT_DELAY_MS;
        DWORD        lifetime_ms = DEFAULT_LIFETIME_MS;
    };

    std::wstring ToWide(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size_needed = MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0);
        if (size_needed <= 0)
        {
            return {};
        }
        std::wstring result(size_needed, L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            size_needed);
        return result;
    }

    DWORD ParseMillisecondValue(std::string_view text, DWORD fallback)
    {
        DWORD      value = 0;
        const auto parse_result =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (parse_result.ec != std::errc{} || parse_result.ptr == text.data())
        {
            return fallback;
        }
        return value;
    }

    StubWindowConfig ParseArguments(int argc, char** argv)
    {
        StubWindowConfig config;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument{argv[index]};
            if (argument == "--class" && index + 1 < argc)
            {
                config.window_class = ToWide(argv[++index]);
            }
            else if (argument == "--title" && index + 1 < argc)
            {
                config.window_title = ToWide(argv[++index]);
            }
            else if (argument == "--delay-ms" && index + 1 < argc)
            {
                config.delay_ms =
                    ParseMillisecondValue(argv[++index], DEFAULT_DELAY_MS);
            }
            else if (argument == "--lifetime-ms" && index + 1 < argc)
            {
                config.lifetime_ms =
                    ParseMillisecondValue(argv[++index], DEFAULT_LIFETIME_MS);
            }
        }
        return config;
    }

    LRESULT CALLBACK
    StubWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
    {
        switch (message)
        {
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, w_param, l_param);
        }
    }
} // namespace

int main(int argc, char* argv[])
{
    const StubWindowConfig config = ParseArguments(argc, argv);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr)
    {
        return 1;
    }

    const WNDCLASSW window_class{
        CS_NOCLOSE,
        StubWindowProc,
        0,
        0,
        instance,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        config.window_class.c_str()};
    if (RegisterClassW(&window_class) == 0)
    {
        return 1;
    }

    const HWND window = CreateWindowExW(
        0,
        config.window_class.c_str(),
        config.window_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        200,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr)
    {
        return 1;
    }

    // spawn 到窗口出现的延迟模拟：延迟期内窗口存在但不可见，可见窗口
    // 轮询（IsWindowVisible）在显示之前匹配不到。
    Sleep(config.delay_ms);
    ShowWindow(window, SW_SHOW);

    // 生命周期兜底：即使无人投递 WM_CLOSE，定时器到期也强制销毁窗口，
    // 失败路径不遗留僵尸窗口。
    SetTimer(window, LIFETIME_TIMER_ID, config.lifetime_ms, nullptr);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (message.message == WM_TIMER && message.wParam == LIFETIME_TIMER_ID)
        {
            DestroyWindow(window);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
