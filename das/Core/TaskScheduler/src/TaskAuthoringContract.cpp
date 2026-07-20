#include <das/Core/TaskScheduler/TaskAuthoringContract.h>

#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
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
            "invokeCommand",
            "convertView",
            "addFormSequenceItem",
            "moveFormSequenceItem",
            "removeFormSequenceItem",
            "addGraphNode",
            "removeGraphNode",
            "connectGraphPorts",
            "disconnectGraphPorts"};

        constexpr std::array<int, 7> VALUE_TYPES{0, 1, 2, 4, 8, 16, 32};

        constexpr std::array<int, 7> INPUT_TYPES{0, 1, 2, 3, 4, 5, 6};

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

        bool IntInSet(int64_t value, const auto& allowed) noexcept
        {
            return std::ranges::find(allowed, value) != allowed.end();
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

        bool HasBoolField(const auto& obj, std::string_view key)
        {
            return obj.contains(key) && obj[key].is_bool();
        }

        bool HasIntegerField(const auto& obj, std::string_view key)
        {
            return obj.contains(key)
                   && (obj[key].is_sint() || obj[key].is_uint());
        }

        int64_t ReadInt(const auto& value) noexcept
        {
            return value.is_sint()
                       ? value.as_sint().value()
                       : static_cast<int64_t>(value.as_uint().value());
        }

        bool IsAlpha(char c) noexcept
        {
            return std::isalpha(static_cast<unsigned char>(c)) != 0;
        }

        bool IsAbsoluteLocalPath(std::string_view s) noexcept
        {
            if (s.empty())
            {
                return false;
            }
            // Windows drive letter: "X:/..." or "X:\..."
            if (s.size() >= 3 && IsAlpha(s[0]) && s[1] == ':'
                && (s[2] == '/' || s[2] == '\\'))
            {
                return true;
            }
            // POSIX absolute path: "/..."
            if (s[0] == '/')
            {
                return true;
            }
            // UNC: "\\\\..."
            if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\')
            {
                return true;
            }
            return false;
        }

        bool IsValidIconUri(std::string_view s) noexcept
        {
            if (s.starts_with("http://") || s.starts_with("https://"))
            {
                return true;
            }
            if (s.starts_with("data:"))
            {
                return false;
            }
            return IsAbsoluteLocalPath(s);
        }

        AuthoringValidationResult Pass() { return {true, {}, {}}; }

        AuthoringValidationResult Fail(
            std::string error_kind,
            std::string message)
        {
            return {false, std::move(error_kind), std::move(message)};
        }

        AuthoringValidationResult ValidateOptionalIcon(
            const auto&      obj,
            std::string_view key)
        {
            if (!obj.contains(key))
            {
                return Pass();
            }
            const auto& v = obj[key];
            if (!v.is_string())
            {
                return Fail("invalidIcon", "icon must be string");
            }
            auto sv = v.as_string().value();
            if (!IsValidIconUri(sv))
            {
                return Fail(
                    "invalidIcon",
                    "icon must be absolute local path or http(s) URL");
            }
            return Pass();
        }

        bool OnlyKeys(
            const auto&                                   obj,
            const std::initializer_list<std::string_view> allowed)
        {
            for (auto [k, v] : obj)
            {
                (void)v;
                bool found = false;
                for (auto allowed_key : allowed)
                {
                    if (k == allowed_key)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return false;
                }
            }
            return true;
        }

        // Forward declarations — defined after the structural helpers but
        // referenced by FieldDescriptor/FieldContainer because commands[] can
        // appear at every FieldContainer level (D-84-14/D-84-26).
        AuthoringValidationResult ValidateCommandDescriptorArray(
            const yyjson::value& arr);

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

            if (!OnlyKeys(obj, {"severity", "message", "code", "path"}))
            {
                return Fail(
                    "unknownDiagnosticField",
                    "Diagnostic rejects unknown field");
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

        AuthoringValidationResult ValidateValidation(
            const yyjson::value& validation)
        {
            auto obj_opt = validation.as_object();
            if (!obj_opt)
            {
                return Fail("invalidValidation", "validation must be object");
            }
            const auto& obj = obj_opt.value();
            if (!HasStringField(obj, "pattern"))
            {
                return Fail("missingField", "validation.pattern required");
            }
            if (obj["pattern"].as_string().value().empty())
            {
                return Fail(
                    "invalidValidationPattern",
                    "validation.pattern must be non-empty");
            }
            if (!HasStringField(obj, "message"))
            {
                return Fail("missingField", "validation.message required");
            }
            if (obj["message"].as_string().value().empty())
            {
                return Fail(
                    "invalidValidationMessage",
                    "validation.message must be non-empty");
            }
            if (!OnlyKeys(obj, {"pattern", "message"}))
            {
                return Fail("unknownField", "validation rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateCandidate(
            const yyjson::value& candidate)
        {
            auto obj_opt = candidate.as_object();
            if (!obj_opt)
            {
                return Fail("invalidCandidate", "Candidate must be object");
            }
            const auto& obj = obj_opt.value();
            if (!obj.contains("value"))
            {
                return Fail("missingField", "Candidate.value required");
            }
            const auto& v = obj["value"];
            // value must be non-null JSON scalar (bool/number/string);
            // objects, arrays and null are rejected per D-84-21.
            if (v.is_null() || v.is_object() || v.is_array())
            {
                return Fail(
                    "invalidCandidateValue",
                    "Candidate.value must be non-null scalar");
            }
            if (!HasStringField(obj, "label"))
            {
                return Fail("missingField", "Candidate.label required");
            }
            if (obj["label"].as_string().value().empty())
            {
                return Fail(
                    "invalidCandidateLabel",
                    "Candidate.label must be non-empty");
            }
            if (auto r = ValidateOptionalIcon(obj, "icon"); !r.valid)
            {
                return r;
            }
            if (!OnlyKeys(obj, {"value", "label", "icon"}))
            {
                return Fail("unknownField", "Candidate rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateFieldDescriptor(
            const yyjson::value&   field,
            std::set<std::string>& seen_names)
        {
            auto obj_opt = field.as_object();
            if (!obj_opt)
            {
                return Fail("invalidField", "FieldDescriptor must be object");
            }
            const auto& obj = obj_opt.value();

            if (!HasStringField(obj, "name"))
            {
                return Fail("missingField", "FieldDescriptor.name required");
            }
            const auto name = obj["name"].as_string().value();
            if (name.empty())
            {
                return Fail(
                    "invalidFieldName",
                    "FieldDescriptor.name must be non-empty");
            }
            if (!seen_names.insert(std::string(name)).second)
            {
                return Fail(
                    "duplicateFieldName",
                    "FieldDescriptor.name duplicate");
            }

            if (!HasStringField(obj, "label"))
            {
                return Fail("missingField", "FieldDescriptor.label required");
            }
            if (obj["label"].as_string().value().empty())
            {
                return Fail(
                    "invalidFieldLabel",
                    "FieldDescriptor.label must be non-empty");
            }

            if (!HasIntegerField(obj, "valueType"))
            {
                return Fail(
                    "missingField",
                    "FieldDescriptor.valueType required");
            }
            const auto vt = ReadInt(obj["valueType"]);
            if (!IntInSet(vt, VALUE_TYPES))
            {
                return Fail(
                    "invalidValueType",
                    "FieldDescriptor.valueType unsupported");
            }

            if (!HasIntegerField(obj, "inputType"))
            {
                return Fail(
                    "missingField",
                    "FieldDescriptor.inputType required");
            }
            const auto it = ReadInt(obj["inputType"]);
            if (!IntInSet(it, INPUT_TYPES))
            {
                return Fail(
                    "invalidInputType",
                    "FieldDescriptor.inputType unsupported");
            }

            if (!obj.contains("value"))
            {
                return Fail("missingField", "FieldDescriptor.value required");
            }

            if (obj.contains("description") && !obj["description"].is_string())
            {
                return Fail(
                    "invalidDescription",
                    "FieldDescriptor.description must be string");
            }

            if (obj.contains("required") && !obj["required"].is_bool())
            {
                return Fail(
                    "invalidRequired",
                    "FieldDescriptor.required must be bool");
            }
            if (obj.contains("readonly") && !obj["readonly"].is_bool())
            {
                return Fail(
                    "invalidReadonly",
                    "FieldDescriptor.readonly must be bool");
            }

            if (obj.contains("candidates"))
            {
                if (it != 1 && it != 2)
                {
                    return Fail(
                        "candidatesRequireSelect",
                        "candidates only valid for select/multiselect");
                }
                auto carr_opt = obj["candidates"].as_array();
                if (!carr_opt)
                {
                    return Fail(
                        "invalidCandidates",
                        "candidates must be array");
                }
                for (const auto& c : carr_opt.value())
                {
                    if (auto r = ValidateCandidate(c); !r.valid)
                    {
                        return r;
                    }
                }
            }

            if (obj.contains("validation"))
            {
                if (auto r = ValidateValidation(obj["validation"]); !r.valid)
                {
                    return r;
                }
            }

            if (auto r = ValidateOptionalIcon(obj, "icon"); !r.valid)
            {
                return r;
            }

            if (obj.contains("commands"))
            {
                if (auto r = ValidateCommandDescriptorArray(obj["commands"]);
                    !r.valid)
                {
                    return r;
                }
            }

            if (!OnlyKeys(
                    obj,
                    {"name",
                     "label",
                     "valueType",
                     "inputType",
                     "value",
                     "description",
                     "required",
                     "readonly",
                     "candidates",
                     "validation",
                     "icon",
                     "commands"}))
            {
                return Fail(
                    "unknownField",
                    "FieldDescriptor rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateFieldDescriptorArray(
            const yyjson::value&   arr,
            std::set<std::string>& seen_names)
        {
            auto arr_opt = arr.as_array();
            if (!arr_opt)
            {
                return Fail("invalidFields", "fields must be array");
            }
            for (const auto& f : arr_opt.value())
            {
                if (auto r = ValidateFieldDescriptor(f, seen_names); !r.valid)
                {
                    return r;
                }
            }
            return Pass();
        }

        AuthoringValidationResult ValidateFieldLayoutTree(
            const yyjson::value&         node,
            const std::set<std::string>& field_names,
            std::set<std::string>&       referenced_names,
            std::set<std::string>&       section_ids)
        {
            auto obj_opt = node.as_object();
            if (!obj_opt)
            {
                return Fail("invalidLayoutNode", "layout node must be object");
            }
            const auto& obj = obj_opt.value();

            if (obj.contains("fieldName"))
            {
                if (!obj["fieldName"].is_string())
                {
                    return Fail(
                        "invalidLayoutLeaf",
                        "fieldName must be string");
                }
                const auto fname = obj["fieldName"].as_string().value();
                if (fname.empty())
                {
                    return Fail(
                        "invalidLayoutLeaf",
                        "fieldName must be non-empty");
                }
                if (field_names.count(std::string(fname)) == 0)
                {
                    return Fail(
                        "layoutFieldNotInContainer",
                        "fieldName not in container");
                }
                if (!referenced_names.insert(std::string(fname)).second)
                {
                    return Fail(
                        "layoutDuplicateFieldRef",
                        "fieldName referenced multiple times");
                }
                if (!OnlyKeys(obj, {"fieldName"}))
                {
                    return Fail(
                        "unknownField",
                        "layout leaf rejects unknown field");
                }
                return Pass();
            }

            if (obj.contains("entries"))
            {
                if (!HasStringField(obj, "id"))
                {
                    return Fail("missingField", "section.id required");
                }
                const auto sid = obj["id"].as_string().value();
                if (sid.empty())
                {
                    return Fail(
                        "invalidSectionId",
                        "section.id must be non-empty");
                }
                if (!section_ids.insert(std::string(sid)).second)
                {
                    return Fail(
                        "duplicateSectionId",
                        "section.id duplicate within layout tree");
                }
                if (!HasStringField(obj, "label"))
                {
                    return Fail("missingField", "section.label required");
                }
                if (obj["label"].as_string().value().empty())
                {
                    return Fail(
                        "invalidSectionLabel",
                        "section.label must be non-empty");
                }
                if (obj.contains("description")
                    && !obj["description"].is_string())
                {
                    return Fail(
                        "invalidSectionDescription",
                        "section.description must be string");
                }
                if (auto r = ValidateOptionalIcon(obj, "icon"); !r.valid)
                {
                    return r;
                }
                if (obj.contains("defaultExpanded")
                    && !obj["defaultExpanded"].is_bool())
                {
                    return Fail(
                        "invalidDefaultExpanded",
                        "defaultExpanded must be bool");
                }
                auto entries_opt = obj["entries"].as_array();
                if (!entries_opt)
                {
                    return Fail(
                        "invalidEntries",
                        "section.entries must be array");
                }
                for (const auto& child : entries_opt.value())
                {
                    if (auto r = ValidateFieldLayoutTree(
                            child,
                            field_names,
                            referenced_names,
                            section_ids);
                        !r.valid)
                    {
                        return r;
                    }
                }
                if (!OnlyKeys(
                        obj,
                        {"id",
                         "label",
                         "description",
                         "icon",
                         "defaultExpanded",
                         "entries"}))
                {
                    return Fail(
                        "unknownField",
                        "layout section rejects unknown field");
                }
                return Pass();
            }

            return Fail(
                "invalidLayoutNode",
                "layout node must be leaf {fieldName} or section "
                "{id,label,entries}");
        }

        AuthoringValidationResult ValidateFieldLayout(
            const yyjson::value&         layout,
            const std::set<std::string>& field_names)
        {
            auto arr_opt = layout.as_array();
            if (!arr_opt)
            {
                return Fail("invalidLayout", "fieldLayout must be array");
            }
            std::set<std::string> referenced_names;
            std::set<std::string> section_ids;
            for (const auto& node : arr_opt.value())
            {
                if (auto r = ValidateFieldLayoutTree(
                        node,
                        field_names,
                        referenced_names,
                        section_ids);
                    !r.valid)
                {
                    return r;
                }
            }
            if (referenced_names.size() != field_names.size())
            {
                return Fail(
                    "layoutMissingField",
                    "fieldLayout must reference every field exactly once");
            }
            return Pass();
        }

        // Validate the apply template portion of a CommandDescriptor. The
        // template has the same {kind, payload} shape as AuthoringChange but
        // must NOT carry baseRevision; the caller injects the current
        // revision at submit time.
        AuthoringValidationResult ValidateCommandApplyTemplate(
            const yyjson::value& apply)
        {
            auto obj_opt = apply.as_object();
            if (!obj_opt)
            {
                return Fail("invalidApplyTemplate", "apply must be object");
            }
            const auto& obj = obj_opt.value();
            if (obj.contains("baseRevision"))
            {
                return Fail(
                    "applyTemplateHasBaseRevision",
                    "apply template must not contain baseRevision");
            }
            if (!HasStringField(obj, "kind"))
            {
                return Fail("missingField", "apply.kind required");
            }
            if (!HasObjectField(obj, "payload"))
            {
                return Fail("missingField", "apply.payload required");
            }
            if (!OnlyKeys(obj, {"kind", "payload"}))
            {
                return Fail(
                    "unknownField",
                    "apply template rejects unknown field");
            }
            return Pass();
        }

        // D-84-26/D-84-27: CommandDescriptor strict shape. Must contain
        // exactly one of apply|openView; optional confirmation only allowed
        // with apply; openView restricted to the built-in actionCatalog
        // enumeration for Phase 84.
        AuthoringValidationResult ValidateCommandDescriptor(
            const yyjson::value&   cmd,
            std::set<std::string>& seen_ids)
        {
            auto obj_opt = cmd.as_object();
            if (!obj_opt)
            {
                return Fail(
                    "invalidCommand",
                    "CommandDescriptor must be object");
            }
            const auto& obj = obj_opt.value();

            if (!HasStringField(obj, "id"))
            {
                return Fail("missingField", "CommandDescriptor.id required");
            }
            const auto id = obj["id"].as_string().value();
            if (id.empty())
            {
                return Fail(
                    "invalidCommandId",
                    "CommandDescriptor.id must be non-empty");
            }
            if (!seen_ids.insert(std::string(id)).second)
            {
                return Fail(
                    "duplicateCommandId",
                    "CommandDescriptor.id duplicate within container");
            }

            if (!HasStringField(obj, "label"))
            {
                return Fail("missingField", "CommandDescriptor.label required");
            }
            if (obj["label"].as_string().value().empty())
            {
                return Fail(
                    "invalidCommandLabel",
                    "CommandDescriptor.label must be non-empty");
            }

            if (obj.contains("description") && !obj["description"].is_string())
            {
                return Fail(
                    "invalidDescription",
                    "CommandDescriptor.description must be string");
            }

            if (auto r = ValidateOptionalIcon(obj, "icon"); !r.valid)
            {
                return r;
            }

            const bool has_apply = obj.contains("apply");
            const bool has_open_view = obj.contains("openView");
            if (has_apply == has_open_view)
            {
                return Fail(
                    "commandActionXorRequired",
                    "CommandDescriptor must have exactly one of apply|openView");
            }

            if (obj.contains("confirmation"))
            {
                if (!has_apply)
                {
                    return Fail(
                        "confirmationRequiresApply",
                        "confirmation only allowed with apply");
                }
                if (!HasStringField(obj, "confirmation"))
                {
                    return Fail(
                        "invalidConfirmation",
                        "confirmation must be string");
                }
                if (obj["confirmation"].as_string().value().empty())
                {
                    return Fail(
                        "invalidConfirmation",
                        "confirmation must be non-empty");
                }
            }

            if (has_apply)
            {
                if (auto r = ValidateCommandApplyTemplate(obj["apply"]);
                    !r.valid)
                {
                    return r;
                }
            }
            else
            {
                if (!HasStringField(obj, "openView"))
                {
                    return Fail("invalidOpenView", "openView must be string");
                }
                const auto ov = obj["openView"].as_string().value();
                if (ov != std::string_view("actionCatalog"))
                {
                    return Fail(
                        "unsupportedOpenView",
                        "openView must be actionCatalog");
                }
            }

            if (!OnlyKeys(
                    obj,
                    {"id",
                     "label",
                     "description",
                     "icon",
                     "confirmation",
                     "apply",
                     "openView"}))
            {
                return Fail(
                    "unknownField",
                    "CommandDescriptor rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateCommandDescriptorArray(
            const yyjson::value& arr)
        {
            auto arr_opt = arr.as_array();
            if (!arr_opt)
            {
                return Fail("invalidCommands", "commands must be array");
            }
            std::set<std::string> seen_ids;
            for (const auto& cmd : arr_opt.value())
            {
                if (auto r = ValidateCommandDescriptor(cmd, seen_ids); !r.valid)
                {
                    return r;
                }
            }
            return Pass();
        }

        // Validate a FieldContainer (document root, FormSequenceItem, or
        // GraphNode): fields[], fieldLayout[], commands[]. Fills field_names
        // so the caller can apply additional constraints (e.g. enabled
        // collision check).
        AuthoringValidationResult ValidateFieldContainer(
            const auto&            obj,
            std::set<std::string>& field_names)
        {
            if (obj.contains("fields"))
            {
                if (auto r = ValidateFieldDescriptorArray(
                        obj["fields"],
                        field_names);
                    !r.valid)
                {
                    return r;
                }
            }
            if (obj.contains("fieldLayout"))
            {
                if (auto r =
                        ValidateFieldLayout(obj["fieldLayout"], field_names);
                    !r.valid)
                {
                    return r;
                }
            }
            if (obj.contains("commands"))
            {
                if (auto r = ValidateCommandDescriptorArray(obj["commands"]);
                    !r.valid)
                {
                    return r;
                }
            }
            return Pass();
        }

        AuthoringValidationResult ValidateEntryRef(const yyjson::value& ref)
        {
            auto obj_opt = ref.as_object();
            if (!obj_opt)
            {
                return Fail("invalidEntryRef", "entryRef must be object");
            }
            const auto& obj = obj_opt.value();
            if (!HasIntegerField(obj, "entryId"))
            {
                return Fail("missingField", "entryRef.entryId required");
            }
            if (!OnlyKeys(obj, {"entryId"}))
            {
                return Fail("unknownField", "entryRef rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateTarget(
            const auto& obj,
            bool        allow_entry_ref)
        {
            if (!HasStringField(obj, "targetKind"))
            {
                return Fail("invalidTargetKind", "targetKind must be string");
            }
            const auto tk = obj["targetKind"].as_string().value();
            if (tk == std::string_view("componentRef"))
            {
                return Pass();
            }
            if (tk == std::string_view("entryRef"))
            {
                if (!allow_entry_ref)
                {
                    return Fail(
                        "invalidTargetKind",
                        "targetKind=entryRef not allowed here");
                }
                if (!obj.contains("entryRef"))
                {
                    return Fail(
                        "missingField",
                        "entryRef required when targetKind=entryRef");
                }
                return ValidateEntryRef(obj["entryRef"]);
            }
            return Fail(
                "invalidTargetKind",
                "targetKind must be componentRef|entryRef");
        }

        AuthoringValidationResult ValidateFormSequenceItem(
            const yyjson::value& item)
        {
            auto obj_opt = item.as_object();
            if (!obj_opt)
            {
                return Fail(
                    "invalidFormSequenceItem",
                    "FormSequenceItem must be object");
            }
            const auto& obj = obj_opt.value();

            if (!HasStringField(obj, "id"))
            {
                return Fail("missingField", "FormSequenceItem.id required");
            }
            if (obj["id"].as_string().value().empty())
            {
                return Fail(
                    "invalidItemId",
                    "FormSequenceItem.id must be non-empty");
            }

            if (obj.contains("componentGuid")
                && !obj["componentGuid"].is_string())
            {
                return Fail(
                    "invalidComponentGuid",
                    "componentGuid must be string");
            }

            if (obj.contains("targetKind"))
            {
                if (auto r = ValidateTarget(obj, /*allow_entry_ref=*/true);
                    !r.valid)
                {
                    return r;
                }
            }

            if (obj.contains("enabled") && !obj["enabled"].is_bool())
            {
                return Fail("invalidEnabled", "enabled must be bool");
            }

            if (obj.contains("debugInfo") && !obj["debugInfo"].is_object())
            {
                return Fail("invalidDebugInfo", "debugInfo must be object");
            }

            std::set<std::string> field_names;
            if (auto r = ValidateFieldContainer(obj, field_names); !r.valid)
            {
                return r;
            }

            if (obj.contains("enabled") && field_names.count("enabled") > 0)
            {
                return Fail(
                    "duplicateEnabledField",
                    "fields[] must not declare 'enabled' when builtin enabled "
                    "is present");
            }

            if (!OnlyKeys(
                    obj,
                    {"id",
                     "componentGuid",
                     "targetKind",
                     "entryRef",
                     "fields",
                     "fieldLayout",
                     "commands",
                     "enabled",
                     "debugInfo"}))
            {
                return Fail(
                    "unknownField",
                    "FormSequenceItem rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateGraphNode(const yyjson::value& node)
        {
            auto obj_opt = node.as_object();
            if (!obj_opt)
            {
                return Fail("invalidGraphNode", "GraphNode must be object");
            }
            const auto& obj = obj_opt.value();

            if (!HasStringField(obj, "id"))
            {
                return Fail("missingField", "GraphNode.id required");
            }
            if (obj["id"].as_string().value().empty())
            {
                return Fail("invalidNodeId", "GraphNode.id must be non-empty");
            }

            if (obj.contains("componentGuid")
                && !obj["componentGuid"].is_string())
            {
                return Fail(
                    "invalidComponentGuid",
                    "componentGuid must be string");
            }

            if (obj.contains("targetKind"))
            {
                if (auto r = ValidateTarget(obj, /*allow_entry_ref=*/true);
                    !r.valid)
                {
                    return r;
                }
            }

            if (obj.contains("debugInfo") && !obj["debugInfo"].is_object())
            {
                return Fail("invalidDebugInfo", "debugInfo must be object");
            }

            std::set<std::string> field_names;
            if (auto r = ValidateFieldContainer(obj, field_names); !r.valid)
            {
                return r;
            }

            if (!OnlyKeys(
                    obj,
                    {"id",
                     "componentGuid",
                     "targetKind",
                     "entryRef",
                     "fields",
                     "fieldLayout",
                     "commands",
                     "debugInfo"}))
            {
                return Fail("unknownField", "GraphNode rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateFormSequence(const yyjson::value& fs)
        {
            auto obj_opt = fs.as_object();
            if (!obj_opt)
            {
                return Fail(
                    "invalidFormSequence",
                    "formSequence must be object");
            }
            const auto& obj = obj_opt.value();
            // Reject the removed legacy `sequence` alias explicitly so callers
            // migrating from the pre-Phase-84 wire see the structural error
            // before a generic "items missing" message.
            if (obj.contains("sequence"))
            {
                return Fail(
                    "unknownField",
                    "formSequence.sequence is removed; use items[]");
            }
            if (!HasArrayField(obj, "items"))
            {
                return Fail("missingField", "formSequence.items required");
            }
            std::set<std::string> ids;
            auto                  items_view = obj["items"].as_array().value();
            for (const auto& item : items_view)
            {
                if (auto r = ValidateFormSequenceItem(item); !r.valid)
                {
                    return r;
                }
                const auto id =
                    item.as_object().value()["id"].as_string().value();
                if (!ids.insert(std::string(id)).second)
                {
                    return Fail(
                        "duplicateItemId",
                        "FormSequenceItem.id duplicate");
                }
            }
            if (!OnlyKeys(obj, {"items"}))
            {
                return Fail(
                    "unknownField",
                    "formSequence rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateGraphDocument(const yyjson::value& gd)
        {
            auto obj_opt = gd.as_object();
            if (!obj_opt)
            {
                return Fail("invalidGraph", "graph must be object");
            }
            const auto& obj = obj_opt.value();
            if (obj.contains("nodes"))
            {
                if (!obj["nodes"].is_array())
                {
                    return Fail("invalidNodes", "graph.nodes must be array");
                }
                std::set<std::string> ids;
                auto nodes_view = obj["nodes"].as_array().value();
                for (const auto& node : nodes_view)
                {
                    if (auto r = ValidateGraphNode(node); !r.valid)
                    {
                        return r;
                    }
                    const auto id =
                        node.as_object().value()["id"].as_string().value();
                    if (!ids.insert(std::string(id)).second)
                    {
                        return Fail(
                            "duplicateNodeId",
                            "GraphNode.id duplicate");
                    }
                }
            }
            // D-84-37: graph carries nodes[] and connections[]; the
            // connectGraphPorts/disconnectGraphPorts change kinds operate on
            // connection endpoints, so connections must survive across the
            // boundary. The connection shape is intentionally permissive here
            // (full endpoint validation lives in the change payload validator).
            if (obj.contains("connections"))
            {
                if (!obj["connections"].is_array())
                {
                    return Fail(
                        "invalidConnections",
                        "graph.connections must be array");
                }
            }
            if (!OnlyKeys(obj, {"nodes", "connections"}))
            {
                return Fail("unknownField", "graph rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateActionCatalog(const yyjson::value& ac)
        {
            auto obj_opt = ac.as_object();
            if (!obj_opt)
            {
                return Fail(
                    "invalidActionCatalog",
                    "actionCatalog must be object");
            }
            const auto& obj = obj_opt.value();
            if (!HasArrayField(obj, "entries"))
            {
                return Fail("missingField", "actionCatalog.entries required");
            }

            std::set<std::string> action_ids;
            auto entries_view = obj["entries"].as_array().value();
            for (const auto& e : entries_view)
            {
                auto eo_opt = e.as_object();
                if (!eo_opt)
                {
                    return Fail(
                        "invalidActionCatalogEntry",
                        "entry must be object");
                }
                const auto& eo = eo_opt.value();
                if (!HasStringField(eo, "actionId"))
                {
                    return Fail("missingField", "entry.actionId required");
                }
                if (eo["actionId"].as_string().value().empty())
                {
                    return Fail(
                        "invalidActionId",
                        "actionId must be non-empty");
                }
                if (!HasStringField(eo, "label"))
                {
                    return Fail("missingField", "entry.label required");
                }
                if (eo["label"].as_string().value().empty())
                {
                    return Fail(
                        "invalidEntryLabel",
                        "entry.label must be non-empty");
                }
                if (eo.contains("description")
                    && !eo["description"].is_string())
                {
                    return Fail(
                        "invalidEntryDescription",
                        "entry.description must be string");
                }
                if (auto r = ValidateOptionalIcon(eo, "icon"); !r.valid)
                {
                    return r;
                }
                if (!action_ids
                         .insert(
                             std::string(eo["actionId"].as_string().value()))
                         .second)
                {
                    return Fail("duplicateActionId", "actionId duplicate");
                }
                if (!OnlyKeys(eo, {"actionId", "label", "description", "icon"}))
                {
                    return Fail("unknownField", "entry rejects unknown field");
                }
            }

            if (obj.contains("groups"))
            {
                if (!obj["groups"].is_array())
                {
                    return Fail("invalidGroups", "groups must be array");
                }
                std::set<std::string> group_ids;
                auto groups_view = obj["groups"].as_array().value();
                for (const auto& g : groups_view)
                {
                    auto go_opt = g.as_object();
                    if (!go_opt)
                    {
                        return Fail(
                            "invalidActionCatalogGroup",
                            "group must be object");
                    }
                    const auto& go = go_opt.value();
                    if (!HasStringField(go, "id"))
                    {
                        return Fail("missingField", "group.id required");
                    }
                    if (go["id"].as_string().value().empty())
                    {
                        return Fail(
                            "invalidGroupId",
                            "group.id must be non-empty");
                    }
                    if (!group_ids
                             .insert(std::string(go["id"].as_string().value()))
                             .second)
                    {
                        return Fail("duplicateGroupId", "group.id duplicate");
                    }
                    if (!HasStringField(go, "label"))
                    {
                        return Fail("missingField", "group.label required");
                    }
                    if (go["label"].as_string().value().empty())
                    {
                        return Fail(
                            "invalidGroupLabel",
                            "group.label must be non-empty");
                    }
                    if (go.contains("description")
                        && !go["description"].is_string())
                    {
                        return Fail(
                            "invalidGroupDescription",
                            "group.description must be string");
                    }
                    if (auto r = ValidateOptionalIcon(go, "icon"); !r.valid)
                    {
                        return r;
                    }
                    if (!HasArrayField(go, "actionIds"))
                    {
                        return Fail("missingField", "group.actionIds required");
                    }
                    auto action_ids_view = go["actionIds"].as_array().value();
                    for (const auto& aid : action_ids_view)
                    {
                        if (!aid.is_string())
                        {
                            return Fail(
                                "invalidActionId",
                                "group.actionIds must be strings");
                        }
                        if (action_ids.count(
                                std::string(aid.as_string().value()))
                            == 0)
                        {
                            return Fail(
                                "groupActionIdNotFound",
                                "group.actionIds must reference existing "
                                "entry");
                        }
                    }
                    if (!OnlyKeys(
                            go,
                            {"id",
                             "label",
                             "description",
                             "icon",
                             "actionIds"}))
                    {
                        return Fail(
                            "unknownField",
                            "group rejects unknown field");
                    }
                }
            }

            if (!OnlyKeys(obj, {"entries", "groups"}))
            {
                return Fail(
                    "unknownField",
                    "actionCatalog rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateSetValueTarget(
            const yyjson::value& target)
        {
            auto t_opt = target.as_object();
            if (!t_opt)
            {
                return Fail("invalidTarget", "target must be object");
            }
            const auto& t = t_opt.value();
            if (!HasStringField(t, "kind"))
            {
                return Fail("invalidTargetKind", "target.kind required");
            }
            const auto tkind = t["kind"].as_string().value();
            if (tkind == std::string_view("document"))
            {
                if (!OnlyKeys(t, {"kind"}))
                {
                    return Fail(
                        "unknownField",
                        "document target rejects unknown field");
                }
                return Pass();
            }
            if (tkind == std::string_view("formSequenceItem")
                || tkind == std::string_view("graphNode"))
            {
                if (!HasStringField(t, "id"))
                {
                    return Fail(
                        "missingField",
                        "target.id required for formSequenceItem/graphNode");
                }
                if (t["id"].as_string().value().empty())
                {
                    return Fail(
                        "invalidTargetId",
                        "target.id must be non-empty");
                }
                if (!OnlyKeys(t, {"kind", "id"}))
                {
                    return Fail("unknownField", "target rejects unknown field");
                }
                return Pass();
            }
            return Fail(
                "invalidTargetKind",
                "target.kind must be document|formSequenceItem|graphNode");
        }

        AuthoringValidationResult RequireNonEmptyString(
            const auto&      obj,
            std::string_view key,
            std::string_view error_kind,
            std::string_view message)
        {
            if (!HasStringField(obj, key))
            {
                return Fail("missingField", std::string{message});
            }
            if (obj[key].as_string().value().empty())
            {
                return Fail(std::string{error_kind}, std::string{message});
            }
            return Pass();
        }

        AuthoringValidationResult ValidateSetValuePayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (!HasObjectField(obj, "target"))
            {
                return Fail("missingField", "setValue.target required");
            }
            if (auto r = ValidateSetValueTarget(obj["target"]); !r.valid)
            {
                return r;
            }
            if (auto r = RequireNonEmptyString(
                    obj,
                    "fieldName",
                    "invalidFieldName",
                    "setValue.fieldName required and non-empty");
                !r.valid)
            {
                return r;
            }
            if (!obj.contains("value"))
            {
                return Fail("missingField", "setValue.value required");
            }
            if (!OnlyKeys(obj, {"target", "fieldName", "value"}))
            {
                return Fail(
                    "unknownField",
                    "setValue payload rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateInvokeCommandPayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (auto r = RequireNonEmptyString(
                    obj,
                    "commandId",
                    "invalidCommandId",
                    "invokeCommand.commandId required and non-empty");
                !r.valid)
            {
                return r;
            }
            if (!OnlyKeys(obj, {"commandId"}))
            {
                return Fail(
                    "unknownField",
                    "invokeCommand rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateConvertViewPayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (!HasStringField(obj, "targetViewKind"))
            {
                return Fail(
                    "missingField",
                    "convertView.targetViewKind required");
            }
            const auto tvk = obj["targetViewKind"].as_string().value();
            if (!IsOneOf(tvk, DOCUMENT_KINDS))
            {
                return Fail(
                    "unsupportedViewKind",
                    "convertView.targetViewKind unsupported");
            }
            if (!OnlyKeys(obj, {"targetViewKind"}))
            {
                return Fail(
                    "unknownField",
                    "convertView rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateAddFormSequenceItemPayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (auto r = RequireNonEmptyString(
                    obj,
                    "actionId",
                    "invalidActionId",
                    "addFormSequenceItem.actionId required and non-empty");
                !r.valid)
            {
                return r;
            }
            if (obj.contains("insertionIndex")
                && !HasIntegerField(obj, "insertionIndex"))
            {
                return Fail(
                    "invalidInsertionIndex",
                    "insertionIndex must be integer");
            }
            const bool has_index = obj.contains("insertionIndex");
            if (has_index)
            {
                if (!OnlyKeys(obj, {"actionId", "insertionIndex"}))
                {
                    return Fail(
                        "unknownField",
                        "addFormSequenceItem rejects unknown field");
                }
            }
            else
            {
                if (!OnlyKeys(obj, {"actionId"}))
                {
                    return Fail(
                        "unknownField",
                        "addFormSequenceItem rejects unknown field");
                }
            }
            return Pass();
        }

        AuthoringValidationResult ValidateMoveFormSequenceItemPayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (auto r = RequireNonEmptyString(
                    obj,
                    "formSequenceItemId",
                    "invalidFormSequenceItemId",
                    "moveFormSequenceItem.formSequenceItemId required");
                !r.valid)
            {
                return r;
            }
            if (!HasIntegerField(obj, "destinationIndex"))
            {
                return Fail(
                    "missingField",
                    "moveFormSequenceItem.destinationIndex required");
            }
            if (!OnlyKeys(obj, {"formSequenceItemId", "destinationIndex"}))
            {
                return Fail(
                    "unknownField",
                    "moveFormSequenceItem rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateRemoveFormSequenceItemPayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (auto r = RequireNonEmptyString(
                    obj,
                    "formSequenceItemId",
                    "invalidFormSequenceItemId",
                    "removeFormSequenceItem.formSequenceItemId required");
                !r.valid)
            {
                return r;
            }
            if (!OnlyKeys(obj, {"formSequenceItemId"}))
            {
                return Fail(
                    "unknownField",
                    "removeFormSequenceItem rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateAddGraphNodePayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (auto r = RequireNonEmptyString(
                    obj,
                    "actionId",
                    "invalidActionId",
                    "addGraphNode.actionId required and non-empty");
                !r.valid)
            {
                return r;
            }
            if (!OnlyKeys(obj, {"actionId"}))
            {
                return Fail(
                    "unknownField",
                    "addGraphNode rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateRemoveGraphNodePayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (auto r = RequireNonEmptyString(
                    obj,
                    "nodeId",
                    "invalidNodeId",
                    "removeGraphNode.nodeId required and non-empty");
                !r.valid)
            {
                return r;
            }
            if (!OnlyKeys(obj, {"nodeId"}))
            {
                return Fail(
                    "unknownField",
                    "removeGraphNode rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateGraphPortsPayload(
            const yyjson::value& payload,
            std::string_view     kind)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto&                       obj = obj_opt.value();
            static constexpr std::string_view kFields[] =
                {"fromNodeId", "fromPortId", "toNodeId", "toPortId"};
            for (auto f : kFields)
            {
                if (!HasStringField(obj, f)
                    || obj[f].as_string().value().empty())
                {
                    return Fail(
                        "missingField",
                        std::string{kind} + "." + std::string{f}
                            + " required and non-empty");
                }
            }
            if (!OnlyKeys(
                    obj,
                    {"fromNodeId", "fromPortId", "toNodeId", "toPortId"}))
            {
                return Fail(
                    "unknownField",
                    std::string{kind} + " rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateChangePayload(
            std::string_view     kind,
            const yyjson::value& payload)
        {
            if (kind == std::string_view("setValue"))
            {
                return ValidateSetValuePayload(payload);
            }
            if (kind == std::string_view("invokeCommand"))
            {
                return ValidateInvokeCommandPayload(payload);
            }
            if (kind == std::string_view("convertView"))
            {
                return ValidateConvertViewPayload(payload);
            }
            if (kind == std::string_view("addFormSequenceItem"))
            {
                return ValidateAddFormSequenceItemPayload(payload);
            }
            if (kind == std::string_view("moveFormSequenceItem"))
            {
                return ValidateMoveFormSequenceItemPayload(payload);
            }
            if (kind == std::string_view("removeFormSequenceItem"))
            {
                return ValidateRemoveFormSequenceItemPayload(payload);
            }
            if (kind == std::string_view("addGraphNode"))
            {
                return ValidateAddGraphNodePayload(payload);
            }
            if (kind == std::string_view("removeGraphNode"))
            {
                return ValidateRemoveGraphNodePayload(payload);
            }
            if (kind == std::string_view("connectGraphPorts"))
            {
                return ValidateGraphPortsPayload(payload, "connectGraphPorts");
            }
            if (kind == std::string_view("disconnectGraphPorts"))
            {
                return ValidateGraphPortsPayload(
                    payload,
                    "disconnectGraphPorts");
            }
            // Unknown kinds are rejected by the caller before dispatch.
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
        if (!HasStringField(obj, "viewKind"))
        {
            return Fail("missingField", "AuthoringDocument.viewKind required");
        }
        const auto viewKind = obj["viewKind"].as_string().value();
        if (!IsOneOf(viewKind, DOCUMENT_KINDS))
        {
            return Fail(
                "unsupportedViewKind",
                "AuthoringDocument.viewKind unsupported");
        }
        if (!HasIntegerField(obj, "revision"))
        {
            return Fail("missingField", "AuthoringDocument.revision required");
        }

        std::set<std::string> field_names;
        if (auto r = ValidateFieldContainer(obj, field_names); !r.valid)
        {
            return r;
        }

        if (obj.contains("formSequence"))
        {
            if (auto r = ValidateFormSequence(obj["formSequence"]); !r.valid)
            {
                return r;
            }
        }
        if (obj.contains("graph"))
        {
            if (auto r = ValidateGraphDocument(obj["graph"]); !r.valid)
            {
                return r;
            }
        }
        if (obj.contains("actionCatalog"))
        {
            if (auto r = ValidateActionCatalog(obj["actionCatalog"]); !r.valid)
            {
                return r;
            }
        }

        if (!OnlyKeys(
                obj,
                {"contractVersion",
                 "viewKind",
                 "revision",
                 "fields",
                 "fieldLayout",
                 "commands",
                 "formSequence",
                 "graph",
                 "actionCatalog"}))
        {
            return Fail(
                "unknownField",
                "AuthoringDocument rejects unknown field");
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

        if (auto r = ValidateChangePayload(kind, obj["payload"]); !r.valid)
        {
            return r;
        }

        if (!OnlyKeys(obj, {"baseRevision", "kind", "payload"}))
        {
            return Fail(
                "unknownField",
                "AuthoringChange rejects unknown field");
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

        if (!OnlyKeys(obj, {"document", "diagnostics"}))
        {
            return Fail(
                "unknownField",
                "AuthoringGetResult rejects unknown field");
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
        if (!obj.contains("document") || obj["document"].is_null())
        {
            return Fail(
                "missingField",
                "AuthoringApplyResult.document required");
        }
        auto doc_opt = obj["document"].as_object();
        if (!doc_opt)
        {
            return Fail(
                "invalidDocument",
                "AuthoringApplyResult.document must be object");
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
        const int64_t actual_revision = ReadInt(revision_value);
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

        if (!OnlyKeys(obj, {"acceptedProperties", "document", "diagnostics"}))
        {
            return Fail(
                "unknownField",
                "AuthoringApplyResult rejects unknown field");
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

        if (!OnlyKeys(obj, {"compile", "diagnostics"}))
        {
            return Fail(
                "unknownField",
                "AuthoringCompileResult rejects unknown field");
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
