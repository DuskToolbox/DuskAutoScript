#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>

#include "ControllerMethodMaps.h"
#include "I18nResolver.h"

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
#include <MaaFramework/MaaAPI.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        std::string ToLower(std::string_view text)
        {
            std::string out(text);
            std::transform(
                out.begin(),
                out.end(),
                out.begin(),
                [](unsigned char ch)
                { return static_cast<char>(std::tolower(ch)); });
            return out;
        }
        // D-86-12: 运行时查询 MaaFramework 版本作为 PI client_maafw_version。
        // 测试构建（DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY）不链接 MaaFramework，
        // 此时返回非空占位字符串以满足「禁止空 stub」契约。
        std::string QueryMaaFrameworkVersion()
        {
#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
            return std::string{MaaVersion()};
#else
            return "test-maafw";
#endif
        }

        yyjson::value Object() { return Das::Utils::MakeYyjsonObject(); }

        template <typename JsonValue>
        yyjson::value CopyJsonValue(const JsonValue& value)
        {
            return yyjson::value(value);
        }

        yyjson::value ParseRawObject(const std::string& raw)
        {
            if (raw.empty())
            {
                return Object();
            }
            auto parsed = Das::Utils::ParseYyjsonFromString(raw);
            return parsed && parsed->is_object() ? std::move(*parsed)
                                                 : Object();
        }

        template <typename ObjectRef>
        std::optional<std::string> OptionalStringField(
            const ObjectRef& obj,
            std::string_view key)
        {
            if (!obj.contains(key) || !obj[key].is_string())
            {
                return std::nullopt;
            }
            return std::string(obj[key].as_string().value_or(""));
        }

        template <typename ObjectRef>
        std::optional<std::string> OptionalStringField(
            const ObjectRef&                        obj,
            std::initializer_list<std::string_view> keys)
        {
            for (const auto key : keys)
            {
                auto value = OptionalStringField(obj, key);
                if (value)
                {
                    return value;
                }
            }
            return std::nullopt;
        }

        template <typename ObjectRef>
        std::optional<int32_t> OptionalInt32Field(
            const ObjectRef&                        obj,
            std::initializer_list<std::string_view> keys)
        {
            for (const auto key : keys)
            {
                if (!obj.contains(key))
                {
                    continue;
                }
                auto value = obj[key].as_sint();
                if (value)
                {
                    return static_cast<int32_t>(*value);
                }
            }
            return std::nullopt;
        }

        template <typename ObjectRef>
        std::vector<std::string> StringArrayField(
            const ObjectRef&                        obj,
            std::initializer_list<std::string_view> keys)
        {
            for (const auto key : keys)
            {
                if (!obj.contains(key))
                {
                    continue;
                }
                auto array = obj[key].as_array();
                if (!array)
                {
                    continue;
                }
                std::vector<std::string> result;
                for (auto it = array->begin(); it != array->end(); ++it)
                {
                    if (it->is_string())
                    {
                        result.emplace_back(it->as_string().value_or(""));
                    }
                }
                return result;
            }
            return {};
        }

        template <typename ObjectRef>
        std::string ControllerConfigJson(const ObjectRef& obj)
        {
            if (!obj.contains(std::string_view("config")))
            {
                return "{}";
            }
            const auto& config = obj[std::string_view("config")];
            if (config.is_string())
            {
                return std::string(config.as_string().value_or("{}"));
            }
            return Das::Utils::SerializeYyjsonValue(config).value_or("{}");
        }

        ControllerSpec NormalizeControllerSpec(const PiController& controller)
        {
            ControllerSpec spec;
            spec.name = controller.dto.name;
            spec.type = controller.dto.type;

            auto raw = ParseRawObject(controller.raw.raw_json);
            auto obj = raw.as_object();
            if (!obj)
            {
                return spec;
            }

            spec.name = OptionalStringField(*obj, "name")
                            .value_or(std::move(spec.name));
            spec.type = OptionalStringField(*obj, "type")
                            .value_or(std::move(spec.type));
            spec.read_path =
                OptionalStringField(*obj, {"readPath", "read_path"})
                    .value_or("");
            spec.address = OptionalStringField(*obj, "address").value_or("");
            spec.adb_path = OptionalStringField(*obj, {"adbPath", "adb_path"})
                                .value_or("adb");
            spec.config_json = ControllerConfigJson(*obj);
            spec.agent_path =
                OptionalStringField(*obj, {"agentPath", "agent_path"})
                    .value_or("");
            return spec;
        }

        template <typename ObjectRef>
        AgentRuntime::AgentSpecDto NormalizeAgentSpec(const ObjectRef& obj)
        {
            AgentRuntime::AgentSpecDto spec;
            spec.child_exec =
                OptionalStringField(obj, {"child_exec", "childExec"})
                    .value_or("");
            spec.child_args =
                StringArrayField(obj, {"child_args", "childArgs"});
            spec.identifier = OptionalStringField(obj, "identifier");
            if (auto timeout =
                    OptionalInt32Field(obj, {"timeout_ms", "timeoutMs"}))
            {
                spec.timeout_ms = *timeout;
            }
            return spec;
        }

        template <typename ArrayRef>
        void AppendAgentSpecs(
            const ArrayRef&                          array,
            std::vector<AgentRuntime::AgentSpecDto>& agents)
        {
            for (auto it = array.begin(); it != array.end(); ++it)
            {
                if (auto obj = it->as_object())
                {
                    agents.emplace_back(NormalizeAgentSpec(*obj));
                }
            }
        }

        std::vector<AgentRuntime::AgentSpecDto> NormalizeAgentSpecs(
            const std::string& raw_agent_json)
        {
            std::vector<AgentRuntime::AgentSpecDto> agents;
            auto raw = Das::Utils::ParseYyjsonFromString(raw_agent_json);
            if (!raw)
            {
                return agents;
            }
            if (auto obj = raw->as_object())
            {
                agents.emplace_back(NormalizeAgentSpec(*obj));
                return agents;
            }
            if (auto array = raw->as_array())
            {
                AppendAgentSpecs(*array, agents);
            }
            return agents;
        }

        void MergeObject(yyjson::value& target, const yyjson::value& source)
        {
            auto target_obj = target.as_object();
            auto source_obj = source.as_object();
            if (!target_obj || !source_obj)
            {
                return;
            }
            for (auto it = source_obj->begin(); it != source_obj->end(); ++it)
            {
                (*target_obj)[std::string_view(it->first)] =
                    CopyJsonValue(it->second);
            }
        }

        const PiController* FindController(
            const PiCatalog& catalog,
            std::string_view name)
        {
            auto it = std::find_if(
                catalog.controllers.begin(),
                catalog.controllers.end(),
                [&](const PiController& item)
                { return item.dto.name == name; });
            return it == catalog.controllers.end() ? nullptr : &*it;
        }

        const PiResource* FindResource(
            const PiCatalog& catalog,
            std::string_view name)
        {
            auto it = std::find_if(
                catalog.resources.begin(),
                catalog.resources.end(),
                [&](const PiResource& item) { return item.dto.name == name; });
            return it == catalog.resources.end() ? nullptr : &*it;
        }

        const PiTask* FindTask(const PiCatalog& catalog, std::string_view name)
        {
            auto it = std::find_if(
                catalog.tasks.begin(),
                catalog.tasks.end(),
                [&](const PiTask& item) { return item.dto.name == name; });
            return it == catalog.tasks.end() ? nullptr : &*it;
        }

        void AddDiagnostic(
            CompileResultDto& result,
            std::string       severity,
            std::string       code,
            std::string       message)
        {
            PiDiagnosticDto diagnostic;
            diagnostic.severity = std::move(severity);
            diagnostic.code = std::move(code);
            diagnostic.message = std::move(message);
            result.diagnostics.emplace_back(std::move(diagnostic));
        }

        bool ContainsName(
            const std::vector<std::string>& names,
            std::string_view                name)
        {
            return names.empty()
                   || std::find(names.begin(), names.end(), name)
                          != names.end();
        }

        bool IsActive(
            const PiOption&  option,
            std::string_view controller,
            std::string_view resource)
        {
            return ContainsName(option.dto.controller, controller)
                   && ContainsName(option.dto.resource, resource);
        }

        std::string SwitchCaseName(
            const PiOption&                 option,
            const MaapiPiOptionSettingsDto& selected)
        {
            if (!selected.bool_value)
            {
                return selected.selected_cases.empty()
                           ? std::string{}
                           : selected.selected_cases.front();
            }
            const bool desired = *selected.bool_value;
            for (const auto& item : option.dto.cases)
            {
                const bool truthy = item.name == "Yes" || item.name == "yes"
                                    || item.name == "Y" || item.name == "y";
                const bool falsy = item.name == "No" || item.name == "no"
                                   || item.name == "N" || item.name == "n";
                if ((desired && truthy) || (!desired && falsy))
                {
                    return item.name;
                }
            }
            return {};
        }

        // Locate the falsy case name for an unedited switch option. PI v2.7.0
        // permits arbitrary case names; the default "关" branch is identified
        // by the same case-insensitive No/no/N/n match used by SwitchCaseName
        // for edited switches. Returns the first falsy case name found so
        // MergeCaseOverrides can apply its pipeline_override; falls back to
        // "No" when no case matches (preserves prior literal-default behavior
        // as a last-resort safety net).
        std::string DefaultSwitchCaseName(const PiOption& option)
        {
            for (const auto& item : option.dto.cases)
            {
                const bool falsy = item.name == "No" || item.name == "no"
                                   || item.name == "N" || item.name == "n";
                if (falsy)
                {
                    return item.name;
                }
            }
            return "No";
        }

        // Resolved form of a PI option's current selection. SSOT for
        // "what value does this option contribute right now?" — used by both
        // pipeline_override merging (MergeSelectedOption) and pretask option
        // serialization (CollectPretaskOptionValue). When `selected` is null
        // the option is unedited, so PI defaults apply per protocol v2.7.0:
        //   select/checkbox → option.dto.default_cases
        //   switch          → falsy case (No/no/N/n, see DefaultSwitchCaseName)
        //   input           → input.default_value.value_or("")
        // When `selected` is non-null the user's edited value wins.
        struct ResolvedOptionSelection
        {
            std::vector<std::string>           cases;
            std::map<std::string, std::string> input_values;
        };

        ResolvedOptionSelection ResolveOptionSelection(
            const PiOption&                 option,
            const MaapiPiOptionSettingsDto* selected)
        {
            ResolvedOptionSelection out;
            const auto&             type = option.dto.type;
            if (type == "switch")
            {
                std::string case_name = selected
                                            ? SwitchCaseName(option, *selected)
                                            : DefaultSwitchCaseName(option);
                if (!case_name.empty())
                {
                    out.cases.emplace_back(std::move(case_name));
                }
                return out;
            }
            if (type == "input")
            {
                for (const auto& input : option.dto.inputs)
                {
                    std::string value = input.default_value.value_or("");
                    if (selected)
                    {
                        auto it = selected->input_values.find(input.name);
                        if (it != selected->input_values.end())
                        {
                            value = it->second;
                        }
                    }
                    out.input_values[input.name] = std::move(value);
                }
                return out;
            }
            // select / checkbox (unrecognized types fall back to cases path).
            out.cases =
                selected ? selected->selected_cases : option.dto.default_cases;
            return out;
        }

        std::string ReplaceAll(
            std::string                               text,
            const std::map<std::string, std::string>& values)
        {
            for (const auto& [key, value] : values)
            {
                const std::string token = "{" + key + "}";
                size_t            pos = 0;
                while ((pos = text.find(token, pos)) != std::string::npos)
                {
                    text.replace(pos, token.size(), value);
                    pos += value.size();
                }
            }
            return text;
        }

        void MergeCaseOverrides(
            yyjson::value&                  pipeline,
            const PiOption&                 option,
            const std::vector<std::string>& selected_cases)
        {
            for (const auto& defined_case : option.dto.cases)
            {
                if (std::find(
                        selected_cases.begin(),
                        selected_cases.end(),
                        defined_case.name)
                    == selected_cases.end())
                {
                    continue;
                }
                auto raw =
                    Das::Utils::ParseYyjsonFromString(option.raw.raw_json);
                if (!raw || !raw->is_object())
                {
                    continue;
                }
                auto opt_obj = raw->as_object();
                if (!opt_obj->contains(std::string_view("cases")))
                {
                    continue;
                }
                auto cases = (*opt_obj)[std::string_view("cases")].as_array();
                if (!cases)
                {
                    continue;
                }
                for (auto it = cases->begin(); it != cases->end(); ++it)
                {
                    auto case_obj = it->as_object();
                    if (!case_obj
                        || !case_obj->contains(std::string_view("name"))
                        || (*case_obj)[std::string_view("name")]
                                   .as_string()
                                   .value_or("")
                               != defined_case.name
                        || !case_obj->contains(
                            std::string_view("pipeline_override")))
                    {
                        continue;
                    }
                    MergeObject(
                        pipeline,
                        (*case_obj)[std::string_view("pipeline_override")]);
                }
            }
        }

        void MergeSelectedOption(
            yyjson::value&                  pipeline,
            const PiOption&                 option,
            const MaapiPiOptionSettingsDto* selected,
            std::string_view                controller,
            std::string_view                resource)
        {
            if (!IsActive(option, controller, resource))
            {
                return;
            }
            auto resolved = ResolveOptionSelection(option, selected);
            if (option.dto.type == "input")
            {
                MergeObject(
                    pipeline,
                    ParseRawObject(ReplaceAll(
                        option.raw_pipeline_override_json,
                        resolved.input_values)));
                return;
            }
            MergeCaseOverrides(pipeline, option, resolved.cases);
        }

        std::vector<MaapiPiTaskSettingsDto> SelectedTasks(
            const AcceptedSettingsDto& settings,
            const PiCatalog&           catalog)
        {
            std::vector<MaapiPiTaskSettingsDto> tasks;
            for (const auto& task : settings.pi.tasks)
            {
                if (task.enabled)
                {
                    tasks.emplace_back(task);
                }
            }
            if (!tasks.empty())
            {
                return tasks;
            }
            for (const auto& task : catalog.tasks)
            {
                if (task.dto.default_check)
                {
                    MaapiPiTaskSettingsDto selected;
                    selected.task_name = task.dto.name;
                    tasks.emplace_back(std::move(selected));
                }
            }
            if (tasks.empty() && !catalog.tasks.empty())
            {
                MaapiPiTaskSettingsDto selected;
                selected.task_name = catalog.tasks.front().dto.name;
                tasks.emplace_back(std::move(selected));
            }
            return tasks;
        }

        // Locate the user's stored selection for an option key. Pretask option
        // values live at the global scope.
        const MaapiPiOptionSettingsDto* FindGlobalSelection(
            const AcceptedSettingsDto& settings,
            std::string_view           option_name)
        {
            for (const auto& opt : settings.pi.global_options)
            {
                if (opt.option_name == option_name)
                {
                    return &opt;
                }
            }
            return nullptr;
        }

        const MaapiPiOptionSettingsDto* FindResourceSelection(
            const AcceptedSettingsDto& settings,
            std::string_view           option_name)
        {
            for (const auto& opt : settings.pi.resource_options)
            {
                if (opt.option_name == option_name)
                {
                    return &opt;
                }
            }
            return nullptr;
        }

        const MaapiPiOptionSettingsDto* FindControllerSelection(
            const AcceptedSettingsDto& settings,
            std::string_view           option_name)
        {
            for (const auto& opt : settings.pi.controller_options)
            {
                if (opt.option_name == option_name)
                {
                    return &opt;
                }
            }
            return nullptr;
        }

        const MaapiPiOptionSettingsDto* FindTaskSelection(
            const MaapiPiTaskSettingsDto& task_settings,
            std::string_view              option_name)
        {
            for (const auto& opt : task_settings.options)
            {
                if (opt.option_name == option_name)
                {
                    return &opt;
                }
            }
            return nullptr;
        }

        // D-86-02 / PI v2.7.0 pretask option serialization. Recursively emits
        // the selected value for `option_key` into `result`, then descends
        // into the activated case's nested option[] entries (protocol L290).
        // v2.8.1 controller/resource filter via IsActive; cycles broken by
        // `visited`.
        void CollectPretaskOptionValue(
            const PiCatalog&           catalog,
            std::string_view           option_key,
            const AcceptedSettingsDto& settings,
            std::string_view           controller_name,
            std::string_view           resource_name,
            yyjson::value&             result,
            std::set<std::string>&     visited)
        {
            const std::string key{option_key};
            if (visited.count(key) != 0)
            {
                return;
            }
            const auto* option = FindOption(catalog, option_key);
            if (!option)
            {
                return;
            }
            if (!IsActive(*option, controller_name, resource_name))
            {
                return;
            }
            visited.insert(key);

            const auto* selected = FindGlobalSelection(settings, option_key);
            auto        resolved = ResolveOptionSelection(*option, selected);
            auto        result_obj = result.as_object();
            const auto& type = option->dto.type;
            if (type == "select" || type == "switch")
            {
                const std::string case_name = resolved.cases.empty()
                                                  ? std::string{}
                                                  : resolved.cases.front();
                // copy_string avoids dangling pointer when key/case_name go
                // out of scope (das-yyjson-string-safety).
                (*result_obj)[key] = std::make_pair(
                    std::string_view(case_name),
                    yyjson::copy_string);
                // Recurse into nested option[] refs on the activated case.
                for (const auto& case_def : option->dto.cases)
                {
                    if (case_def.name != case_name)
                    {
                        continue;
                    }
                    for (const auto& nested : case_def.option)
                    {
                        CollectPretaskOptionValue(
                            catalog,
                            nested,
                            settings,
                            controller_name,
                            resource_name,
                            result,
                            visited);
                    }
                }
                return;
            }
            if (type == "checkbox")
            {
                const auto&   cases = resolved.cases;
                yyjson::value arr(Das::Utils::MakeYyjsonArray());
                auto          arr_ref = arr.as_array();
                for (const auto& c : cases)
                {
                    arr_ref->emplace_back(yyjson::value(std::string(c)));
                }
                (*result_obj)[key] = std::move(arr);
                for (const auto& case_def : option->dto.cases)
                {
                    if (std::find(cases.begin(), cases.end(), case_def.name)
                        == cases.end())
                    {
                        continue;
                    }
                    for (const auto& nested : case_def.option)
                    {
                        CollectPretaskOptionValue(
                            catalog,
                            nested,
                            settings,
                            controller_name,
                            resource_name,
                            result,
                            visited);
                    }
                }
                return;
            }
            if (type == "input")
            {
                yyjson::value input_obj(Das::Utils::MakeYyjsonObject());
                auto          input_ref = input_obj.as_object();
                for (const auto& input : option->dto.inputs)
                {
                    const std::string value = resolved.input_values[input.name];
                    (*input_ref)[input.name] = std::make_pair(
                        std::string_view(value),
                        yyjson::copy_string);
                }
                (*result_obj)[key] = std::move(input_obj);
            }
        }

        // Build the trailing compact single-line JSON for a pretask. Returns
        // empty string when no option keys are declared; otherwise a compact
        // JSON object (possibly "{}" when all options get filtered out by
        // v2.8.1 controller/resource). Caller decides to append based on
        // whether pretask.option was empty.
        std::string SerializePretaskOptionJson(
            const std::vector<std::string>& option_keys,
            const PiCatalog&                catalog,
            const AcceptedSettingsDto&      settings,
            std::string_view                controller_name,
            std::string_view                resource_name)
        {
            yyjson::value         result(Das::Utils::MakeYyjsonObject());
            std::set<std::string> visited;
            for (const auto& key : option_keys)
            {
                CollectPretaskOptionValue(
                    catalog,
                    key,
                    settings,
                    controller_name,
                    resource_name,
                    result,
                    visited);
            }
            auto serialized = Das::Utils::SerializeYyjsonValue(result);
            return serialized.value_or("{}");
        }

    } // namespace

    CompileResultDto CompileMaapi(
        const AcceptedSettingsDto& settings,
        const PiCatalog&           catalog,
        std::string_view)
    {
        CompileResultDto     result;
        ExecutionEnvelopeDto envelope;
        envelope.maapi.interface_directory = std::string{
            DAS::Utils::U8AsString(catalog.interface_directory.u8string())};
        envelope.maapi.fail_fast = settings.adapter.execution_policy.fail_fast;
        envelope.maapi.requires_agent_runtime = !catalog.raw_agent_json.empty();
        envelope.maapi.agent = NormalizeAgentSpecs(catalog.raw_agent_json);
        envelope.maapi.pi_env.project_version = catalog.version;
        // D-86-12: 拓宽 envelope env snapshot 至 8 字段，与
        // AgentRuntime::PiEnvDto 对齐，让 compile→envelope→run 路径全 8 个 PI_*
        // env 变量可达。 client_maafw_version 由 MaaVersion()
        // 在编译期填充（MaaUtility.h:22）， client_version 取 DasMaaPi
        // 插件版本（与 manifest DasMaaPi.json:version 对齐），禁止空 stub。
        envelope.maapi.pi_env.client_maafw_version = QueryMaaFrameworkVersion();
        envelope.maapi.pi_env.client_version = std::string{PLUGIN_VERSION_TEXT};
        result.summary.requires_agent_runtime =
            envelope.maapi.requires_agent_runtime;

        const auto controller_name = settings.pi.controller_name.value_or(
            catalog.controllers.empty() ? std::string{}
                                        : catalog.controllers.front().dto.name);
        const auto resource_name = settings.pi.resource_name.value_or(
            catalog.resources.empty() ? std::string{}
                                      : catalog.resources.front().dto.name);
        const auto* controller = FindController(catalog, controller_name);
        const auto* resource = FindResource(catalog, resource_name);
        if (!controller)
        {
            AddDiagnostic(
                result,
                "error",
                "missing-controller",
                "Selected controller is missing");
        }
        if (!resource)
        {
            AddDiagnostic(
                result,
                "error",
                "missing-resource",
                "Selected resource is missing");
        }
        if (envelope.maapi.requires_agent_runtime
            && envelope.maapi.agent.empty())
        {
            AddDiagnostic(
                result,
                "error",
                "missing-agent",
                "PI agent runtime is required but no valid agent spec exists");
        }
        if (!controller || !resource)
        {
            result.ok = false;
            result.can_execute = false;
            return result;
        }

        envelope.maapi.controller_name = controller->dto.name;
        envelope.maapi.controller = NormalizeControllerSpec(*controller);
        // Typed init params: always emitted. Every supported controller type
        // (Adb/Dbg/Win32/Gamepad) now routes through the unified
        // CreateControllerFromParams dispatch as typed variants alongside
        // Tier2 types (MacOS/PlayCover/WlRoots/...) which surface
        // controller-type-unsupported-on-platform. The legacy flat-string
        // CreateController(spec) overload has been deleted.
        envelope.maapi.controller_init_params =
            NormalizeControllerInitParams(*controller);
        envelope.maapi.resource_name = resource->dto.name;
        envelope.maapi.resource_hash = resource->dto.hash;
        for (const auto& path : resource->resolved_paths)
        {
            envelope.maapi.resource_paths.emplace_back(
                DAS::Utils::U8AsString(path.u8string()));
        }
        // PI `$key` i18n resolution: walk controller / resource raw_json
        // before assignment so the runtime receives localized strings
        // instead of literal `$key` placeholders. Language selection is
        // shared with AuthoringProjector via SelectPiLanguage so the
        // envelope and the projected document resolve the same catalog to
        // the same language (zh_cn-preferred fallback). Malformed JSON falls
        // back to the original raw_json verbatim (Pitfall 5 defense — no
        // crash, no exception).
        const std::string pi_language =
            SelectPiLanguage(settings.pi.language, &catalog);
        const auto resolve_raw = [&](const std::string& raw_json) -> std::string
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(raw_json);
            if (!parsed.has_value())
            {
                return raw_json;
            }
            auto serialized = Das::Utils::SerializeYyjsonValue(
                ResolveI18nJson(*parsed, catalog, pi_language));
            return serialized.value_or(raw_json);
        };
        envelope.maapi.pi_env.controller_json =
            resolve_raw(controller->raw.raw_json);
        envelope.maapi.pi_env.resource_json =
            resolve_raw(resource->raw.raw_json);
        result.summary.controller_name = controller->dto.name;
        result.summary.resource_name = resource->dto.name;

        // PI v2.7.0 pretasks (D-86-02). Filter by v2.8.1 controller/resource,
        // serialize selected option values as trailing compact JSON when the
        // pretask declares any option[] keys. MaaRuntime spawns each entry
        // BEFORE CreateController.
        for (const auto& pretask : catalog.pretasks)
        {
            if (!ContainsName(pretask.dto.controller, controller->dto.name)
                || !ContainsName(pretask.dto.resource, resource->dto.name))
            {
                continue;
            }
            MaaPretaskDto dto;
            dto.name = pretask.dto.name;
            dto.exec = pretask.dto.exec;
            dto.args = pretask.dto.args;
            if (!pretask.dto.option.empty())
            {
                dto.args.push_back(SerializePretaskOptionJson(
                    pretask.dto.option,
                    catalog,
                    settings,
                    controller->dto.name,
                    resource->dto.name));
            }
            envelope.maapi.pretasks.emplace_back(std::move(dto));
        }

        for (const auto& selected : SelectedTasks(settings, catalog))
        {
            const auto* task = FindTask(catalog, selected.task_name);
            if (!task)
            {
                AddDiagnostic(
                    result,
                    "error",
                    "missing-task",
                    "Selected task is missing");
                continue;
            }
            MaaTaskExecutionDto execution;
            execution.task_name = task->dto.name;
            execution.entry = task->dto.entry;
            execution.pipeline_override =
                ParseRawObject(task->raw_pipeline_override_json);
            // PI v2.7.0 option merge order: global<resource<controller<task.
            // Each level iterates the option keys DECLARED by the catalog
            // (catalog.global_options, catalog.settings[].option[],
            // resource->dto.option, controller->dto.option, task->dto.option)
            // and looks up the user's stored selection; unedited options fall
            // back to PI defaults via ResolveOptionSelection inside
            // MergeSelectedOption so defaults are no longer silently dropped.
            //
            // Global level: explicit global_option[] plus setting-referenced
            // options (PI v2.8.0 setting[].option[]). The latter share
            // settings.pi.global_options storage with explicit global_option.
            // `merged_global_keys` dedups across global_option[] and all
            // setting partitions: the same key is merged at most once even
            // when declared in multiple places (idempotent MergeObject).
            std::set<std::string> merged_global_keys;
            for (const auto& opt : catalog.global_options)
            {
                merged_global_keys.insert(opt.dto.name);
                MergeSelectedOption(
                    execution.pipeline_override,
                    opt,
                    FindGlobalSelection(settings, opt.dto.name),
                    controller->dto.name,
                    resource->dto.name);
            }
            for (const auto& setting : catalog.settings)
            {
                for (const auto& key : setting.dto.option)
                {
                    if (!merged_global_keys.insert(key).second)
                    {
                        continue;
                    }
                    const auto* opt = FindOption(catalog, key);
                    if (!opt)
                    {
                        continue;
                    }
                    MergeSelectedOption(
                        execution.pipeline_override,
                        *opt,
                        FindGlobalSelection(settings, key),
                        controller->dto.name,
                        resource->dto.name);
                }
            }
            // Resource level.
            for (const auto& key : resource->dto.option)
            {
                const auto* opt = FindOption(catalog, key);
                if (!opt)
                {
                    continue;
                }
                MergeSelectedOption(
                    execution.pipeline_override,
                    *opt,
                    FindResourceSelection(settings, key),
                    controller->dto.name,
                    resource->dto.name);
            }
            // Controller level.
            for (const auto& key : controller->dto.option)
            {
                const auto* opt = FindOption(catalog, key);
                if (!opt)
                {
                    continue;
                }
                MergeSelectedOption(
                    execution.pipeline_override,
                    *opt,
                    FindControllerSelection(settings, key),
                    controller->dto.name,
                    resource->dto.name);
            }
            // Task level.
            for (const auto& key : task->dto.option)
            {
                const auto* opt = FindOption(catalog, key);
                if (!opt)
                {
                    continue;
                }
                MergeSelectedOption(
                    execution.pipeline_override,
                    *opt,
                    FindTaskSelection(selected, key),
                    controller->dto.name,
                    resource->dto.name);
            }
            result.summary.task_names.emplace_back(task->dto.name);
            envelope.maapi.tasks.emplace_back(std::move(execution));
        }

        // Stale option selection scan: PI interface evolution may drop an
        // option from the catalog while the user's stored settings still
        // references its key. Compile silently ignores such keys (forward-
        // progress contract: never fail compile for stale state), but
        // operators need observability to clean up settings.json. Emit one
        // non-blocking warning diagnostic per stale key. Scanned levels:
        // global / resource / controller / each selected task's options.
        // Task level uses SelectedTasks so default-picked tasks (no stored
        // user options) do not synthesize spurious warnings.
        auto emit_stale_warning = [&](std::string_view option_key)
        {
            if (FindOption(catalog, option_key) == nullptr)
            {
                AddDiagnostic(
                    result,
                    "warning",
                    "stale-option-selection",
                    "stale option selection ignored: "
                        + std::string(option_key));
            }
        };
        for (const auto& opt : settings.pi.global_options)
        {
            emit_stale_warning(opt.option_name);
        }
        for (const auto& opt : settings.pi.resource_options)
        {
            emit_stale_warning(opt.option_name);
        }
        for (const auto& opt : settings.pi.controller_options)
        {
            emit_stale_warning(opt.option_name);
        }
        for (const auto& task : SelectedTasks(settings, catalog))
        {
            for (const auto& opt : task.options)
            {
                emit_stale_warning(opt.option_name);
            }
        }

        result.ok = std::none_of(
            result.diagnostics.begin(),
            result.diagnostics.end(),
            [](const PiDiagnosticDto& diagnostic)
            { return diagnostic.severity == "error"; });
        result.can_execute = result.ok && !envelope.maapi.tasks.empty();
        result.summary.can_execute = result.can_execute;
        if (result.ok)
        {
            result.execution_input = std::move(envelope);
        }
        return result;
    }

    // Build typed init params from a PI controller's raw_json. Reads
    // same-level adb/dbg/win32/gamepad blocks and display_* keys (NOT the
    // `config` sub-key, which is Adb's generic JSON blob). String→enum
    // translation delegates to ControllerMethodMaps. ControllerSpec wire
    // format is untouched — this struct is a parallel, non-serialized
    // carrier. Every supported controller type (adb/dbg/win32/gamepad/Tier2)
    // now routes through CreateControllerFromParams; the legacy flat-string
    // CreateController(spec) overload is gone.
    ControllerInitParams NormalizeControllerInitParams(
        const PiController& controller)
    {
        ControllerInitParams params;
        params.controller_type = controller.dto.type;
        auto raw = ParseRawObject(controller.raw.raw_json);
        auto obj = raw.as_object();
        if (!obj)
        {
            return params;
        }

        const auto lower_type = ToLower(params.controller_type);

        // Adb typed block: reads same-level address/adbPath/config/agentPath
        // (mirrors NormalizeControllerSpec). screencap/input methods default
        // to 0; production boundary can substitute Maa defaults at call time
        // when both are zero.
        if (lower_type == "adb")
        {
            AdbInitParams adb;
            adb.address = OptionalStringField(*obj, "address").value_or("");
            adb.adb_path = OptionalStringField(*obj, {"adbPath", "adb_path"})
                               .value_or("adb");
            adb.config_json = ControllerConfigJson(*obj);
            adb.agent_path =
                OptionalStringField(*obj, {"agentPath", "agent_path"})
                    .value_or("");
            params.adb = std::move(adb);
        }

        // Dbg typed block: read_path only.
        if (lower_type == "dbg" || lower_type == "debug")
        {
            DbgInitParams dbg;
            dbg.read_path = OptionalStringField(*obj, {"readPath", "read_path"})
                                .value_or("");
            params.dbg = std::move(dbg);
        }

        // Same-level win32 typed block (NOT config.win32).
        if (obj->contains(std::string_view("win32")))
        {
            if (auto win32_obj = (*obj)[std::string_view("win32")].as_object())
            {
                Win32InitParams win32;
                win32.class_regex =
                    OptionalStringField(*win32_obj, "class_regex").value_or("");
                win32.window_regex =
                    OptionalStringField(*win32_obj, "window_regex")
                        .value_or("");
                win32.screencap_method = ParseWin32ScreencapMethod(
                    OptionalStringField(*win32_obj, "screencap").value_or(""));
                win32.mouse_method = ParseWin32InputMethod(
                    OptionalStringField(*win32_obj, "mouse").value_or(""));
                win32.keyboard_method = ParseWin32InputMethod(
                    OptionalStringField(*win32_obj, "keyboard").value_or(""));
                params.win32 = std::move(win32);
            }
        }

        // Same-level gamepad typed block.
        if (obj->contains(std::string_view("gamepad")))
        {
            if (auto gamepad_obj =
                    (*obj)[std::string_view("gamepad")].as_object())
            {
                GamepadInitParams gamepad;
                gamepad.class_regex =
                    OptionalStringField(*gamepad_obj, "class_regex")
                        .value_or("");
                gamepad.window_regex =
                    OptionalStringField(*gamepad_obj, "window_regex")
                        .value_or("");
                gamepad.gamepad_type = ParseGamepadType(
                    OptionalStringField(*gamepad_obj, "gamepad_type")
                        .value_or(""));
                gamepad.screencap_method = ParseWin32ScreencapMethod(
                    OptionalStringField(*gamepad_obj, "screencap")
                        .value_or(""));
                params.gamepad = std::move(gamepad);
            }
        }

        // Display options (PI v2.3.0 focus.display, applied via
        // MaaControllerSetOption). Same-level keys, integers / bool.
        params.display_short_side = OptionalInt32Field(
            *obj,
            {"display_short_side", "displayShortSide"});
        params.display_long_side =
            OptionalInt32Field(*obj, {"display_long_side", "displayLongSide"});
        if (obj->contains(std::string_view("display_raw"))
            && (*obj)[std::string_view("display_raw")].is_bool())
        {
            params.display_raw =
                (*obj)[std::string_view("display_raw")].as_bool().value_or(
                    false);
        }
        else if (
            obj->contains(std::string_view("displayRaw"))
            && (*obj)[std::string_view("displayRaw")].is_bool())
        {
            params.display_raw =
                (*obj)[std::string_view("displayRaw")].as_bool().value_or(
                    false);
        }
        return params;
    }

    Das::DasPtr<Das::ExportInterface::IDasJson> SerializeCompileResult(
        const CompileResultDto& result,
        std::string_view        purpose)
    {
        // 手动按字段构造 owning yyjson::value：preview 路径不写入
        // executionInput 字段（保持 caller 期望的「字段缺失」语义，而不是
        // default_caster 对 optional<nullopt> 默认的 null 输出）。
        // 同时所有 owning value 操作都在本函数内完成并立即序列化为字符串，
        // 避免 owning yyjson::value 跨函数 move 损坏 mut_doc（参见
        // cpp_yyjson-value-moved-from-null-deref.md）。
        yyjson::value compiled_json(Das::Utils::MakeYyjsonObject());
        auto          obj = compiled_json.as_object();
        (*obj)[std::string_view("ok")] = result.ok;
        (*obj)[std::string_view("canExecute")] = result.can_execute;
        (*obj)[std::string_view("summary")] = result.summary;
        yyjson::value diagnostics_json(Das::Utils::MakeYyjsonArray());
        for (const auto& diagnostic : result.diagnostics)
        {
            yyjson::value item(Das::Utils::MakeYyjsonObject());
            auto          item_obj = item.as_object();
            (*item_obj)[std::string_view("severity")] =
                yyjson::value(std::string(diagnostic.severity));
            (*item_obj)[std::string_view("code")] =
                yyjson::value(std::string(diagnostic.code));
            (*item_obj)[std::string_view("message")] =
                yyjson::value(std::string(diagnostic.message));
            diagnostics_json.as_array()->emplace_back(std::move(item));
        }
        (*obj)[std::string_view("diagnostics")] = std::move(diagnostics_json);
        if (purpose == "execution" && result.execution_input)
        {
            (*obj)[std::string_view("executionInput")] =
                *result.execution_input;
        }

        auto serialized = Utils::SerializeYyjsonValue(compiled_json);
        if (!serialized)
        {
            return {};
        }
        Das::DasPtr<Das::ExportInterface::IDasJson> out;
        ParseDasJsonFromString(serialized->c_str(), out.Put());
        return out;
    }
} // namespace Das::Plugins::DasMaaPi
