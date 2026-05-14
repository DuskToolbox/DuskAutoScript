#include <das/Core/TaskScheduler/TaskAuthoringContract.h>

#include <das/Utils/DasJsonCore.h>

#include <array>
#include <string_view>

namespace Das::Core::TaskScheduler
{
    namespace
    {
        constexpr std::array<std::string_view, 2> kDocumentKinds{
            "formSequence",
            "graph"};

        constexpr std::array<std::string_view, 7> kChangeKinds{
            "setValue",
            "applyPreset",
            "addSequenceItem",
            "moveSequenceItem",
            "addNode",
            "connectPorts",
            "updateNodeConfig"};

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
        if (!HasIntegerField(obj, "version"))
        {
            return Fail("missingField", "AuthoringDocument.version required");
        }
        if (!HasStringField(obj, "kind"))
        {
            return Fail("missingField", "AuthoringDocument.kind required");
        }
        auto kind = obj["kind"].as_string().value();
        if (!IsOneOf(kind, kDocumentKinds))
        {
            return Fail(
                "unsupportedKind",
                "AuthoringDocument.kind unsupported");
        }
        if (!HasIntegerField(obj, "revision"))
        {
            return Fail("missingField", "AuthoringDocument.revision required");
        }
        if (!HasObjectField(obj, "values"))
        {
            return Fail("missingField", "AuthoringDocument.values required");
        }
        if (!HasObjectField(obj, "view"))
        {
            return Fail("missingField", "AuthoringDocument.view required");
        }
        if (!HasObjectField(obj, "schema"))
        {
            return Fail("missingField", "AuthoringDocument.schema required");
        }
        if (!HasObjectField(obj, "catalog"))
        {
            return Fail("missingField", "AuthoringDocument.catalog required");
        }
        if (!HasObjectField(obj, "state"))
        {
            return Fail("missingField", "AuthoringDocument.state required");
        }
        if (!HasArrayField(obj, "diagnostics"))
        {
            return Fail(
                "missingField",
                "AuthoringDocument.diagnostics required");
        }
        if (!HasObjectField(obj, "migration"))
        {
            return Fail("missingField", "AuthoringDocument.migration required");
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
        if (!IsOneOf(kind, kChangeKinds))
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
