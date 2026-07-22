#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    // One discovered desktop window. `handle` is the MaaToolkit-reported HWND
    // (Win32) or CGWindowID (macOS) — opaque to the caller, forwarded straight
    // to MaaWin32ControllerCreate / MaaGamepadControllerCreate.
    struct WindowInfo
    {
        void*       handle = nullptr;
        std::string class_name;
        std::string window_name;
    };

    // Injectable window enumerator. Production DefaultWindowEnumerator wraps
    // MaaToolkitDesktopWindowFindAll; tests pass a lambda returning a fixed
    // vector to drive 0/1/N-match coverage without linking MaaToolkit.
    using WindowEnumerator = std::function<std::vector<WindowInfo>()>;

    struct WindowMatchResult
    {
        void* handle = nullptr;
        // Empty on success. On failure, one of:
        //   - controller-window-not-found   (0 regex matches)
        //   - controller-window-ambiguous   (>1 regex matches)
        std::string error_code;
        std::string error_message;
    };

    // Find the unique desktop window whose class name matches `class_regex`
    // and whose window name matches `window_regex`. Either regex may be empty
    // (matches anything). Regex is ECMA-262 (std::regex) case-sensitive.
    //
    // 0 matches  → controller-window-not-found
    // 1 match    → success, returns handle
    // >1 matches → controller-window-ambiguous
    WindowMatchResult FindWindowByRegex(
        std::string_view        class_regex,
        std::string_view        window_regex,
        const WindowEnumerator& enumerator);

    // Convenience overload using the production default enumerator
    // (MaaToolkitDesktopWindowFindAll). Tests should call the 3-arg form.
    WindowMatchResult FindWindowByRegex(
        std::string_view class_regex,
        std::string_view window_regex);

    // Production enumerator backed by MaaToolkitDesktopWindowFindAll. Under
    // DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY (test build) returns an empty list.
    WindowEnumerator DefaultWindowEnumerator();

    // Test seam: install a custom enumerator used by the 2-arg
    // FindWindowByRegex overload. Pass nullptr to restore the default.
    void SetWindowEnumeratorForTest(WindowEnumerator enumerator);
} // namespace Das::Plugins::DasMaaPi
