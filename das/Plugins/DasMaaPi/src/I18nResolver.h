#pragma once

#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Utils/DasJsonCore.h>

#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    // Resolve PI `$key` placeholders against catalog.translations[lang].
    //
    // PI v2 protocol scope: `interface.json` source-field localization. The
    // `$` prefix marks a string as a translation key into the author-supplied
    // translation files (interface_zh.json etc.). Behavior:
    //   - text not starting with `$` -> returned unchanged.
    //   - empty text -> empty.
    //   - `$key` hit in translations[lang] -> translated text.
    //   - `$key` miss in translations[lang] -> key (without `$`).
    //   - requested lang missing -> falls back to first available language.
    //   - empty catalog -> key (without `$`).
    //
    // The returned string_view points into the catalog (stable for the
    // catalog's lifetime) or into `text` (caller-owned); callers must
    // immediately copy the result when the underlying storage is about to go
    // out of scope (the yyjson rebuild helper does this).
    std::string_view ResolveI18n(
        std::string_view text,
        const PiCatalog& catalog,
        std::string_view lang);

    // Recursively walk a yyjson value and resolve every `$`-prefixed string
    // leaf against catalog.translations[lang]. Returns a freshly constructed
    // value built via the rebuild API (MakeYyjsonObject / MakeYyjsonArray +
    // yyjson::value(std::string) for resolved strings) — the input is never
    // mutated. Non-string leaves (number / bool / null) are deep-cloned.
    //
    // Rebuild (not mut_doc mutation) is used deliberately: cpp_yyjson owning
    // values are immutable, and mutating them through the mut_doc API after
    // cross-function moves risks invalid UTF-8 on write. Rebuild produces a
    // clean independent value that the caller can serialize or move freely.
    yyjson::value ResolveI18nJson(
        const yyjson::value& raw,
        const PiCatalog&     catalog,
        std::string_view     lang);
} // namespace Das::Plugins::DasMaaPi
