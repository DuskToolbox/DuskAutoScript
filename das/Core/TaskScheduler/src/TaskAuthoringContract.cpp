#include <das/Core/TaskScheduler/TaskAuthoringContract.h>

#include <das/Utils/DasJsonCore.h>

#include <array>
#include <string_view>

namespace Das::Core::TaskScheduler
{
    namespace
    {
        constexpr std::array<std::string_view, 2> DOCUMENT_KINDS{
            "formSequence",
            "graph"};

        constexpr std::array<std::string_view, 10> CHANGE_KINDS{
            "setValue",
            "applyPreset",
            "addSequenceItem",
            "moveSequenceItem",
            "addNode",
            "connectPorts",
            "updateNodeConfig",
            "removeNode",
            "disconnectPorts",
            "removeSequenceItem"};

        bool IsOneOf(
            std::string_view value,
            const auto&      allowed_values) noexcept
        {
            for (auto allowed : allowed_values)
            {
                if (value == allowed)
                {
                    return true;
                }
            }
            return false;
        }

        bool HasObjectField(const auto& obj, std::string_view key)
        {
            return obj.contains(key) && obj[key].is_object();
        }

        bool HasArrayField(const auto& obj, std::string_view key)
        {
            return obj.contains(key) && obj[key].is_array();
        }

        bool HasStringField(const auto& obj, std::string_view key)
        {
            return obj.contains(key) && obj[key].is_string();
        }

        bool HasIntegerField(const auto& obj, std::string_view key)
        {
            return obj.contains(key)
                   && (obj[key].is_sint() || obj[key].is_uint());
        }

        AuthoringValidationResult Pass() { return {true, {}, {}}; }

        AuthoringValidationResult Fail(
            std::string error_kind,
            std::string message)
        {
            return {false, std::move(error_kind), std::move(message)};
        }

        // D-84-11 strict Diagnostic: required severity/message, optional
        // integer code, optional string path, no other fields.
        AuthoringValidationResult ValidateDiagnostic(const yyjson::value& diag)
        {
            auto obj_opt = diag.as_object();
            if (!obj_opt)
            {
                return Fail("invalidDiagnostic", "Diagnostic must be object");
            }
            const auto& obj = obj_opt.value();

            if (!HasStringField(obj, "severity"))
            {
                return Fail(
                    "invalidDiagnosticSeverity",
                    "Diagnostic.severity required");
            }
            const auto severity = obj["severity"].as_string().value();
            if (severity != std::string_view("info")
                && severity != std::string_view("warning")
                && severity != std::string_view("error"))
            {
                return Fail(
                    "invalidDiagnosticSeverity",
                    "Diagnostic.severity must be info|warning|error");
            }

            if (!HasStringField(obj, "message"))
            {
                return Fail(
                    "invalidDiagnosticMessage",
                    "Diagnostic.message required");
            }
            if (obj["message"].as_string().value().empty())
            {
                return Fail(
                    "invalidDiagnosticMessage",
                    "Diagnostic.message must be non-empty");
            }

            if (obj.contains("code"))
            {
                const auto& code = obj["code"];
                if (!code.is_sint() && !code.is_uint())
                {
                    return Fail(
                        "invalidDiagnosticCode",
                        "Diagnostic.code must be integer");
                }
            }

            if (obj.contains("path"))
            {
                if (!obj["path"].is_string())
                {
                    return Fail(
                        "invalidDiagnosticPath",
                        "Diagnostic.path must be string");
                }
            }

            for (auto [key, value] : obj)
            {
                if (key != std::string_view("severity")
                    && key != std::string_view("message")
                    && key != std::string_view("code")
                    && key != std::string_view("path"))
                {
                    return Fail(
                        "unknownDiagnosticField",
                        "Diagnostic rejects unknown field");
                }
            }
            return Pass();
        }

        AuthoringValidationResult ValidateDiagnosticArray(
            const yyjson::value& arr)
        {
            auto arr_opt = arr.as_array();
            if (!arr_opt)
            {
                return Fail("invalidDiagnostics", "diagnostics must be array");
            }
            const auto& arr_view = arr_opt.value();
            for (const auto& diag : arr_view)
            {
                if (auto r = ValidateDiagnostic(diag); !r.valid)
                {
                    return r;
                }
            }
            return Pass();
        }
    } // namespace

    AuthoringValidationResult ValidateAuthoringDocument(
        const yyjson::value& document)
    {
        auto obj_opt = document.as_object();
        if (!obj_opt)
        {
            return Fail("invalidDocument", "AuthoringDocument must be object");
        }

        const auto& obj = obj_opt.value();
        if (!HasIntegerField(obj, "contractVersion"))
        {
            return Fail(
                "missingField",
                "AuthoringDocument.contractVersion required");
        }
        if (!HasStringField(obj, "kind"))
        {
            return Fail("missingField", "AuthoringDocument.kind required");
        }
        auto kind = obj["kind"].as_string().value();
        if (!IsOneOf(kind, DOCUMENT_KINDS))
        {
            return Fail(
                "unsupportedKind",
                "AuthoringDocument.kind unsupported");
        }
        if (!HasIntegerField(obj, "revision"))
        {
            return Fail("missingField", "AuthoringDocument.revision required");
        }

        return Pass();
    }

