#include "PiSemver.h"

namespace Das::Plugins::DasMaaPi
{
    // RED stub: every comparison returns 0 so the boundary assertions fail.
    bool ParseSemVer(std::string_view s, PiSemVer& out)
    {
        (void)s;
        (void)out;
        return false;
    }

    int CompareSemVer(const PiSemVer& a, const PiSemVer& b)
    {
        (void)a;
        (void)b;
        return 0;
    }

    int CompareVersions(const std::string& a, const std::string& b)
    {
        (void)a;
        (void)b;
        return 0;
    }
} // namespace Das::Plugins::DasMaaPi
