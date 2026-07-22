#include "ControllerMethodMaps.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::transform(
                out.begin(),
                out.end(),
                out.begin(),
                [](unsigned char ch)
                { return static_cast<char>(std::tolower(ch)); });
            return out;
        }

        // Split "A|B|C" into trimmed lowercase tokens. Empty tokens are dropped
        // (so "GDI|" doesn't falsely produce a zero-named bit). Whitespace
        // inside tokens is preserved (PI protocol names are identifier-shaped).
        std::vector<std::string> SplitPipe(std::string_view text)
        {
            std::vector<std::string> tokens;
            std::size_t              start = 0;
            for (std::size_t i = 0; i <= text.size(); ++i)
            {
                if (i == text.size() || text[i] == '|')
                {
                    if (i > start)
                    {
                        tokens.emplace_back(
                            Lower(text.substr(start, i - start)));
                    }
                    start = i + 1;
                }
            }
            return tokens;
        }

        // Lookup table for MaaWin32ScreencapMethod single-bit names
        // (MaaDef.h:348-355). Returns 0 on unknown name.
        std::uint64_t SingleScreencapBit(const std::string& name)
        {
            if (name == "gdi")
            {
                return 1ULL;
            }
            if (name == "framepool")
            {
                return 1ULL << 1;
            }
            if (name == "dxgi_desktopdup")
            {
                return 1ULL << 2;
            }
            if (name == "dxgi_desktopdup_window")
            {
                return 1ULL << 3;
            }
            if (name == "printwindow")
            {
                return 1ULL << 4;
            }
            if (name == "screendc")
            {
                return 1ULL << 5;
            }
            return 0ULL;
        }

        // Lookup table for MaaWin32InputMethod single-select values
        // (MaaDef.h:393-404). Returns 0 on unknown name.
        std::uint64_t InputMethodValue(const std::string& name)
        {
            if (name == "seize")
            {
                return 1ULL;
            }
            if (name == "sendmessage")
            {
                return 1ULL << 1;
            }
            if (name == "postmessage")
            {
                return 1ULL << 2;
            }
            if (name == "legacyevent")
            {
                return 1ULL << 3;
            }
            if (name == "postthreadmessage")
            {
                return 1ULL << 4;
            }
            if (name == "sendmessagewithcursorpos")
            {
                return 1ULL << 5;
            }
            if (name == "postmessagewithcursorpos")
            {
                return 1ULL << 6;
            }
            if (name == "sendmessagewithwindowpos")
            {
                return 1ULL << 7;
            }
            if (name == "postmessagewithwindowpos")
            {
                return 1ULL << 8;
            }
            if (name == "interception")
            {
                return 1ULL << 9;
            }
            return 0ULL;
        }
    } // namespace

    std::uint64_t ParseWin32ScreencapMethod(std::string_view text)
    {
        if (text.empty())
        {
            return 0ULL;
        }
        const auto lowered = Lower(text);

        // Predefined combos — expand to their OR (MaaDef.h:357-359).
        if (lowered == "all")
        {
            return ~0ULL;
        }
        if (lowered == "foreground")
        {
            return (1ULL << 3) | (1ULL << 5);
        }
        if (lowered == "background")
        {
            return (1ULL << 1) | (1ULL << 4);
        }

        // Single token or '|'-delimited set. Unknown tokens fail-closed.
        std::uint64_t bits = 0;
        for (const auto& token : SplitPipe(lowered))
        {
            const auto bit = SingleScreencapBit(token);
            if (bit == 0ULL)
            {
                return 0ULL;
            }
            bits |= bit;
        }
        return bits;
    }

    std::uint64_t ParseWin32InputMethod(std::string_view text)
    {
        if (text.empty())
        {
            return 0ULL;
        }
        return InputMethodValue(Lower(text));
    }

    std::uint64_t ParseGamepadType(std::string_view text)
    {
        if (text.empty())
        {
            return 0ULL;
        }
        const auto lowered = Lower(text);
        if (lowered == "xbox360")
        {
            return 0ULL;
        }
        if (lowered == "dualshock4" || lowered == "ds4")
        {
            return 1ULL;
        }
        return 0ULL;
    }
} // namespace Das::Plugins::DasMaaPi