    AuthoringValidationResult ValidateAuthoringChange(
        const yyjson::value& change)
    {
        auto obj_opt = change.as_object();
        if (!obj_opt)
        {
            return Fail("invalidChange", "AuthoringChange must be object");
        }

        const auto& obj = obj_opt.value();
        if (!HasIntegerField(obj, "baseRevision"))
        {
            return Fail(
                "missingField",
                "AuthoringChange.baseRevision required");
        }
        if (!HasStringField(obj, "kind"))
        {
            return Fail("missingField", "AuthoringChange.kind required");
        }

        auto kind = obj["kind"].as_string().value();
        if (!IsOneOf(kind, CHANGE_KINDS))
        {
            return Fail(
                "unsupportedChange",
                "AuthoringChange.kind unsupported");
        }
        if (!HasObjectField(obj, "payload"))
        {
            return Fail("missingField", "AuthoringChange.payload required");
        }

        return Pass();
    }

    AuthoringValidationResult ValidateAuthoringGetResult(
        const yyjson::value& result)
    {
        auto obj_opt = result.as_object();
        if (!obj_opt)
        {
            return Fail("invalidResult", "AuthoringGetResult must be object");
        }

        const auto& obj = obj_opt.value();
        if (obj.contains("document"))
        {
            const auto& doc = obj["document"];
            if (!doc.is_object())
            {
                return Fail(
                    "invalidDocument",
                    "AuthoringGetResult.document must be object");
            }
        }
        if (!HasArrayField(obj, "diagnostics"))
        {
            return Fail(
                "missingField",
                "AuthoringGetResult.diagnostics required");
        }
        if (auto d = ValidateDiagnosticArray(obj["diagnostics"]); !d.valid)
        {
            return d;
        }

        for (auto [key, value] : obj)
        {
            if (key != std::string_view("document")
                && key != std::string_view("diagnostics"))
            {
                return Fail(
                    "unknownField",
                    "AuthoringGetResult rejects unknown field");
            }
        }
        return Pass();
    }

    AuthoringValidationResult ValidateAuthoringApplyResult(
        const yyjson::value& result,
        int64_t              baseRevision)
    {
        auto obj_opt = result.as_object();
        if (!obj_opt)
        {
            return Fail("invalidResult", "AuthoringApplyResult must be object");
        }

        const auto& obj = obj_opt.value();
        if (!obj.contains("acceptedProperties")
            || obj["acceptedProperties"].is_null())
        {
            return Fail(
                "missingField",
                "AuthoringApplyResult.acceptedProperties required");
        }
        auto doc_opt = obj["document"].as_object();
        if (!doc_opt)
        {
            return Fail(
                "missingField",
                "AuthoringApplyResult.document required");
        }
        if (!HasArrayField(obj, "diagnostics"))
        {
            return Fail(
                "missingField",
                "AuthoringApplyResult.diagnostics required");
        }

        const auto& doc = doc_opt.value();
        if (!HasIntegerField(doc, "revision"))
        {
            return Fail(
                "missingField",
                "AuthoringApplyResult.document.revision required");
        }
        const auto&   revision_value = doc["revision"];
        const int64_t actual_revision =
            revision_value.is_sint()
                ? revision_value.as_sint().value()
                : static_cast<int64_t>(revision_value.as_uint().value());
        if (actual_revision != baseRevision + 1)
        {
            return Fail(
                "revisionMismatch",
                "AuthoringApplyResult.document.revision must equal "
                "baseRevision + 1");
        }

        if (auto d = ValidateDiagnosticArray(obj["diagnostics"]); !d.valid)
        {
            return d;
        }

        for (auto [key, value] : obj)
        {
            if (key != std::string_view("acceptedProperties")
                && key != std::string_view("document")
                && key != std::string_view("diagnostics"))
            {
                return Fail(
                    "unknownField",
                    "AuthoringApplyResult rejects unknown field");
            }
        }
        return Pass();
    }

    AuthoringValidationResult ValidateAuthoringCompileResult(
        const yyjson::value& result)
    {
        auto obj_opt = result.as_object();
        if (!obj_opt)
        {
            return Fail(
                "invalidResult",
                "AuthoringCompileResult must be object");
        }

        const auto& obj = obj_opt.value();
        if (!obj.contains("compile") || obj["compile"].is_null())
        {
            return Fail(
                "missingField",
                "AuthoringCompileResult.compile required");
        }
        if (!HasArrayField(obj, "diagnostics"))
        {
            return Fail(
                "missingField",
                "AuthoringCompileResult.diagnostics required");
        }
        if (auto d = ValidateDiagnosticArray(obj["diagnostics"]); !d.valid)
        {
            return d;
        }

        for (auto [key, value] : obj)
        {
            if (key != std::string_view("compile")
                && key != std::string_view("diagnostics"))
            {
                return Fail(
                    "unknownField",
                    "AuthoringCompileResult rejects unknown field");
            }
        }
        return Pass();
    }

    yyjson::value MakeAuthoringError(
        std::string_view error_kind,
        std::string_view message)
    {
        auto error = Das::Utils::MakeYyjsonObject();
        auto obj = *error.as_object();
        obj[std::string_view("errorKind")] = error_kind;
        obj[std::string_view("message")] = message;
        return error;
    }

} // namespace Das::Core::TaskScheduler
