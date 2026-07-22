#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
#include <MaaFramework/MaaAPI.h>
#endif

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
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
            CompileResultDto&               result,
            yyjson::value&                  pipeline,
            const PiCatalog&                catalog,
            const MaapiPiOptionSettingsDto& selected,
            std::string_view                controller,
            std::string_view                resource)
        {
            const auto* option = FindOption(catalog, selected.option_name);
            if (!option)
            {
                AddDiagnostic(
                    result,
                    "error",
                    "missing-option",
                    "Selected PI option is missing from catalog");
                return;
            }
            if (!IsActive(*option, controller, resource))
            {
                return;
            }
            if (option->dto.type == "input")
            {
                MergeObject(
                    pipeline,
                    ParseRawObject(ReplaceAll(
                        option->raw_pipeline_override_json,
                        selected.input_values)));
                return;
            }
            auto cases = selected.selected_cases;
            if (option->dto.type == "switch")
            {
                cases.clear();
                auto case_name = SwitchCaseName(*option, selected);
                if (!case_name.empty())
                {
                    cases.emplace_back(std::move(case_name));
                }
            }
            MergeCaseOverrides(pipeline, *option, cases);
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
        envelope.maapi.pi_env.client_version = std::string{kPluginVersionText};
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
        envelope.maapi.resource_name = resource->dto.name;
        envelope.maapi.resource_hash = resource->dto.hash;
        for (const auto& path : resource->resolved_paths)
        {
            envelope.maapi.resource_paths.emplace_back(
                std::string{DAS::Utils::U8AsString(path.u8string())});
        }
        envelope.maapi.pi_env.controller_json = controller->raw.raw_json;
        envelope.maapi.pi_env.resource_json = resource->raw.raw_json;
        result.summary.controller_name = controller->dto.name;
        result.summary.resource_name = resource->dto.name;

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
            for (const auto& option : settings.pi.global_options)
            {
                MergeSelectedOption(
                    result,
                    execution.pipeline_override,
                    catalog,
                    option,
                    controller->dto.name,
                    resource->dto.name);
            }
            for (const auto& option : settings.pi.resource_options)
            {
                MergeSelectedOption(
                    result,
                    execution.pipeline_override,
                    catalog,
                    option,
                    controller->dto.name,
                    resource->dto.name);
            }
            for (const auto& option : settings.pi.controller_options)
            {
                MergeSelectedOption(
                    result,
                    execution.pipeline_override,
                    catalog,
                    option,
                    controller->dto.name,
                    resource->dto.name);
            }
            for (const auto& option : selected.options)
            {
                MergeSelectedOption(
                    result,
                    execution.pipeline_override,
                    catalog,
                    option,
                    controller->dto.name,
                    resource->dto.name);
            }
            result.summary.task_names.emplace_back(task->dto.name);
            envelope.maapi.tasks.emplace_back(std::move(execution));
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
