#include <das/Plugins/DasMaaPi/MaaPiExecutionEngine.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <string>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        bool IsStopRequested(PluginInterface::IDasStopToken* stop_token)
        {
            if (!stop_token)
            {
                return false;
            }
            bool can_stop = false;
            auto hr = stop_token->StopRequested(&can_stop);
            return DAS::IsSucceeded(hr) && can_stop;
        }

        MaapiPiOptionSettingsDto BuildOptionFromYyjson(
            const std::string&   option_name,
            const yyjson::value& value)
        {
            MaapiPiOptionSettingsDto option;
            option.option_name = option_name;

            if (value.is_string())
            {
                option.kind = "select";
                option.selected_cases.emplace_back(
                    value.as_string().value_or(""));
                return option;
            }

            if (value.is_array())
            {
                option.kind = "checkbox";
                auto arr = value.as_array();
                if (arr)
                {
                    for (auto& elem : *arr)
                    {
                        if (elem.is_string())
                        {
                            option.selected_cases.emplace_back(
                                elem.as_string().value_or(""));
                        }
                    }
                }
                return option;
            }

            if (value.is_bool())
            {
                option.kind = "switch";
                option.bool_value = value.as_bool().value_or(false);
                return option;
            }

            if (value.is_object())
            {
                option.kind = "input";
                auto obj = value.as_object();
                if (obj)
                {
                    for (auto& [key, val] : *obj)
                    {
                        option.input_values.emplace(
                            key,
                            val.as_string().value_or(""));
                    }
                }
                return option;
            }

            option.kind = "input";
            return option;
        }

        AcceptedSettingsDto BuildMinimalSettings(
            const EngineInput& input,
            const PiCatalog&   catalog)
        {
            AcceptedSettingsDto settings;

            settings.adapter.interface_path = input.pi_path;

            MaapiPiTaskSettingsDto task;
            task.task_name = input.task_name;
            task.enabled = true;

            if (input.options.is_object())
            {
                auto obj = input.options.as_object();
                if (obj)
                {
                    for (auto& [key, val] : *obj)
                    {
                        task.options.emplace_back(
                            BuildOptionFromYyjson(std::string(key), val));
                    }
                }
            }

            settings.pi.tasks.emplace_back(std::move(task));

            return settings;
        }

        void MergePortMapInputs(
            ExecutionEnvelopeDto&                       envelope,
            const std::map<std::string, std::string>&   port_map,
            const std::map<std::string, yyjson::value>& inputs)
        {
            if (port_map.empty() || inputs.empty())
            {
                return;
            }

            for (auto& task : envelope.maapi.tasks)
            {
                auto pipeline_obj = task.pipeline_override.as_object();
                if (!pipeline_obj)
                {
                    task.pipeline_override = Das::Utils::MakeYyjsonObject();
                    pipeline_obj = task.pipeline_override.as_object();
                    if (!pipeline_obj)
                    {
                        continue;
                    }
                }

                for (const auto& [graph_port_id, pi_param_name] : port_map)
                {
                    auto it = inputs.find(graph_port_id);
                    if (it == inputs.end())
                    {
                        continue;
                    }

                    const auto& input_value = it->second;
                    auto        serialized =
                        Das::Utils::SerializeYyjsonValue(input_value);
                    if (!serialized)
                    {
                        continue;
                    }

                    auto parsed =
                        Das::Utils::ParseYyjsonFromString(*serialized);
                    if (parsed)
                    {
                        pipeline_obj[pi_param_name] = std::move(*parsed);
                    }
                }
            }
        }

    } // namespace

    EngineOutput MaaPiExecutionEngine::Execute(
        const EngineInput&              input,
        IMaaApiBoundary&                boundary,
        PluginInterface::IDasStopToken* stop_token)
    {
        EngineOutput output;

        if (input.pi_path.empty())
        {
            output.das_result = DAS_E_MAAPI_PI_MISSING;
            output.error_message = "PI path is empty";
            return output;
        }

        if (IsStopRequested(stop_token))
        {
            output.das_result = DAS_E_TIMEOUT;
            output.error_message = "Stopped before execution";
            return output;
        }

        auto parse_result =
            ParseProjectInterface({.interface_path = input.pi_path});
        if (!parse_result.ok)
        {
            output.das_result = DAS_E_MAAPI_PI_PARSE_FAILED;
            output.error_message = "Failed to parse PI at: " + input.pi_path;
            return output;
        }

        if (IsStopRequested(stop_token))
        {
            output.das_result = DAS_E_TIMEOUT;
            output.error_message = "Stopped after PI parse";
            return output;
        }

        const auto& catalog = parse_result.catalog;

        const PiTask* target_task = nullptr;
        for (const auto& task : catalog.tasks)
        {
            if (task.dto.name == input.task_name)
            {
                target_task = &task;
                break;
            }
        }

        if (!target_task)
        {
            output.das_result = DAS_E_MAAPI_TASK_MISSING;
            output.error_message = "Task not found: " + input.task_name;
            return output;
        }

        if (IsStopRequested(stop_token))
        {
            output.das_result = DAS_E_TIMEOUT;
            output.error_message = "Stopped before compile";
            return output;
        }

        auto settings = BuildMinimalSettings(input, catalog);

        auto compile_result = CompileMaapi(settings, catalog, "execute");

        if (!compile_result.ok || !compile_result.execution_input)
        {
            output.das_result = DAS_E_MAAPI_OPTION_PARSE_FAILED;
            output.error_message = "Compile failed";
            for (const auto& diag : compile_result.diagnostics)
            {
                output.diagnostics.emplace_back(
                    MaaRuntimeDiagnostic{
                        diag.severity,
                        diag.code,
                        diag.message,
                        std::nullopt});
            }
            return output;
        }

        auto envelope = std::move(*compile_result.execution_input);

        MergePortMapInputs(envelope, input.port_map, input.inputs);

        if (IsStopRequested(stop_token))
        {
            output.das_result = DAS_E_TIMEOUT;
            output.error_message = "Stopped before runtime execution";
            return output;
        }

        auto runtime_result = MaaRuntime::Run(envelope, boundary, stop_token);

        if (runtime_result.das_result != DAS_S_OK)
        {
            output.das_result = DAS_E_MAAPI_EXECUTION_FAILED;
            output.error_message = "MaaFramework execution failed";
            output.completed_tasks = std::move(runtime_result.completed_tasks);
            output.diagnostics = std::move(runtime_result.diagnostics);
            return output;
        }

        output.das_result = DAS_S_OK;
        output.completed_tasks = std::move(runtime_result.completed_tasks);
        output.diagnostics = std::move(runtime_result.diagnostics);

        auto completed_array = Das::Utils::MakeYyjsonArray();
        auto arr = completed_array.as_array();
        if (arr)
        {
            for (const auto& name : output.completed_tasks)
            {
                arr->push_back(yyjson::value(name));
            }
        }
        output.outputs["completedTasks"] = std::move(completed_array);

        return output;
    }

} // namespace Das::Plugins::DasMaaPi
