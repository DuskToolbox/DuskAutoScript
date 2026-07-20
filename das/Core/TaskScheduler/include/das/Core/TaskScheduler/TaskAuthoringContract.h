#pragma once

#include <cassert>
#include <cpp_yyjson.hpp>
#include <cstdint>
#include <string>

namespace Das::Core::TaskScheduler
{

    struct AuthoringValidationResult
    {
        bool        valid = false;
        std::string error_kind;
        std::string message;
    };

    AuthoringValidationResult ValidateAuthoringDocument(
        const yyjson::value& document);

    AuthoringValidationResult ValidateAuthoringChange(
        const yyjson::value& change);

    // D-84-08: Provider Get result envelope `{document?, diagnostics[]}`.
    // `document` is optional; `diagnostics` is required (may be empty).
    AuthoringValidationResult ValidateAuthoringGetResult(
        const yyjson::value& result);

    // D-84-08/D-84-10: provider Apply result envelope
    // `{acceptedProperties, document, diagnostics[]}`. The result must carry
    // non-null `acceptedProperties`, a valid `document`, and a strict
    // `diagnostics` array; `document.revision` must equal `baseRevision + 1`.
    AuthoringValidationResult ValidateAuthoringApplyResult(
        const yyjson::value& result,
        int64_t              baseRevision);

    // D-84-08: provider Compile result envelope `{compile, diagnostics[]}`.
    AuthoringValidationResult ValidateAuthoringCompileResult(
        const yyjson::value& result);

    yyjson::value MakeAuthoringError(
        std::string_view error_kind,
        std::string_view message);

} // namespace Das::Core::TaskScheduler
