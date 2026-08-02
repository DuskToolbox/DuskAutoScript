#pragma once

#include <string>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    struct PiSemVer
    {
        int         major = 0;
        int         minor = 0;
        int         patch = 0;
        std::string prerelease;
    };

    // Parse a strict semver (leading v/V stripped): major.minor.patch with an
    // optional -prerelease suffix. Returns false when the input is not a valid
    // full semver so the caller can fall back to coercion (MXU compareVersions
    // semantics).
    bool ParseSemVer(std::string_view s, PiSemVer& out);

    // Compare two parsed versions: major -> minor -> patch; on equal patch a
    // release (empty prerelease) sorts above a prerelease (1.6.0
    // > 1.6.0-beta.1). Returns <0, 0, >0.
    int CompareSemVer(const PiSemVer& a, const PiSemVer& b);

    // MXU compareVersions semantics: full semver compare first, coerce on
    // failure (drop prerelease, pad missing triple parts with 0), normalized
    // string comparison as a last resort. Returns <0, 0, >0.
    int CompareVersions(const std::string& a, const std::string& b);
} // namespace Das::Plugins::DasMaaPi
