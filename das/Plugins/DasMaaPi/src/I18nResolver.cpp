#include "I18nResolver.h"

#include <string>
#include <string_view>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    // Verbatim move from AuthoringProjector.cpp anonymous namespace. The
    // signature is unchanged so existing AuthoringProjector callers resolve
    // to this definition via ADL on `Das::Plugins::DasMaaPi`.
    std::string_view ResolveI18n(
        std::string_view text,
        const PiCatalog& catalog,
        std::string_view lang)
    {
        if (text.empty())
        {
            return "";
        }
        if (!text.starts_with('$'))
        {
            return text;
        }
        const std::string_view key = text.substr(1);
        auto lang_it = catalog.translations.find(std::string(lang));
        if (lang_it == catalog.translations.end())
        {
            if (!catalog.translations.empty())
            {
                lang_it = catalog.translations.begin();
            }
            else
            {
                return key;
            }
        }
        auto key_it = lang_it->second.find(std::string(key));
        if (key_it != lang_it->second.end())
        {
            return std::string_view(key_it->second);
        }
        return key;
    }

    // Max recursion depth for ResolveI18nJsonImpl. PI interface.json
    // structures are shallow (typically < 10 nesting levels). Pathological
    // deeply-nested JSON that could overflow the call stack is degenerated
    // to a verbatim deep-clone of the subtree (no resolution). 64 is far
    // above any legitimate PI document depth while staying well under the
    // per-frame stack budget — defense in depth, since the yyjson parser
    // itself also imposes a nesting cap.
    inline constexpr int I18N_RESOLVE_MAX_DEPTH = 64;

    yyjson::value ResolveI18nJsonImpl(
        const yyjson::value& raw,
        const PiCatalog&     catalog,
        std::string_view     lang,
        int                  depth)
    {
        if (depth >= I18N_RESOLVE_MAX_DEPTH)
        {
            // Degenerate path: preserve the subtree verbatim (deep clone,
            // no $key resolution). Prevents stack exhaustion on adversarial
            // or pathological input without crashing the host.
            return Das::Utils::CloneYyjsonValue(raw);
        }
        if (auto str = raw.as_string())
        {
            const auto resolved = ResolveI18n(
                std::string_view(str->data(), str->size()),
                catalog,
                lang);
            // NOTE: parens (not braces) are load-bearing here. cpp_yyjson's
            // constructor overload resolution picks a different path for
            // braced-init-list `{std::string(...)}` that does NOT trigger the
            // rvalue-reference yyjson_mut_strncpy copy, leaving the value
            // empty at read time. The paren form `yyjson::value(std::string)`
            // reliably enters the rvalue-string set_value path that copies the
            // bytes into the mutable document pool (see
            // .claude/skills/das-yyjson-string-safety). Suppress the stylistic
            // modernize-return-braced-init-list warning accordingly.
            // NOLINTNEXTLINE(modernize-return-braced-init-list)
            return yyjson::value(std::string(resolved));
        }
        if (auto obj = raw.as_object())
        {
            yyjson::value out(Das::Utils::MakeYyjsonObject());
            auto          out_obj = out.as_object();
            for (auto it = obj->begin(); it != obj->end(); ++it)
            {
                (*out_obj)[std::string_view(it->first)] =
                    ResolveI18nJsonImpl(it->second, catalog, lang, depth + 1);
            }
            return out;
        }
        if (auto arr = raw.as_array())
        {
            yyjson::value out(Das::Utils::MakeYyjsonArray());
            auto          out_arr = out.as_array();
            for (const auto& elem : *arr)
            {
                out_arr->emplace_back(
                    ResolveI18nJsonImpl(elem, catalog, lang, depth + 1));
            }
            return out;
        }
        // number / bool / null — deep clone via the canonical helper.
        // (Braced `return {raw};` does not reliably deep-copy cpp_yyjson
        // owning values across compilers; CloneYyjsonValue is the project
        // standard, matching AuthoringProjector.cpp:378 / :477 / :1895.)
        return Das::Utils::CloneYyjsonValue(raw);
    }

    // Recursive rebuild (never mut_doc mutation — Pitfall 5 owning-value-move
    // hazard). Each `$`-prefixed string leaf is resolved through ResolveI18n
    // and immediately materialized into an owning yyjson::value via the
    // rvalue std::string path (Pitfall 3 / yyjson-string-safety: catalog
    // string_view must be copied into std::string before constructing the
    // new value). Public entry point seeds the depth counter; the internal
    // ResolveI18nJsonImpl caps nesting at I18N_RESOLVE_MAX_DEPTH.
    yyjson::value ResolveI18nJson(
        const yyjson::value& raw,
        const PiCatalog&     catalog,
        std::string_view     lang)
    {
        return ResolveI18nJsonImpl(raw, catalog, lang, 0);
    }
} // namespace Das::Plugins::DasMaaPi
