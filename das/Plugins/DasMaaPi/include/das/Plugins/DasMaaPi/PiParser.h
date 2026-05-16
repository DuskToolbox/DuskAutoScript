#pragma once

#include <das/Plugins/DasMaaPi/PiCatalog.h>

#include <filesystem>

namespace Das::Plugins::DasMaaPi
{
    struct PiParseRequest
    {
        std::filesystem::path interface_path;
        bool                  load_languages = true;
    };

    struct PiParseResult
    {
        bool      ok = false;
        PiCatalog catalog;
    };

    PiParseResult ParseProjectInterface(const PiParseRequest& request);
} // namespace Das::Plugins::DasMaaPi
