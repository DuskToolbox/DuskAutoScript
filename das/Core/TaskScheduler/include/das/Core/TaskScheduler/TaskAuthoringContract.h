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

    // Guard error returned by SchedulerService lock-held authoring helpers
    // (CheckSchedulerIdleLocked / ResolveAuthoringCapabilityLocked). The
    // std::optional<AuthoringGuardError>::engaged state expresses "guard
    // failed" so no separate valid flag is needed.
    struct AuthoringGuardError
    {
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
    // `kind` names the applied change: the viewData lightweight channel
    // ("moveGraphNode") does not bump the semantic revision, so its success
    // envelope must keep `document.revision == baseRevision`; every other
    // kind still requires `baseRevision + 1` on success. The default keeps
    // existing callers on the semantic path.
    AuthoringValidationResult ValidateAuthoringApplyResult(
        const yyjson::value& result,
        int64_t              baseRevision,
        std::string_view     kind = {});

    // D-84-08: provider Compile result envelope `{compile, diagnostics[]}`.
    AuthoringValidationResult ValidateAuthoringCompileResult(
        const yyjson::value& result);

    yyjson::value MakeAuthoringError(
        std::string_view error_kind,
        std::string_view message);

    // Diagnostic-array inspection helpers. Used by the apply pipeline to
    // distinguish a provider-rejected apply (error diagnostics present) from a
    // malformed envelope, so the real rejection reason surfaces instead of a
    // generic revisionMismatch. Defined in TaskAuthoringContract.cpp.
    bool        HasErrorDiagnostic(const yyjson::value& diagnostics_array);
    std::string FirstErrorDiagnosticMessage(
        const yyjson::value& diagnostics_array);

} // namespace Das::Core::TaskScheduler
