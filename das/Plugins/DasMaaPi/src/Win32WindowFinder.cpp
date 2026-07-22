#include "Win32WindowFinder.h"

#include <algorithm>
#include <regex>
#include <string>
#include <utility>

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
#include <MaaToolkit/MaaToolkitAPI.h>
#endif

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // Compile an ECMA-262 regex. Returns std::nullopt for an empty pattern
        // (matches anything) so callers can short-circuit without forcing the
        // caller to special-case empty.
        std::optional<std::regex> CompileRegex(std::string_view pattern)
        {
            if (pattern.empty())
            {
                return std::nullopt;
            }
            return std::regex(std::string(pattern));
        }

        bool RegexMatches(
            const std::optional<std::regex>& regex,
            std::string_view                 text)
        {
            if (!regex)
            {
                return true; // empty regex = match-anything
            }
            return std::regex_search(std::string(text), *regex);
        }

        WindowEnumerator g_enumerator_for_test;
    } // namespace

    WindowMatchResult FindWindowByRegex(
        std::string_view        class_regex,
        std::string_view        window_regex,
        const WindowEnumerator& enumerator)
    {
        WindowMatchResult result;
        if (!enumerator)
        {
            result.error_code = "controller-window-not-found";
            result.error_message = "Window enumerator is not configured";
            return result;
        }

        const auto class_re = CompileRegex(class_regex);
        const auto window_re = CompileRegex(window_regex);

        std::vector<WindowInfo> matches;
        for (const auto& window : enumerator())
        {
            if (RegexMatches(class_re, window.class_name)
                && RegexMatches(window_re, window.window_name))
            {
                matches.emplace_back(window);
            }
        }

        if (matches.empty())
        {
            result.error_code = "controller-window-not-found";
            result.error_message = "No desktop window matched class_regex='"
                                   + std::string(class_regex)
                                   + "' window_regex='"
                                   + std::string(window_regex) + "'";
            return result;
        }
        if (matches.size() > 1)
        {
            result.error_code = "controller-window-ambiguous";
            result.error_message =
                "Multiple (" + std::to_string(matches.size())
                + ") desktop windows matched class_regex='"
                + std::string(class_regex) + "' window_regex='"
                + std::string(window_regex)
                + "'; refine the regex or pin a specific window";
            return result;
        }
        result.handle = matches.front().handle;
        return result;
    }

    WindowEnumerator DefaultWindowEnumerator()
    {
        return []() -> std::vector<WindowInfo>
        {
#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
            // MaaToolkitDesktopWindowFindAll owns the returned list handle;
            // we copy out the fields we need then destroy it before returning.
            std::vector<WindowInfo> windows;
            auto                    list = std::unique_ptr<
                MaaToolkitDesktopWindowList,
                decltype(&MaaToolkitDesktopWindowListDestroy)>(
                MaaToolkitDesktopWindowListCreate(),
                MaaToolkitDesktopWindowListDestroy);
            if (!list)
            {
                return windows;
            }
            if (!MaaToolkitDesktopWindowFindAll(list.get()))
            {
                return windows;
            }
            const auto size = MaaToolkitDesktopWindowListSize(list.get());
            for (MaaSize i = 0; i < size; ++i)
            {
                const auto* window =
                    MaaToolkitDesktopWindowListAt(list.get(), i);
                if (!window)
                {
                    continue;
                }
                WindowInfo info;
                info.handle = MaaToolkitDesktopWindowGetHandle(window);
                if (const char* class_name =
                        MaaToolkitDesktopWindowGetClassName(window))
                {
                    info.class_name = class_name;
                }
                if (const char* window_name =
                        MaaToolkitDesktopWindowGetWindowName(window))
                {
                    info.window_name = window_name;
                }
                windows.emplace_back(std::move(info));
            }
            return windows;
#else
            return {};
#endif
        };
    }

    void SetWindowEnumeratorForTest(WindowEnumerator enumerator)
    {
        if (enumerator)
        {
            g_enumerator_for_test = std::move(enumerator);
        }
        else
        {
            g_enumerator_for_test = nullptr;
        }
    }

    WindowMatchResult FindWindowByRegex(
        std::string_view class_regex,
        std::string_view window_regex)
    {
        const auto& enumerator = g_enumerator_for_test
                                     ? g_enumerator_for_test
                                     : DefaultWindowEnumerator();
        return FindWindowByRegex(class_regex, window_regex, enumerator);
    }
} // namespace Das::Plugins::DasMaaPi
