#include "Win32WindowFinder.h"

#include "MaaDynamicSymbols.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <utility>

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
            // MaaToolkit is loaded at runtime via MaaDynamicSymbols (one
            // cross-platform binary; no #ifdef
            // DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY branch needed). When the DLL
            // is not co-located or any of the window-enumeration symbols are
            // not exported, every pointer stays null and we return an empty
            // list — the caller surfaces controller-window-not-found, exactly
            // as if no desktop window matched.
            const auto& symbols = LoadMaaDynamicSymbols();
            if (!symbols.tk_list_create || !symbols.tk_list_destroy
                || !symbols.tk_find_all || !symbols.tk_list_size
                || !symbols.tk_list_at || !symbols.tk_get_handle
                || !symbols.tk_get_class_name || !symbols.tk_get_window_name)
            {
                return {};
            }

            std::vector<WindowInfo> windows;
            // MaaToolkitDesktopWindowFindAll owns the returned list handle;
            // we copy out the fields we need then destroy it before
            // returning. Custom deleter routes through the runtime-resolved
            // destroy symbol (stateful — holds the function pointer).
            struct ToolkitListDeleter
            {
                MaaToolkitDesktopWindowListDestroyFn destroy;
                void operator()(void* p) const noexcept
                {
                    if (p && destroy)
                    {
                        destroy(p);
                    }
                }
            };
            const std::unique_ptr<void, ToolkitListDeleter> list(
                symbols.tk_list_create(),
                ToolkitListDeleter{symbols.tk_list_destroy});
            if (!list)
            {
                return windows;
            }
            if (!symbols.tk_find_all(list.get()))
            {
                return windows;
            }
            const auto size = symbols.tk_list_size(list.get());
            for (std::uint64_t i = 0; i < size; ++i)
            {
                const auto* window = symbols.tk_list_at(list.get(), i);
                if (!window)
                {
                    continue;
                }
                WindowInfo info;
                info.handle = symbols.tk_get_handle(window);
                if (const char* class_name = symbols.tk_get_class_name(window))
                {
                    info.class_name = class_name;
                }
                if (const char* window_name =
                        symbols.tk_get_window_name(window))
                {
                    info.window_name = window_name;
                }
                windows.emplace_back(std::move(info));
            }
            return windows;
        };
    }
} // namespace Das::Plugins::DasMaaPi
