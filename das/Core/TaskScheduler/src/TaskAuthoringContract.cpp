#include <das/Core/TaskScheduler/TaskAuthoringContract.h>

#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

        // initializer_list：元素数自动推导，新增 kind 不必同步改长度。
        constexpr std::initializer_list<std::string_view> CHANGE_KINDS{
            "setValue",
            "invokeCommand",
            "convertView",
            "addFormSequenceItem",
            "moveFormSequenceItem",
            "removeFormSequenceItem",
            "addGraphNode",
            "removeGraphNode",
            "connectGraphPorts",
            "disconnectGraphPorts",
            "uiEvent",
            "moveGraphNode",
            "removeGraphNodes",
            "moveGraphNodes",
            "renameGraphNode"};

        constexpr std::array<int, 7> VALUE_TYPES{0, 1, 2, 4, 8, 16, 32};

        constexpr std::array<int, 9> INPUT_TYPES{0, 1, 2, 3, 4, 5, 6, 7, 8};

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
        // seen_ids_out 把 commands[].id 集合回传给 caller，供 fieldLayout
        // 校验 {commandId} 叶子引用完整性。
        AuthoringValidationResult ValidateCommandDescriptorArray(
            const yyjson::value&   arr,
            std::set<std::string>* seen_ids_out = nullptr);

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

            // Optional config type word (manifest config type vocabulary,
            // e.g. "taskRepositoryRef"): the typed-dispatch channel for
            // component-domain words — plain string, no value-shape coupling.
            if (obj.contains("type") && !obj["type"].is_string())
            {
                return Fail(
                    "invalidFieldType",
                    "FieldDescriptor.type must be string");
            }

            // Optional declared target task type (component-domain fields
            // whose manifest declaration names one, e.g. a taskRepositoryRef
            // bound to a specific task type): plain string, engaged-only.
            if (obj.contains("taskGuid") && !obj["taskGuid"].is_string())
            {
                return Fail(
                    "invalidFieldTaskGuid",
                    "FieldDescriptor.taskGuid must be string");
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
            // markdown(inputType=7) 必须 readonly==true：前端只读展示，暂不支持
            // markdown 编辑（契约强制，schema if-then 同步约束）。
            if (it == 7
                && !(
                    obj.contains("readonly") && obj["readonly"].is_bool()
                    && obj["readonly"].as_bool().value_or(false)))
            {
                return Fail(
                    "invalidReadonly",
                    "inputType 7 (markdown) requires readonly=true");
            }

            // time(inputType=8) 必须 valueType=4(string "HH:MM")：通用时间
            // 字段类型，Core 只强制类型契约，HH:MM 语义由投影层/渲染控件负责
            // （schema if-then 同步约束）。
            if (it == 8 && vt != 4)
            {
                return Fail(
                    "invalidValueType",
                    "inputType 8 (time) requires valueType=4 (string \"HH:MM\")");
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
                     "type",
                     "taskGuid",
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

        // fieldLayout 扩展为 6-arg。新参数：
        //   - command_ids (down): 顶层 commands[] 的 id 集合，用于校验
        //     {commandId} 叶子引用完整性（与 fieldName in field_names 对称）。
        //   - referenced_command_ids (out): 已引用的 commandId 集合，用于
        //     检测重复引用。独立于 referenced_names — 不污染 fieldName
        //     全覆盖不变量；commandId 不要求全覆盖：未引用的 command 表示
        //     不在 layout 中渲染，合法。
        //   - row_allowed (down): 顶层 false，section.entries 内 true；row
        //     容器只能在 section.entries 内（schema 模糊防御）。
        AuthoringValidationResult ValidateFieldLayoutTree(
            const yyjson::value&         node,
            const std::set<std::string>& field_names,
            const std::set<std::string>& command_ids,
            std::set<std::string>&       referenced_names,
            std::set<std::string>&       referenced_command_ids,
            std::set<std::string>&       section_ids,
            bool                         row_allowed = false)
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

            // {commandId} 叶子 — 与 {fieldName} 对称，引用 commands[]
            // 定义池。独立 referenced_command_ids 不污染 fieldName 全覆盖
            // 不变量。
            if (obj.contains("commandId"))
            {
                if (!obj["commandId"].is_string())
                {
                    return Fail(
                        "invalidLayoutLeaf",
                        "commandId must be string");
                }
                const auto cid = obj["commandId"].as_string().value();
                if (cid.empty())
                {
                    return Fail(
                        "invalidLayoutLeaf",
                        "commandId must be non-empty");
                }
                if (command_ids.count(std::string(cid)) == 0)
                {
                    return Fail(
                        "layoutCommandNotInContainer",
                        "commandId not in commands[]");
                }
                if (!referenced_command_ids.insert(std::string(cid)).second)
                {
                    return Fail(
                        "layoutDuplicateCommandRef",
                        "commandId referenced multiple times");
                }
                if (!OnlyKeys(obj, {"commandId"}))
                {
                    return Fail(
                        "unknownField",
                        "layout command leaf rejects unknown field");
                }
                return Pass();
            }

            //{row:[...]} 横向容器。只在 section.entries 内合法
            // （row_allowed=true）；顶层直接放 row 会被拒（// schema
            // 模糊防御）。row 可嵌套 row/leaf，但不允许 section （section
            // 只在顶层）。
            if (obj.contains("row"))
            {
                if (!row_allowed)
                {
                    return Fail(
                        "rowOutsideSection",
                        "row must be inside section.entries");
                }
                auto row_opt = obj["row"].as_array();
                if (!row_opt)
                {
                    return Fail("invalidRow", "row must be array");
                }
                for (const auto& child : row_opt.value())
                {
                    if (auto r = ValidateFieldLayoutTree(
                            child,
                            field_names,
                            command_ids,
                            referenced_names,
                            referenced_command_ids,
                            section_ids,
                            /*row_allowed=*/true);
                        !r.valid)
                    {
                        return r;
                    }
                }
                if (!OnlyKeys(obj, {"row"}))
                {
                    return Fail(
                        "unknownField",
                        "layout row rejects unknown field");
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
                // section.entries 内允许 row（wire shape）。
                for (const auto& child : entries_opt.value())
                {
                    if (auto r = ValidateFieldLayoutTree(
                            child,
                            field_names,
                            command_ids,
                            referenced_names,
                            referenced_command_ids,
                            section_ids,
                            /*row_allowed=*/true);
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
                "layout node must be leaf {fieldName}/{commandId}, row, or "
                "section {id,label,entries}");
        }

        AuthoringValidationResult ValidateFieldLayout(
            const yyjson::value&         layout,
            const std::set<std::string>& field_names,
            const std::set<std::string>& command_ids)
        {
            auto arr_opt = layout.as_array();
            if (!arr_opt)
            {
                return Fail("invalidLayout", "fieldLayout must be array");
            }
            std::set<std::string> referenced_names;
            std::set<std::string> referenced_command_ids;
            std::set<std::string> section_ids;
            for (const auto& node : arr_opt.value())
            {
                if (auto r = ValidateFieldLayoutTree(
                        node,
                        field_names,
                        command_ids,
                        referenced_names,
                        referenced_command_ids,
                        section_ids,
                        /*row_allowed=*/false);
                    !r.valid)
                {
                    return r;
                }
            }
            // fieldName 全覆盖不变量保持 — commandId 不要求
            // 全覆盖，未引用的 command 就是不在 layout 中渲染，合法。
            if (referenced_names.size() != field_names.size())
            {
                return Fail(
                    "layoutMissingField",
                    "fieldLayout must reference every field exactly once");
            }
            return Pass();
        }

        // Validate the apply template portion of a CommandDescriptor. The
        // template keeps the single-change element shape {kind, payload} and
        // must NOT carry baseRevision; the submit-time builder wraps it (plus
        // the current revision) into the batch apply request
        // {baseRevision, changes:[...]}.
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

            // D-89-04: disabled（后端状态驱动的命令可用性，如 applyUpdate 无待
            // 更新时置灰）。wire 位于命令对象顶层，缺失视为 false（89-04
            // DasNativeUI 契约）。为 bool 时才合法。
            if (obj.contains("disabled") && !obj["disabled"].is_bool())
            {
                return Fail(
                    "invalidDisabled",
                    "CommandDescriptor.disabled must be bool");
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
                     "openView",
                     "disabled"}))
            {
                return Fail(
                    "unknownField",
                    "CommandDescriptor rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateCommandDescriptorArray(
            const yyjson::value&   arr,
            std::set<std::string>* seen_ids_out)
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
            // 把 commands[].id 集合回传给 caller，供 fieldLayout 校验
            // {commandId} 叶子的引用完整性（与 fieldName in field_names
            // 对称）。
            if (seen_ids_out != nullptr)
            {
                *seen_ids_out = std::move(seen_ids);
            }
            return Pass();
        }

        // Validate a FieldContainer (document root, FormSequenceItem, or
        // GraphNode): fields[], fieldLayout[], commands[]. Fills field_names
        // so the caller can apply additional constraints (e.g. enabled
        // collision check). 同时收集 command_ids 供 ValidateFieldLayout 校验
        // {commandId} 叶子引用完整性。
        AuthoringValidationResult ValidateFieldContainer(
            const auto&            obj,
            std::set<std::string>& field_names)
        {
            std::set<std::string> command_ids;
            if (obj.contains("commands"))
            {
                if (auto r = ValidateCommandDescriptorArray(
                        obj["commands"],
                        &command_ids);
                    !r.valid)
                {
                    return r;
                }
            }
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
                if (auto r = ValidateFieldLayout(
                        obj["fieldLayout"],
                        field_names,
                        command_ids);
                    !r.valid)
                {
                    return r;
                }
            }
            return Pass();
        }

        AuthoringValidationResult ValidateTarget(const auto& obj)
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
            // Fail-closed: componentRef is the only accepted target shape.
            return Fail("invalidTargetKind", "targetKind must be componentRef");
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
                if (auto r = ValidateTarget(obj); !r.valid)
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

            // item.label 是前端 task 卡标题的权威来源；缺失会让前端 fallback
            // 到 id（如 task_1），因此即便允许缺省字段，一旦出现就必须是非空
            // 字符串。description 允许空字符串（仅描述缺失不破坏 UI）。
            if (obj.contains("label"))
            {
                if (!obj["label"].is_string())
                {
                    return Fail(
                        "invalidLabel",
                        "FormSequenceItem.label must be string");
                }
                if (obj["label"].as_string().value().empty())
                {
                    return Fail(
                        "invalidLabel",
                        "FormSequenceItem.label must be non-empty");
                }
            }
            if (obj.contains("description") && !obj["description"].is_string())
            {
                return Fail(
                    "invalidDescription",
                    "FormSequenceItem.description must be string");
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
                     "label",
                     "description",
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

        // Node port projection element: the five locked keys
        // {portId, displayLabel, portType, direction, category} plus the
        // optional default-value key "value" with direction in
        // {"input", "output"} and category in
        // {"execution", "data"} (drag-link precheck data source; the
        // projection never interprets portType semantics — the category is
        // the Core-derived execution/data split). "value" carries the raw
        // node settings entry bound to an unconnected input data port —
        // any JSON shape, engaged-only on the wire (absent key, never an
        // explicit null).
        AuthoringValidationResult ValidateGraphNodePort(
            const yyjson::value& port)
        {
            auto obj_opt = port.as_object();
            if (!obj_opt)
            {
                return Fail(
                    "invalidGraphNodePort",
                    "GraphNode port must be object");
            }
            const auto& obj = obj_opt.value();

            if (!HasStringField(obj, "portId"))
            {
                return Fail("missingField", "GraphNode port.portId required");
            }
            if (obj["portId"].as_string().value().empty())
            {
                return Fail(
                    "invalidPortId",
                    "GraphNode port.portId must be non-empty");
            }
            if (!HasStringField(obj, "displayLabel"))
            {
                return Fail(
                    "missingField",
                    "GraphNode port.displayLabel required");
            }
            if (!HasStringField(obj, "portType"))
            {
                return Fail("missingField", "GraphNode port.portType required");
            }
            if (!HasStringField(obj, "direction"))
            {
                return Fail(
                    "missingField",
                    "GraphNode port.direction required");
            }
            const auto direction = obj["direction"].as_string().value();
            if (direction != std::string_view("input")
                && direction != std::string_view("output"))
            {
                return Fail(
                    "invalidPortDirection",
                    "GraphNode port.direction must be input|output");
            }
            if (!HasStringField(obj, "category"))
            {
                return Fail("missingField", "GraphNode port.category required");
            }
            const auto category = obj["category"].as_string().value();
            if (category != std::string_view("execution")
                && category != std::string_view("data"))
            {
                return Fail(
                    "invalidPortCategory",
                    "GraphNode port.category must be execution|data");
            }

            if (!OnlyKeys(
                    obj,
                    {"portId",
                     "displayLabel",
                     "portType",
                     "direction",
                     "category",
                     "value"}))
            {
                return Fail(
                    "unknownField",
                    "GraphNode port rejects unknown field");
            }
            return Pass();
        }

        AuthoringValidationResult ValidateGraphNodePorts(
            const yyjson::value& ports)
        {
            auto arr_opt = ports.as_array();
            if (!arr_opt)
            {
                return Fail("invalidPorts", "GraphNode.ports must be array");
            }
            for (const auto& port : arr_opt.value())
            {
                if (auto r = ValidateGraphNodePort(port); !r.valid)
                {
                    return r;
                }
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

            // Presentation metadata: the display label (store user label or
            // catalog default name) and the catalog-merged description.
            if (obj.contains("label") && !obj["label"].is_string())
            {
                return Fail("invalidLabel", "GraphNode.label must be string");
            }
            if (obj.contains("description") && !obj["description"].is_string())
            {
                return Fail(
                    "invalidDescription",
                    "GraphNode.description must be string");
            }

            if (obj.contains("targetKind"))
            {
                if (auto r = ValidateTarget(obj); !r.valid)
                {
                    return r;
                }
            }

            if (obj.contains("debugInfo") && !obj["debugInfo"].is_object())
            {
                return Fail("invalidDebugInfo", "debugInfo must be object");
            }

            // The node icon is the catalog entry icon passed through the
            // projection — the same absolute-path/http(s) URI rule the
            // catalog entry itself enforces.
            if (auto r = ValidateOptionalIcon(obj, "icon"); !r.valid)
            {
                return r;
            }

            if (obj.contains("ports"))
            {
                if (auto r = ValidateGraphNodePorts(obj["ports"]); !r.valid)
                {
                    return r;
                }
            }

            std::set<std::string> field_names;
            if (auto r = ValidateFieldContainer(obj, field_names); !r.valid)
            {
                return r;
            }

            if (!OnlyKeys(
                    obj,
                    {"id",
                     "label",
                     "description",
                     "icon",
                     "componentGuid",
                     "targetKind",
                     "fields",
                     "fieldLayout",
                     "commands",
                     "ports",
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

        // moveGraphNode payload: {nodeId, position:{x,y}}。节点布局坐标走
        // 文档级 viewData 轻量通道（不推进语义 revision）。键集锁死 +
        // 坐标数值有限性检查，防任意 viewData 子树注入。
        AuthoringValidationResult ValidateMoveGraphNodePayload(
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
                    "moveGraphNode.nodeId required and non-empty");
                !r.valid)
            {
                return r;
            }
            if (!HasObjectField(obj, "position"))
            {
                return Fail("missingField", "moveGraphNode.position required");
            }
            if (!OnlyKeys(obj, {"nodeId", "position"}))
            {
                return Fail(
                    "unknownField",
                    "moveGraphNode rejects unknown field");
            }
            const auto& position = obj["position"];
            auto        position_obj = position.as_object();
            static constexpr std::string_view AXES[]{"x", "y"};
            for (auto axis : AXES)
            {
                if (!position_obj->contains(axis))
                {
                    return Fail(
                        "missingField",
                        std::string{"moveGraphNode.position."}
                            + std::string{axis} + " required");
                }
                // as_num 兼容整数格式坐标（C# 序列化器把 100.0 写成
                // 100）；有限性检查拒绝 NaN/Infinity 溢出值。
                auto coordinate =
                    (*position_obj)[std::string_view(axis)].as_num();
                if (!coordinate || !std::isfinite(*coordinate))
                {
                    return Fail(
                        "invalidPosition",
                        std::string{"moveGraphNode.position."}
                            + std::string{axis} + " must be a finite number");
                }
            }
            if (!OnlyKeys(*position_obj, {"x", "y"}))
            {
                return Fail(
                    "unknownField",
                    "moveGraphNode.position rejects unknown field");
            }
            return Pass();
        }

        // removeGraphNodes（批量删除）payload: {nodeIds[], connections?[]}。
        // nodeIds 必须是非空字符串数组且无重复；connections 可选，每项为
        // 四非空字符串键的连接形状；nodeIds 与 connections 至少一项非空
        // （连线批量删除可不带 nodeIds，端点节点保留）。
        AuthoringValidationResult ValidateRemoveGraphNodesPayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (!OnlyKeys(obj, {"nodeIds", "connections"}))
            {
                return Fail(
                    "unknownField",
                    "removeGraphNodes rejects unknown field");
            }
            bool has_node_ids = false;
            if (obj.contains("nodeIds"))
            {
                auto ids = obj["nodeIds"].as_array();
                if (!ids)
                {
                    return Fail(
                        "invalidPayload",
                        "removeGraphNodes.nodeIds must be array");
                }
                has_node_ids = !ids->empty();
                std::set<std::string> seen_node_ids;
                for (const auto& item : *ids)
                {
                    auto id = item.as_string();
                    if (!id || id->empty())
                    {
                        return Fail(
                            "invalidNodeId",
                            "removeGraphNodes.nodeIds entries must be "
                            "non-empty strings");
                    }
                    if (!seen_node_ids.insert(std::string(*id)).second)
                    {
                        return Fail(
                            "invalidPayload",
                            "removeGraphNodes.nodeIds entries must be "
                            "unique");
                    }
                }
            }
            bool has_connections = false;
            if (obj.contains("connections"))
            {
                auto connections = obj["connections"].as_array();
                if (!connections)
                {
                    return Fail(
                        "invalidPayload",
                        "removeGraphNodes.connections must be array");
                }
                has_connections = !connections->empty();
                static constexpr std::string_view FIELDS[] =
                    {"fromNodeId", "fromPortId", "toNodeId", "toPortId"};
                for (const auto& connection : *connections)
                {
                    auto connection_obj = connection.as_object();
                    if (!connection_obj)
                    {
                        return Fail(
                            "invalidPayload",
                            "removeGraphNodes.connections entries must be "
                            "objects");
                    }
                    for (auto f : FIELDS)
                    {
                        if (!HasStringField(*connection_obj, f)
                            || (*connection_obj)[f].as_string().value().empty())
                        {
                            return Fail(
                                "missingField",
                                std::string{"removeGraphNodes.connections[]."}
                                    + std::string{f}
                                    + " required and non-empty");
                        }
                    }
                    if (!OnlyKeys(
                            *connection_obj,
                            {"fromNodeId",
                             "fromPortId",
                             "toNodeId",
                             "toPortId"}))
                    {
                        return Fail(
                            "unknownField",
                            "removeGraphNodes.connections[] rejects unknown "
                            "field");
                    }
                }
            }
            if (!has_node_ids && !has_connections)
            {
                return Fail(
                    "invalidPayload",
                    "removeGraphNodes requires at least one nodeIds entry "
                    "or connections entry");
            }
            return Pass();
        }

        // moveGraphNodes（批量坐标提交）payload: {moves[]}。moves 必须为
        // 非空数组；每项 OnlyKeys({nodeId, position})，nodeId 非空且整个
        // moves 内无重复；position 复用 moveGraphNode 的 as_num +
        // isfinite 有限性检查。
        AuthoringValidationResult ValidateMoveGraphNodesPayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (!HasArrayField(obj, "moves"))
            {
                return Fail("missingField", "moveGraphNodes.moves required");
            }
            if (!OnlyKeys(obj, {"moves"}))
            {
                return Fail(
                    "unknownField",
                    "moveGraphNodes rejects unknown field");
            }
            auto moves = obj["moves"].as_array();
            if (moves->empty())
            {
                return Fail(
                    "invalidPayload",
                    "moveGraphNodes.moves must be non-empty");
            }
            std::set<std::string> seen_node_ids;
            for (const auto& item : *moves)
            {
                auto move_obj = item.as_object();
                if (!move_obj)
                {
                    return Fail(
                        "invalidPayload",
                        "moveGraphNodes.moves entries must be objects");
                }
                if (auto r = RequireNonEmptyString(
                        *move_obj,
                        "nodeId",
                        "invalidNodeId",
                        "moveGraphNodes.moves[].nodeId required and "
                        "non-empty");
                    !r.valid)
                {
                    return r;
                }
                if (!HasObjectField(*move_obj, "position"))
                {
                    return Fail(
                        "missingField",
                        "moveGraphNodes.moves[].position required");
                }
                if (!OnlyKeys(*move_obj, {"nodeId", "position"}))
                {
                    return Fail(
                        "unknownField",
                        "moveGraphNodes.moves[] rejects unknown field");
                }
                const auto& position = (*move_obj)["position"];
                auto        position_obj = position.as_object();
                static constexpr std::string_view AXES[]{"x", "y"};
                for (auto axis : AXES)
                {
                    if (!position_obj->contains(axis))
                    {
                        return Fail(
                            "missingField",
                            std::string{"moveGraphNodes.moves[].position."}
                                + std::string{axis} + " required");
                    }
                    auto coordinate =
                        (*position_obj)[std::string_view(axis)].as_num();
                    if (!coordinate || !std::isfinite(*coordinate))
                    {
                        return Fail(
                            "invalidPosition",
                            std::string{"moveGraphNodes.moves[].position."}
                                + std::string{axis}
                                + " must be a finite number");
                    }
                }
                if (!OnlyKeys(*position_obj, {"x", "y"}))
                {
                    return Fail(
                        "unknownField",
                        "moveGraphNodes.moves[].position rejects unknown "
                        "field");
                }
                const auto node_id = (*move_obj)["nodeId"].as_string().value();
                if (!seen_node_ids.insert(std::string(node_id)).second)
                {
                    return Fail(
                        "invalidPayload",
                        "moveGraphNodes.moves[].nodeId must be unique "
                        "within moves");
                }
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
            static constexpr std::string_view FIELDS[] =
                {"fromNodeId", "fromPortId", "toNodeId", "toPortId"};
            for (auto f : FIELDS)
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

        // renameGraphNode payload: {nodeId, label}。label 是纯显示数据
        // （用户重命名，语义变更 revision+1）：键集锁死、两键非空、label
        // UTF-8 字节数 ≤200（防无界文本入档）。寻址面零扰动由应用层
        // 测试锁定——label 永不参与节点寻址。
        AuthoringValidationResult ValidateRenameGraphNodePayload(
            const yyjson::value& payload)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                return Fail("invalidPayload", "payload must be object");
            }
            const auto& obj = obj_opt.value();
            if (!OnlyKeys(obj, {"nodeId", "label"}))
            {
                return Fail(
                    "unknownField",
                    "renameGraphNode rejects unknown field");
            }
            if (auto r = RequireNonEmptyString(
                    obj,
                    "nodeId",
                    "invalidNodeId",
                    "renameGraphNode.nodeId required and non-empty");
                !r.valid)
            {
                return r;
            }
            if (auto r = RequireNonEmptyString(
                    obj,
                    "label",
                    "invalidPayload",
                    "renameGraphNode.label required and non-empty");
                !r.valid)
            {
                return r;
            }
            constexpr std::size_t MAX_LABEL_UTF8_BYTES = 200;
            const auto            label = obj["label"].as_string().value();
            if (label.size() > MAX_LABEL_UTF8_BYTES)
            {
                return Fail(
                    "invalidPayload",
                    "renameGraphNode.label must be at most 200 UTF-8 "
                    "bytes");
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
            if (kind == std::string_view("moveGraphNode"))
            {
                return ValidateMoveGraphNodePayload(payload);
            }
            if (kind == std::string_view("removeGraphNodes"))
            {
                return ValidateRemoveGraphNodesPayload(payload);
            }
            if (kind == std::string_view("moveGraphNodes"))
            {
                return ValidateMoveGraphNodesPayload(payload);
            }
            if (kind == std::string_view("renameGraphNode"))
            {
                return ValidateRenameGraphNodePayload(payload);
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

        // Document-level viewData partition (UI-owned canvas coordinates,
        // written by moveGraphNode): container type only — the content is
        // the UI's free partition and must not be interpreted here.
        if (obj.contains("viewData") && !obj["viewData"].is_object())
        {
            return Fail("invalidViewData", "viewData must be object");
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
                 "actionCatalog",
                 "viewData"}))
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
        if (!HasArrayField(obj, "changes"))
        {
            return Fail("missingField", "AuthoringChange.changes required");
        }

        auto changes_array = obj["changes"].as_array();
        if (changes_array->empty())
        {
            return Fail(
                "invalidChanges",
                "AuthoringChange.changes must not be empty");
        }
        if (changes_array->size() > kMaxAuthoringApplyChanges)
        {
            return Fail(
                "tooManyChanges",
                "AuthoringChange.changes exceeds the batch limit");
        }

        for (const auto& element : *changes_array)
        {
            auto element_obj = element.as_object();
            if (!element_obj)
            {
                return Fail(
                    "invalidChange",
                    "AuthoringChange.changes element must be object");
            }
            if (!HasStringField(*element_obj, "kind"))
            {
                return Fail("missingField", "changes element kind required");
            }

            auto kind = (*element_obj)["kind"].as_string().value();
            if (!IsOneOf(kind, CHANGE_KINDS))
            {
                return Fail(
                    "unsupportedChange",
                    "changes element kind unsupported");
            }
            if (!HasObjectField(*element_obj, "payload"))
            {
                return Fail("missingField", "changes element payload required");
            }

            if (auto r = ValidateChangePayload(kind, (*element_obj)["payload"]);
                !r.valid)
            {
                return r;
            }

            if (!OnlyKeys(*element_obj, {"kind", "payload"}))
            {
                return Fail(
                    "unknownField",
                    "changes element rejects unknown field");
            }
        }

        if (!OnlyKeys(obj, {"baseRevision", "changes"}))
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

    bool HasErrorDiagnostic(const yyjson::value& diagnostics_array)
    {
        auto arr_opt = diagnostics_array.as_array();
        if (!arr_opt)
        {
            return false;
        }
        for (const auto& diag : *arr_opt)
        {
            auto obj = diag.as_object();
            if (obj)
            {
                auto sev = (*obj)[std::string_view("severity")].as_string();
                if (sev && *sev == "error")
                {
                    return true;
                }
            }
        }
        return false;
    }

    std::string FirstErrorDiagnosticMessage(
        const yyjson::value& diagnostics_array)
    {
        auto arr_opt = diagnostics_array.as_array();
        if (!arr_opt)
        {
            return {};
        }
        for (const auto& diag : *arr_opt)
        {
            auto obj = diag.as_object();
            if (obj)
            {
                auto sev = (*obj)[std::string_view("severity")].as_string();
                if (sev && *sev == "error")
                {
                    auto msg = (*obj)[std::string_view("message")].as_string();
                    return msg ? std::string(*msg) : std::string{};
                }
            }
        }
        return {};
    }

    // viewData 轻量通道 kind：成功也不推进语义 revision（canvas 坐标走
    // document.viewData 分区，GraphRuntime 外壳不 ++version）。
    static bool IsViewDataChannelKind(std::string_view kind)
    {
        return kind == std::string_view("moveGraphNode")
               || kind == std::string_view("moveGraphNodes");
    }

    // Apply 结果 envelope 共用校验核。revision 合法域由调用方按批量语义
    // 折算后传入：单 change 形态下界 == 上界（精确 +1 或 viewData 的 0）；
    // 批量形态下界 = base +（存在 bump change ? 1 : 0）、上界 =
    // base + bump change 数（每个语义 change 恰 +1）。
    static AuthoringValidationResult ValidateApplyResultEnvelope(
        const yyjson::value& result,
        int64_t              base_revision,
        int64_t              minimum_success_revision,
        int64_t              maximum_success_revision)
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
        // document 可选：uiEvent 等短路 kind 的响应不带 document（前端据有无
        // document 分流 UiEventApplied/ChangeApplied）。acceptedProperties +
        // diagnostics 恒需；document 有则校验 revision 不变量，无则跳过。
        if (!HasArrayField(obj, "diagnostics"))
        {
            return Fail(
                "missingField",
                "AuthoringApplyResult.diagnostics required");
        }

        const bool has_document =
            obj.contains("document") && !obj["document"].is_null();
        if (has_document)
        {
            auto doc_opt = obj["document"].as_object();
            if (!doc_opt)
            {
                return Fail(
                    "invalidDocument",
                    "AuthoringApplyResult.document must be object");
            }
            const auto& doc = doc_opt.value();
            if (!HasIntegerField(doc, "revision"))
            {
                return Fail(
                    "missingField",
                    "AuthoringApplyResult.document.revision required");
            }
            const int64_t actual_revision = ReadInt(doc["revision"]);
            // 失败 envelope（有 error diagnostic）不推进 revision（须 ==
            // baseRevision，批内零突变）；成功 envelope 的 revision 须落在
            // 调用方折算的批量推进域内。
            const bool has_error = HasErrorDiagnostic(obj["diagnostics"]);
            if (has_error)
            {
                if (actual_revision != base_revision)
                {
                    return Fail(
                        "revisionMismatch",
                        "failed AuthoringApplyResult (error diagnostics) must "
                        "keep document.revision == baseRevision (no mutation "
                        "on failure)");
                }
            }
            else if (
                actual_revision < minimum_success_revision
                || actual_revision > maximum_success_revision)
            {
                return Fail(
                    "revisionMismatch",
                    "AuthoringApplyResult.document.revision must stay within "
                    "the batch advance domain [base, base + bumping changes]");
            }
        }

        if (auto d = ValidateDiagnosticArray(obj["diagnostics"]); !d.valid)
        {
            return d;
        }

        // commandResult 可选（无业务结果的命令不带）。出现时必须符合
        // {commandId: 非空 string, data: JsonValue} 严格形状。
        if (obj.contains("commandResult"))
        {
            auto cr_opt = obj["commandResult"].as_object();
            if (!cr_opt)
            {
                return Fail(
                    "invalidCommandResult",
                    "commandResult must be object");
            }
            const auto& cr = cr_opt.value();
            if (!OnlyKeys(cr, {"commandId", "data"}))
            {
                return Fail(
                    "invalidCommandResult",
                    "commandResult rejects unknown field");
            }
            if (!HasStringField(cr, "commandId")
                || cr["commandId"].as_string().value().empty())
            {
                return Fail(
                    "invalidCommandResult",
                    "commandResult.commandId required and non-empty");
            }
            if (!cr.contains("data"))
            {
                return Fail(
                    "invalidCommandResult",
                    "commandResult.data required");
            }
        }

        if (has_document)
        {
            if (!OnlyKeys(
                    obj,
                    {"acceptedProperties",
                     "document",
                     "diagnostics",
                     "commandResult"}))
            {
                return Fail(
                    "unknownField",
                    "AuthoringApplyResult rejects unknown field");
            }
        }
        else if (!OnlyKeys(
                     obj,
                     {"acceptedProperties", "diagnostics", "commandResult"}))
        {
            return Fail(
                "unknownField",
                "AuthoringApplyResult rejects unknown field");
        }
        return Pass();
    }

    AuthoringValidationResult ValidateAuthoringApplyResult(
        const yyjson::value& result,
        int64_t              baseRevision,
        std::string_view     kind)
    {
        const bool bumps_revision = !IsViewDataChannelKind(kind);
        return ValidateApplyResultEnvelope(
            result,
            baseRevision,
            baseRevision + (bumps_revision ? 1 : 0),
            baseRevision + (bumps_revision ? 1 : 0));
    }

    AuthoringValidationResult ValidateAuthoringApplyResult(
        const yyjson::value&            result,
        int64_t                         baseRevision,
        const std::vector<std::string>& kinds)
    {
        int64_t bumping_changes = 0;
        for (const auto& kind : kinds)
        {
            if (!IsViewDataChannelKind(kind))
            {
                ++bumping_changes;
            }
        }
        return ValidateApplyResultEnvelope(
            result,
            baseRevision,
            baseRevision + (bumping_changes > 0 ? 1 : 0),
            baseRevision + bumping_changes);
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
        // The string_view parameters must be deep-copied into the document
        // arena (das-yyjson-string-safety): a bare assignment only stores
        // the view's data pointer, dangling once the caller's strings die.
        obj[std::string_view("errorKind")] =
            std::make_pair(error_kind, yyjson::copy_string);
        obj[std::string_view("message")] =
            std::make_pair(message, yyjson::copy_string);
        return error;
    }

} // namespace Das::Core::TaskScheduler
