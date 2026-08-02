#include "PiSemver.h"

#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // Strip a single leading 'v'/'V' prefix (MXU compareVersions
        // normalize).
        std::string_view StripVPrefix(std::string_view s)
        {
            if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
            {
                s.remove_prefix(1);
            }
            return s;
        }

        // Parse a decimal segment; fails on empty or non-digit input.
        bool ParseInt(std::string_view text, int& out)
        {
            if (text.empty())
            {
                return false;
            }
            int value = 0;
            for (const char c : text)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
                value = value * 10 + (c - '0');
            }
            out = value;
            return true;
        }

        // Lenient coercion: strip the v prefix and the prerelease suffix, then
        // take up to three leading numeric dot-segments, padding missing parts
        // with 0 ("2.0" -> 2.0.0). Fails when no leading numeric segment exists
        // (the caller then falls back to string comparison).
        bool CoerceSemVer(std::string_view s, PiSemVer& out)
        {
            s = StripVPrefix(s);
            if (const auto dash = s.find('-'); dash != std::string_view::npos)
            {
                s = s.substr(0, dash);
            }

            int  parts[3] = {0, 0, 0};
            auto segment = s;
            for (int i = 0; i < 3; ++i)
            {
                const auto dot = segment.find('.');
                const auto part = segment.substr(0, dot);
                if (part.empty() || !ParseInt(part, parts[i]))
                {
                    if (i == 0)
                    {
                        return false;
                    }
                    break;
                }
                if (dot == std::string_view::npos)
                {
                    break;
                }
                segment = segment.substr(dot + 1);
            }

            out.major = parts[0];
            out.minor = parts[1];
            out.patch = parts[2];
            out.prerelease.clear();
            return true;
        }
    } // namespace

    bool ParseSemVer(std::string_view s, PiSemVer& out)
    {
        s = StripVPrefix(s);

        std::string_view core = s;
        std::string_view prerelease;
        if (const auto dash = s.find('-'); dash != std::string_view::npos)
        {
            core = s.substr(0, dash);
            prerelease = s.substr(dash + 1);
        }

        // core must be exactly major.minor.patch (three numeric segments).
        int  parts[3] = {0, 0, 0};
        auto segment = core;
        for (int i = 0; i < 3; ++i)
        {
            const auto dot = segment.find('.');
            if (dot == std::string_view::npos)
            {
                // A short version (fewer than 3 parts) is not a full semver and
                // must be handled by coercion.
                if (i != 2)
                {
                    return false;
                }
                if (!ParseInt(segment, parts[i]))
                {
                    return false;
                }
                segment = {};
                break;
            }
            if (!ParseInt(segment.substr(0, dot), parts[i]))
            {
                return false;
            }
            segment = segment.substr(dot + 1);
        }
        if (!segment.empty())
        {
            // A fourth dot segment invalidates the strict parse.
            return false;
        }

        out.major = parts[0];
        out.minor = parts[1];
        out.patch = parts[2];
        out.prerelease = std::string(prerelease);
        return true;
    }

    int CompareSemVer(const PiSemVer& a, const PiSemVer& b)
    {
        if (a.major != b.major)
        {
            return a.major < b.major ? -1 : 1;
        }
        if (a.minor != b.minor)
        {
            return a.minor < b.minor ? -1 : 1;
        }
        if (a.patch != b.patch)
        {
            return a.patch < b.patch ? -1 : 1;
        }
        if (a.prerelease.empty() && b.prerelease.empty())
        {
            return 0;
        }
        // Formal release (empty prerelease) sorts above a prerelease.
        if (a.prerelease.empty())
        {
            return 1;
        }
        if (b.prerelease.empty())
        {
            return -1;
        }
        return a.prerelease.compare(b.prerelease);
    }

    int CompareVersions(const std::string& a, const std::string& b)
    {
        PiSemVer   va;
        PiSemVer   vb;
        const bool pa = ParseSemVer(a, va);
        const bool pb = ParseSemVer(b, vb);
        if (pa && pb)
        {
            return CompareSemVer(va, vb);
        }

        // Coerce fallback: drop the prerelease and pad the numeric triple with
        // zeros so "2.0" compares as "2.0.0".
        PiSemVer   ea;
        PiSemVer   eb;
        const bool coa = pa || CoerceSemVer(a, ea);
        const bool cob = pb || CoerceSemVer(b, eb);
        if (coa && cob)
        {
            const PiSemVer& na = pa ? va : ea;
            const PiSemVer& nb = pb ? vb : eb;
            return CompareSemVer(na, nb);
        }

        // Last resort: normalized string comparison (MXU localeCompare).
        return StripVPrefix(a).compare(StripVPrefix(b));
    }
} // namespace Das::Plugins::DasMaaPi
