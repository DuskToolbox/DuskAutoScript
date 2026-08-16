#include <das/Plugins/DasMaaPi/MaaPiExecutionEngine.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <cctype>
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
            return DAS::IsOk(stop_token->StopRequested(&can_stop)) && can_stop;
        }

        // 所选控制器（settings 选择或 catalog 首项默认，镜像 CompileMaapi
        // 的解析顺序）是否 Adb 类型。大小写不敏感，与
        // NormalizeControllerInitParams 的 ToLower(controller_type) 对齐。
        bool IsAdbControllerSelected(
            const PiCatalog&           catalog,
            const AcceptedSettingsDto& settings)
        {
            const std::string controller_name =
                settings.pi.controller_name.value_or(
                    catalog.controllers.empty()
                        ? std::string{}
                        : catalog.controllers.front().dto.name);
            const auto controller_it = std::find_if(
                catalog.controllers.begin(),
                catalog.controllers.end(),
                [&](const PiController& item)
                { return item.dto.name == controller_name; });
            if (controller_it == catalog.controllers.end())
            {
                return false;
            }
            std::string controller_type = controller_it->dto.type;
            std::transform(
                controller_type.begin(),
                controller_type.end(),
                controller_type.begin(),
                [](unsigned char character)
                { return static_cast<char>(std::tolower(character)); });
            return controller_type == "adb";
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
                    for (auto it = arr->begin(); it != arr->end(); ++it)
                    {
                        if (it->is_string())
                        {
                            option.selected_cases.emplace_back(
                                it->as_string().value_or(""));
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
                    for (auto it = obj->begin(); it != obj->end(); ++it)
                    {
                        option.input_values.emplace(
                            it->first,
                            it->second.as_string().value_or(""));
                    }
                }
                return option;
            }

            option.kind = "input";
            return option;
        }

        AcceptedSettingsDto BuildMinimalSettings(
            const EngineInput& input,
            const PiCatalog& /*catalog*/)
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
                    for (auto it = obj->begin(); it != obj->end(); ++it)
                    {
                        task.options.emplace_back(BuildOptionFromYyjson(
                            std::string(it->first),
                            it->second));
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
                    (*pipeline_obj)[pi_param_name] =
                        Das::Utils::CloneYyjsonValue(input_value);
                }
            }
        }

        yyjson::value BuildCompletedTasksArray(
            const std::vector<std::string>& completed_tasks)
        {
            auto arr = Das::Utils::MakeYyjsonArray();
            for (const auto& task : completed_tasks)
            {
                arr.as_array()->emplace_back(yyjson::value(std::string{task}));
            }
            return arr;
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

        // PI v2 的 controller.adb 对象不含 address（协议：设备由客户端
        // 运行时发现），Adb 控制器从 raw_json 只能解析出空 address，真实
        // 边界会在 MaaAdbControllerCreate 处拒绝。设备解析放在与
        // PI_MISSING / TASK_MISSING 同层的执行输入校验段：任务设置必须
        // 携带用户显式选择的 adbDevice（address），且该 address 必须在
        // 执行期 Toolkit 扫描结果中（六字段 adb_path/screencap/input/
        // config 只能运行时取，不持久化）。不做静默自动选设备——headless
        // 调度跑错设备的代价远大于显性失败。
        std::vector<AdbDeviceInfo> discovered_devices;
        if (IsAdbControllerSelected(catalog, settings))
        {
            if (input.adb_device.empty())
            {
                output.das_result = DAS_E_MAAPI_ADB_DEVICE_MISSING;
                output.error_message = "Adb device is not selected";
                output.diagnostics.emplace_back(
                    MaaRuntimeDiagnostic{
                        "error",
                        "adb-device-missing",
                        "Adb controller requires a selected device; set "
                        "'adbDevice' in the task settings",
                        std::nullopt});
                return output;
            }
            discovered_devices = boundary.FindAdbDevices();
            const auto device_it = std::find_if(
                discovered_devices.begin(),
                discovered_devices.end(),
                [&](const AdbDeviceInfo& info)
                { return info.address == input.adb_device; });
            if (device_it == discovered_devices.end())
            {
                output.das_result = DAS_E_MAAPI_ADB_DEVICE_MISSING;
                output.error_message =
                    "Selected Adb device not found: " + input.adb_device;
                output.diagnostics.emplace_back(
                    MaaRuntimeDiagnostic{
                        "error",
                        "adb-device-not-found",
                        "Selected Adb device '" + input.adb_device
                            + "' is absent from the Toolkit scan; check the "
                              "device connection or re-select the device",
                        std::nullopt});
                return output;
            }
            settings.pi.controller_adb_device = input.adb_device;
        }

        auto compile_result =
            CompileMaapi(settings, catalog, "execute", &discovered_devices);

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

        output.completed_tasks = std::move(runtime_result.completed_tasks);
        output.diagnostics = std::move(runtime_result.diagnostics);
        output.outputs["completedTasks"] =
            BuildCompletedTasksArray(output.completed_tasks);

        if (runtime_result.das_result != DAS_S_OK)
        {
            output.das_result = runtime_result.stopped
                                    ? DAS_E_TIMEOUT
                                    : DAS_E_MAAPI_EXECUTION_FAILED;
            output.error_message = runtime_result.stopped
                                       ? "Execution stopped"
                                       : "MaaFramework execution failed";
            return output;
        }

        output.das_result = DAS_S_OK;
        return output;
    }

} // namespace Das::Plugins::DasMaaPi
