#pragma once

#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Utils/DasJsonCore.h>

#include <optional>
#include <string>
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

    // Select the PI document language used for `$key` resolution, applying the
    // shared fallback policy so every caller resolves the same catalog to the
    // same language within one authoring cycle:
    //   1. Explicit non-empty setting wins.
    //   2. Otherwise prefer zh_cn when the catalog has it (PI convention).
    //   3. Otherwise the first available language in catalog order.
    //   4. Otherwise empty (caller proceeds with an empty language, which
    //      ResolveI18n treats as a miss / key-without-`$` fallback).
    // `catalog` may be null (caller has no catalog) — returns empty.
    // Centralizing this here keeps PiCompiler (envelope.maapi.pi_env raw
    // JSON) and AuthoringProjector (document field labels) aligned instead
    // of each picking a fallback by a different rule.
    inline std::string SelectPiLanguage(
        const std::optional<std::string>& setting,
        const PiCatalog*                  catalog)
    {
        if (setting && !setting->empty())
        {
            return *setting;
        }
        if (!catalog)
        {
            return {};
        }
        const auto zh_cn = catalog->translations.find("zh_cn");
        if (zh_cn != catalog->translations.end())
        {
            return "zh_cn";
        }
        if (!catalog->translations.empty())
        {
            return catalog->translations.begin()->first;
        }
        return {};
    }

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
    //
    // Nesting is capped at an internal depth limit (well above any legitimate
    // PI document depth). If the limit is exceeded the offending subtree is
    // deep-cloned unchanged (no resolution) rather than risking stack
    // exhaustion on pathological or adversarial input.
    //
    // Caller scope: PiCompiler invokes this ONLY on controller / resource
    // raw_json destined for AgentRuntime — a separate process that has no
    // catalog access and cannot resolve `$key` itself. Display-layer
    // strings (welcome messages, task labels, etc.) are NOT routed through
    // this entry; AuthoringProjector resolves them during projection with
    // its own display-time access to the catalog. The narrow scope is an
    // intentional layering decision, not a coverage gap.
    yyjson::value ResolveI18nJson(
        const yyjson::value& raw,
        const PiCatalog&     catalog,
        std::string_view     lang);
} // namespace Das::Plugins::DasMaaPi
