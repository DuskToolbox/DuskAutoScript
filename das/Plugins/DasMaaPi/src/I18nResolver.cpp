#include "I18nResolver.h"

namespace Das::Plugins::DasMaaPi
{
    // RED stub: returns text unchanged. Real body is filled in GREEN.
    std::string_view ResolveI18n(
        std::string_view text,
        const PiCatalog& /*catalog*/,
        std::string_view /*lang*/)
    {
        return text;
    }

    // RED stub: deep-clones input unchanged. Real body is filled in GREEN.
    yyjson::value ResolveI18nJson(
        const yyjson::value& raw,
        const PiCatalog& /*catalog*/,
        std::string_view /*lang*/)
    {
        return {raw};
    }
} // namespace Das::Plugins::DasMaaPi
