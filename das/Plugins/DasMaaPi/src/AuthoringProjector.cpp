#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        yyjson::value Object() { return Das::Utils::MakeYyjsonObject(); }
        yyjson::value Array() { return Das::Utils::MakeYyjsonArray(); }
        yyjson::value JsonString(std::string_view value)
        {
            return yyjson::value(std::string(value));
        }

        template <typename TObject>
        std::optional<std::string> OptionalString(
            const TObject&   obj,
            std::string_view key)
        {
            if (!obj.contains(key) || !obj[key].is_string())
            {
                return std::nullopt;
            }
            return std::string(obj[key].as_string().value_or(""));
        }

        template <typename TObject>
        bool
        OptionalBool(const TObject& obj, std::string_view key, bool fallback)
        {
            if (!obj.contains(key) || !obj[key].is_bool())
            {
                return fallback;
            }
            return obj[key].as_bool().value_or(fallback);
        }

        template <typename TObject>
        std::vector<std::string> StringArray(
            const TObject&   obj,
            std::string_view key);

        std::vector<MaapiPiOptionSettingsDto> ParseOptionSettingsArray(
            const auto&      obj,
            std::string_view key)
        {
            std::vector<MaapiPiOptionSettingsDto> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                auto option_obj = it->as_object();
                if (!option_obj)
                {
                    continue;
                }
                MaapiPiOptionSettingsDto option;
                option.option_name =
                    OptionalString(*option_obj, "optionName").value_or("");
                option.kind = OptionalString(*option_obj, "kind").value_or("");
                option.selected_cases =
                    StringArray(*option_obj, "selectedCases");
                if (option_obj->contains(std::string_view("boolValue"))
                    && (*option_obj)[std::string_view("boolValue")].is_bool())
                {
                    option.bool_value =
                        (*option_obj)[std::string_view("boolValue")]
                            .as_bool()
                            .value_or(false);
                }
                if (option_obj->contains(std::string_view("inputValues")))
                {
                    if (auto inputs =
                            (*option_obj)[std::string_view("inputValues")]
                                .as_object())
                    {
                        for (auto input_it = inputs->begin();
                             input_it != inputs->end();
                             ++input_it)
                        {
                            if (input_it->second.is_string())
                            {
                                option.input_values.emplace(
                                    input_it->first,
                                    input_it->second.as_string().value_or(""));
                            }
                        }
                    }
                }
                if (!option.option_name.empty())
                {
                    result.emplace_back(std::move(option));
                }
            }
            return result;
        }

        std::vector<MaapiPiTaskSettingsDto> ParseTaskSettingsArray(
            const auto&      obj,
            std::string_view key)
        {
            std::vector<MaapiPiTaskSettingsDto> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                auto task_obj = it->as_object();
                if (!task_obj)
                {
                    continue;
                }
                MaapiPiTaskSettingsDto task;
                task.task_name =
                    OptionalString(*task_obj, "taskName").value_or("");
                task.enabled = OptionalBool(*task_obj, "enabled", true);
                task.options = ParseOptionSettingsArray(*task_obj, "options");
                if (!task.task_name.empty())
                {
                    result.emplace_back(std::move(task));
                }
            }
            return result;
        }

        template <typename TObject>
        std::vector<std::string> StringArray(
            const TObject&   obj,
            std::string_view key)
        {
            std::vector<std::string> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                if (it->is_string())
                {
                    result.emplace_back(it->as_string().value_or(""));
                }
            }
            return result;
        }

        template <typename TObject>
        AcceptedSettingsDto ParseAcceptedObject(const TObject& obj)
        {
            AcceptedSettingsDto settings;
            if (obj.contains(std::string_view("adapter")))
            {
                if (auto adapter = obj[std::string_view("adapter")].as_object())
                {
                    settings.adapter.interface_path =
                        OptionalString(*adapter, "interfacePath");
                    settings.adapter.project_root =
                        OptionalString(*adapter, "projectRoot");
                    settings.adapter.source_fingerprint =
                        OptionalString(*adapter, "sourceFingerprint");
                    if (adapter->contains(std::string_view("executionPolicy")))
                    {
                        if (auto policy =
                                (*adapter)[std::string_view("executionPolicy")]
                                    .as_object())
                        {
                            settings.adapter.execution_policy.fail_fast =
                                OptionalBool(*policy, "failFast", true);
                        }
                    }
                }
            }

            if (obj.contains(std::string_view("pi")))
            {
                if (auto pi = obj[std::string_view("pi")].as_object())
                {
                    settings.pi.controller_name =
                        OptionalString(*pi, "controllerName");
                    settings.pi.resource_name =
                        OptionalString(*pi, "resourceName");
                    settings.pi.preset_name = OptionalString(*pi, "presetName");
                    settings.pi.orphan_paths = StringArray(*pi, "orphanPaths");
                    settings.pi.global_options =
                        ParseOptionSettingsArray(*pi, "globalOptions");
                    settings.pi.resource_options =
                        ParseOptionSettingsArray(*pi, "resourceOptions");
                    settings.pi.controller_options =
                        ParseOptionSettingsArray(*pi, "controllerOptions");
                    settings.pi.tasks = ParseTaskSettingsArray(*pi, "tasks");
                }
            }
            return settings;
        }

        void AddDiagnosticJson(yyjson::value& arr, const PiDiagnosticDto& item)
        {
            yyjson::value diag(Object());
            auto          obj = diag.as_object();
            (*obj)[std::string_view("severity")] = JsonString(item.severity);
            (*obj)[std::string_view("code")] = JsonString(item.code);
            (*obj)[std::string_view("message")] = JsonString(item.message);
            if (item.path)
            {
                (*obj)[std::string_view("path")] = JsonString(*item.path);
            }
            if (item.source)
            {
                (*obj)[std::string_view("source")] = JsonString(*item.source);
            }
            arr.as_array()->emplace_back(std::move(diag));
        }

        yyjson::value CandidateArray(const auto& items)
        {
            yyjson::value arr(Array());
            for (const auto& item : items)
            {
                yyjson::value candidate(Object());
                auto          obj = candidate.as_object();
                (*obj)[std::string_view("name")] = JsonString(item.dto.name);
                if (item.dto.label)
                {
                    (*obj)[std::string_view("label")] =
                        JsonString(*item.dto.label);
                }
                arr.as_array()->emplace_back(std::move(candidate));
            }
            return arr;
        }

        yyjson::value MakePropertyField(
            std::string_view     name,
            std::string_view     label,
            int                  value_type,
            int                  input_type,
            const yyjson::value& value,
            const yyjson::value* candidates = nullptr)
        {
            yyjson::value field(Object());
            auto          obj = field.as_object();
            (*obj)[std::string_view("name")] = JsonString(name);
            (*obj)[std::string_view("label")] = JsonString(label);
            (*obj)[std::string_view("valueType")] = value_type;
            (*obj)[std::string_view("inputType")] = input_type;
            (*obj)[std::string_view("value")] =
                Das::Utils::CloneYyjsonValue(value);
            if (candidates != nullptr)
            {
                (*obj)[std::string_view("candidates")] =
                    Das::Utils::CloneYyjsonValue(*candidates);
            }
            return field;
        }

        // PropertySchema valueType/inputType mapping from PiOption.dto.type.
        // select→(4,1); checkbox→(32,2); switch→(8,0); input→pipeline_type
        // int→(0,0)/float|double→(2,0)/else→(4,0); fallback→(4,0).
        struct PropertyTypeMapping
        {
            int value_type;
            int input_type;
        };

        PropertyTypeMapping MapOptionType(const PiOptionDto& dto)
        {
            if (dto.type == "select")
            {
                return {4, 1};
            }
            if (dto.type == "checkbox")
            {
                return {32, 2};
            }
            if (dto.type == "switch")
            {
                return {8, 0};
            }
            if (dto.type == "input" && !dto.inputs.empty()
                && dto.inputs.front().pipeline_type)
            {
                const auto& pt = *dto.inputs.front().pipeline_type;
                if (pt == "int")
                {
                    return {0, 0};
                }
                if (pt == "float" || pt == "double")
                {
                    return {2, 0};
                }
            }
            return {4, 0};
        }

        yyjson::value OptionCandidates(const PiOptionDto& dto)
        {
            yyjson::value arr(Array());
            for (const auto& item : dto.cases)
            {
                yyjson::value candidate(Object());
                auto          obj = candidate.as_object();
                (*obj)[std::string_view("name")] = JsonString(item.name);
                if (item.label)
                {
                    (*obj)[std::string_view("label")] = JsonString(*item.label);
                }
                arr.as_array()->emplace_back(std::move(candidate));
            }
            return arr;
        }

        // Build a PropertySchema field for a pi option, projecting from catalog
        // dto and seeding value from the current option settings (if any).
        yyjson::value MakeOptionPropertyField(
            std::string_view                field_name,
            const PiOptionDto&              dto,
            const MaapiPiOptionSettingsDto* current)
        {
            auto          mapping = MapOptionType(dto);
            yyjson::value value(Das::Utils::MakeYyjsonObject());
            auto          vobj = value.as_object();
            if (current)
            {
                yyjson::value cases(Array());
                for (const auto& selected_case : current->selected_cases)
                {
                    cases.as_array()->emplace_back(JsonString(selected_case));
                }
                (*vobj)[std::string_view("selectedCases")] = std::move(cases);
                if (current->bool_value)
                {
                    (*vobj)[std::string_view("boolValue")] =
                        *current->bool_value;
                }
                yyjson::value inputs(Object());
                for (const auto& [key, input_value] : current->input_values)
                {
                    (*inputs.as_object())[std::string_view(key)] =
                        JsonString(input_value);
                }
                (*vobj)[std::string_view("inputValues")] = std::move(inputs);
            }
            yyjson::value  candidates;
            yyjson::value* candidates_ptr = nullptr;
            if (mapping.input_type == 1 || mapping.input_type == 2)
            {
                candidates = OptionCandidates(dto);
                candidates_ptr = &candidates;
            }
            return MakePropertyField(
                field_name,
                dto.label.value_or(dto.name),
                mapping.value_type,
                mapping.input_type,
                value,
                candidates_ptr);
        }

        yyjson::value TaskSettingsArray(
            const std::vector<MaapiPiTaskSettingsDto>& tasks)
        {
            yyjson::value arr(Array());
            for (const auto& task : tasks)
            {
                yyjson::value value(Object());
                auto          obj = value.as_object();
                (*obj)[std::string_view("taskName")] =
                    JsonString(task.task_name);
                (*obj)[std::string_view("enabled")] = task.enabled;
                yyjson::value options(Array());
                for (const auto& option : task.options)
                {
                    yyjson::value option_json(Object());
                    auto          option_obj = option_json.as_object();
                    (*option_obj)[std::string_view("optionName")] =
                        JsonString(option.option_name);
                    (*option_obj)[std::string_view("kind")] =
                        JsonString(option.kind);
                    yyjson::value cases(Array());
                    for (const auto& selected_case : option.selected_cases)
                    {
                        cases.as_array()->emplace_back(
                            JsonString(selected_case));
                    }
                    (*option_obj)[std::string_view("selectedCases")] =
                        std::move(cases);
                    if (option.bool_value)
                    {
                        (*option_obj)[std::string_view("boolValue")] =
                            *option.bool_value;
                    }
                    yyjson::value inputs(Object());
                    for (const auto& [key, input_value] : option.input_values)
                    {
                        (*inputs.as_object())[std::string_view(key)] =
                            JsonString(input_value);
                    }
                    (*option_obj)[std::string_view("inputValues")] =
                        std::move(inputs);
                    options.as_array()->emplace_back(std::move(option_json));
                }
                (*obj)[std::string_view("options")] = std::move(options);
                arr.as_array()->emplace_back(std::move(value));
            }
            return arr;
        }

        yyjson::value OptionSettingsArray(
            const std::vector<MaapiPiOptionSettingsDto>& options)
        {
            yyjson::value arr(Array());
            for (const auto& option : options)
            {
                yyjson::value value(Object());
                auto          obj = value.as_object();
                (*obj)[std::string_view("optionName")] =
                    JsonString(option.option_name);
                (*obj)[std::string_view("kind")] = JsonString(option.kind);
                yyjson::value cases(Array());
                for (const auto& selected_case : option.selected_cases)
                {
                    cases.as_array()->emplace_back(JsonString(selected_case));
                }
                (*obj)[std::string_view("selectedCases")] = std::move(cases);
                if (option.bool_value)
                {
                    (*obj)[std::string_view("boolValue")] = *option.bool_value;
                }
                yyjson::value inputs(Object());
                for (const auto& [key, input_value] : option.input_values)
                {
                    (*inputs.as_object())[std::string_view(key)] =
                        JsonString(input_value);
                }
                (*obj)[std::string_view("inputValues")] = std::move(inputs);
                arr.as_array()->emplace_back(std::move(value));
            }
            return arr;
        }

        bool HasTask(const PiCatalog& catalog, std::string_view name)
        {
            return std::any_of(
                catalog.tasks.begin(),
                catalog.tasks.end(),
                [&](const PiTask& task) { return task.dto.name == name; });
        }

        bool ContainsName(
            const std::vector<std::string>& values,
            std::string_view                name)
        {
            return std::find(values.begin(), values.end(), name)
                   != values.end();
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

        MaapiPiTaskSettingsDto* FindSelectedTask(
            AcceptedSettingsDto& settings,
            std::string_view     task_name)
        {
            auto it = std::find_if(
                settings.pi.tasks.begin(),
                settings.pi.tasks.end(),
                [&](const MaapiPiTaskSettingsDto& item)
                { return item.task_name == task_name; });
            return it == settings.pi.tasks.end() ? nullptr : &*it;
        }

        std::vector<MaapiPiOptionSettingsDto>* FindTaskOptionContainer(
            AcceptedSettingsDto& settings,
            const PiCatalog&     catalog,
            std::string_view     option_name)
        {
            for (auto& task : settings.pi.tasks)
            {
                const auto* catalog_task = FindTask(catalog, task.task_name);
                if (catalog_task
                    && ContainsName(catalog_task->dto.option, option_name))
                {
                    return &task.options;
                }
            }

            auto catalog_task = std::find_if(
                catalog.tasks.begin(),
                catalog.tasks.end(),
                [&](const PiTask& item)
                { return ContainsName(item.dto.option, option_name); });
            if (catalog_task == catalog.tasks.end())
            {
                return nullptr;
            }

            MaapiPiTaskSettingsDto selected;
            selected.task_name = catalog_task->dto.name;
            settings.pi.tasks.emplace_back(std::move(selected));
            return &settings.pi.tasks.back().options;
        }

        const MaapiPiOptionSettingsDto* FindOptionSettings(
            const std::vector<MaapiPiOptionSettingsDto>& options,
            std::string_view                             option_name)
        {
            auto it = std::find_if(
                options.begin(),
                options.end(),
                [&](const MaapiPiOptionSettingsDto& item)
                { return item.option_name == option_name; });
            return it == options.end() ? nullptr : &*it;
        }

        const MaapiPiOptionSettingsDto* FindOptionSettingsEverywhere(
            const AcceptedSettingsDto& settings,
            std::string_view           option_name)
        {
            if (const auto* o =
                    FindOptionSettings(settings.pi.global_options, option_name))
            {
                return o;
            }
            if (const auto* o = FindOptionSettings(
                    settings.pi.resource_options,
                    option_name))
            {
                return o;
            }
            if (const auto* o = FindOptionSettings(
                    settings.pi.controller_options,
                    option_name))
            {
                return o;
            }
            for (const auto& task : settings.pi.tasks)
            {
                if (const auto* o =
                        FindOptionSettings(task.options, option_name))
                {
                    return o;
                }
            }
            return nullptr;
        }

        void RemoveOption(
            std::vector<MaapiPiOptionSettingsDto>& options,
            std::string_view                       option_name)
        {
            std::erase_if(
                options,
                [&](const MaapiPiOptionSettingsDto& item)
                { return item.option_name == option_name; });
        }

        void RemoveOptionEverywhere(
            AcceptedSettingsDto& settings,
            std::string_view     option_name)
        {
            RemoveOption(settings.pi.global_options, option_name);
            RemoveOption(settings.pi.resource_options, option_name);
            RemoveOption(settings.pi.controller_options, option_name);
            for (auto& task : settings.pi.tasks)
            {
                RemoveOption(task.options, option_name);
            }
        }

        void RemoveOrphanPath(
            AcceptedSettingsDto& settings,
            std::string_view     path)
        {
            std::erase_if(
                settings.pi.orphan_paths,
                [&](const std::string& item) { return item == path; });
        }

        void RemoveOrphanPrefix(
            AcceptedSettingsDto& settings,
            std::string_view     prefix)
        {
            std::erase_if(
                settings.pi.orphan_paths,
                [&](const std::string& item)
                { return item.starts_with(prefix); });
        }

        void UpsertOption(
            std::vector<MaapiPiOptionSettingsDto>& options,
            MaapiPiOptionSettingsDto               option)
        {
            auto it = std::find_if(
                options.begin(),
                options.end(),
                [&](const MaapiPiOptionSettingsDto& item)
                { return item.option_name == option.option_name; });
            if (it == options.end())
            {
                options.emplace_back(std::move(option));
                return;
            }
            *it = std::move(option);
        }

        std::optional<MaapiPiOptionSettingsDto> ParseOptionValue(
            const PiOption&      option,
            const yyjson::value& value)
        {
            if (value.is_null())
            {
                return std::nullopt;
            }

            MaapiPiOptionSettingsDto result;
            result.option_name = option.dto.name;
            result.kind = option.dto.type;

            if (auto obj = value.as_object())
            {
                result.kind =
                    OptionalString(*obj, "kind").value_or(result.kind);
                result.selected_cases = StringArray(*obj, "selectedCases");
                if (obj->contains(std::string_view("boolValue"))
                    && (*obj)[std::string_view("boolValue")].is_bool())
                {
                    result.bool_value = (*obj)[std::string_view("boolValue")]
                                            .as_bool()
                                            .value_or(false);
                }
                if (obj->contains(std::string_view("inputValues")))
                {
                    if (auto inputs =
                            (*obj)[std::string_view("inputValues")].as_object())
                    {
                        for (auto input_it = inputs->begin();
                             input_it != inputs->end();
                             ++input_it)
                        {
                            if (input_it->second.is_string())
                            {
                                result.input_values.emplace(
                                    input_it->first,
                                    input_it->second.as_string().value_or(""));
                            }
                        }
                    }
                }
                return result;
            }

            if (value.is_bool())
            {
                result.bool_value = value.as_bool().value_or(false);
                return result;
            }

            if (value.is_string())
            {
                const auto text = std::string(value.as_string().value_or(""));
                if (option.dto.type == "input" && !option.dto.inputs.empty())
                {
                    result.input_values.emplace(
                        option.dto.inputs.front().name,
                        text);
                }
                else
                {
                    result.selected_cases.emplace_back(text);
                }
                return result;
            }

            if (auto arr = value.as_array())
            {
                for (auto it = arr->begin(); it != arr->end(); ++it)
                {
                    if (it->is_string())
                    {
                        result.selected_cases.emplace_back(
                            it->as_string().value_or(""));
                    }
                }
                return result;
            }

            return result;
        }

        void ApplyOptionValueChange(
            AcceptedSettingsDto& settings,
            const PiCatalog&     catalog,
            std::string_view     option_name,
            const yyjson::value& value)
        {
            const auto* option = FindOption(catalog, option_name);
            if (!option)
            {
                if (value.is_null())
                {
                    RemoveOptionEverywhere(settings, option_name);
                    RemoveOrphanPath(
                        settings,
                        std::string{"pi.options:"} + std::string{option_name});
                    RemoveOrphanPath(
                        settings,
                        std::string{"pi.options."} + std::string{option_name});
                    return;
                }
                settings.pi.orphan_paths.emplace_back(
                    std::string{"pi.options:"} + std::string{option_name});
                return;
            }

            RemoveOrphanPath(
                settings,
                std::string{"pi.options:"} + std::string{option_name});
            RemoveOrphanPath(
                settings,
                std::string{"pi.options."} + std::string{option_name});
            if (value.is_null())
            {
                RemoveOptionEverywhere(settings, option_name);
                return;
            }

            auto parsed = ParseOptionValue(*option, value);
            if (!parsed)
            {
                RemoveOptionEverywhere(settings, option_name);
                return;
            }

            RemoveOptionEverywhere(settings, option_name);
            if (std::any_of(
                    catalog.global_options.begin(),
                    catalog.global_options.end(),
                    [&](const PiOption& item)
                    { return item.dto.name == option_name; }))
            {
                UpsertOption(settings.pi.global_options, std::move(*parsed));
                return;
            }

            const auto controller_name = settings.pi.controller_name.value_or(
                catalog.controllers.empty()
                    ? std::string{}
                    : catalog.controllers.front().dto.name);
            if (const auto* controller =
                    FindController(catalog, controller_name);
                controller && ContainsName(controller->dto.option, option_name))
            {
                UpsertOption(
                    settings.pi.controller_options,
                    std::move(*parsed));
                return;
            }

            const auto resource_name = settings.pi.resource_name.value_or(
                catalog.resources.empty() ? std::string{}
                                          : catalog.resources.front().dto.name);
            if (const auto* resource = FindResource(catalog, resource_name);
                resource && ContainsName(resource->dto.option, option_name))
            {
                UpsertOption(settings.pi.resource_options, std::move(*parsed));
                return;
            }

            if (auto* task_options =
                    FindTaskOptionContainer(settings, catalog, option_name))
            {
                UpsertOption(*task_options, std::move(*parsed));
            }
        }
    } // namespace

    AcceptedSettingsDto ParseAcceptedSettings(const yyjson::value& value)
    {
        auto obj = value.as_object();
        if (!obj)
        {
            return {};
        }
        if (obj->contains(std::string_view("properties")))
        {
            if (auto props = (*obj)[std::string_view("properties")].as_object())
            {
                return ParseAcceptedObject(*props);
            }
        }
        return ParseAcceptedObject(*obj);
    }

    yyjson::value SerializeAcceptedSettings(const AcceptedSettingsDto& settings)
    {
        yyjson::value result(Object());
        auto          obj = result.as_object();
        (*obj)[std::string_view("version")] = settings.version;

        yyjson::value adapter(Object());
        auto          adapter_obj = adapter.as_object();
        if (settings.adapter.interface_path)
        {
            (*adapter_obj)[std::string_view("interfacePath")] =
                JsonString(*settings.adapter.interface_path);
        }
        if (settings.adapter.project_root)
        {
            (*adapter_obj)[std::string_view("projectRoot")] =
                JsonString(*settings.adapter.project_root);
        }
        if (settings.adapter.source_fingerprint)
        {
            (*adapter_obj)[std::string_view("sourceFingerprint")] =
                JsonString(*settings.adapter.source_fingerprint);
        }
        yyjson::value policy(Object());
        (*policy.as_object())[std::string_view("failFast")] =
            settings.adapter.execution_policy.fail_fast;
        (*adapter_obj)[std::string_view("executionPolicy")] = std::move(policy);
        (*obj)[std::string_view("adapter")] = std::move(adapter);

        yyjson::value pi(Object());
        auto          pi_obj = pi.as_object();
        if (settings.pi.controller_name)
        {
            (*pi_obj)[std::string_view("controllerName")] =
                JsonString(*settings.pi.controller_name);
        }
        if (settings.pi.resource_name)
        {
            (*pi_obj)[std::string_view("resourceName")] =
                JsonString(*settings.pi.resource_name);
        }
        if (settings.pi.preset_name)
        {
            (*pi_obj)[std::string_view("presetName")] =
                JsonString(*settings.pi.preset_name);
        }
        (*pi_obj)[std::string_view("globalOptions")] =
            OptionSettingsArray(settings.pi.global_options);
        (*pi_obj)[std::string_view("resourceOptions")] =
            OptionSettingsArray(settings.pi.resource_options);
        (*pi_obj)[std::string_view("controllerOptions")] =
            OptionSettingsArray(settings.pi.controller_options);
        (*pi_obj)[std::string_view("tasks")] =
            TaskSettingsArray(settings.pi.tasks);
        yyjson::value orphans(Array());
        for (const auto& path : settings.pi.orphan_paths)
        {
            orphans.as_array()->emplace_back(JsonString(path));
        }
        (*pi_obj)[std::string_view("orphanPaths")] = std::move(orphans);
        (*obj)[std::string_view("pi")] = std::move(pi);
        return result;
    }

    yyjson::value ProjectAuthoringDocument(
        const AcceptedSettingsDto&          settings,
        const PiCatalog*                    catalog,
        const std::vector<PiDiagnosticDto>& diagnostics,
        int64_t                             revision)
    {
        yyjson::value document(Object());
        auto          obj = document.as_object();
        (*obj)[std::string_view("contractVersion")] = 1;
        (*obj)[std::string_view("kind")] = "formSequence";
        (*obj)[std::string_view("revision")] = revision;

        std::string fingerprint = "maapi-unparsed";
        if (catalog)
        {
            fingerprint = catalog->name + ":" + catalog->version;
        }
        else if (settings.adapter.source_fingerprint)
        {
            fingerprint = *settings.adapter.source_fingerprint;
        }
        (*obj)[std::string_view("sourceFingerprint")] = JsonString(fingerprint);

        yyjson::value form_sequence(Object());
        auto          fs_obj = form_sequence.as_object();

        yyjson::value fields(Array());

        fields.as_array()->emplace_back(MakePropertyField(
            "adapter.interfacePath",
            "ProjectInterface path",
            4,
            0,
            settings.adapter.interface_path
                ? yyjson::value(*settings.adapter.interface_path)
                : yyjson::value(std::string{})));
        fields.as_array()->emplace_back(MakePropertyField(
            "adapter.executionPolicy.failFast",
            "Fail fast",
            8,
            0,
            yyjson::value(settings.adapter.execution_policy.fail_fast)));

        if (catalog)
        {
            auto controller_candidates = CandidateArray(catalog->controllers);
            fields.as_array()->emplace_back(MakePropertyField(
                "pi.controllerName",
                "Controller",
                4,
                1,
                settings.pi.controller_name
                    ? yyjson::value(*settings.pi.controller_name)
                    : yyjson::value(std::string{}),
                &controller_candidates));
            auto resource_candidates = CandidateArray(catalog->resources);
            fields.as_array()->emplace_back(MakePropertyField(
                "pi.resourceName",
                "Resource",
                4,
                1,
                settings.pi.resource_name
                    ? yyjson::value(*settings.pi.resource_name)
                    : yyjson::value(std::string{}),
                &resource_candidates));
            auto preset_candidates = CandidateArray(catalog->presets);
            fields.as_array()->emplace_back(MakePropertyField(
                "pi.presetName",
                "Preset",
                4,
                1,
                settings.pi.preset_name
                    ? yyjson::value(*settings.pi.preset_name)
                    : yyjson::value(std::string{}),
                &preset_candidates));

            for (const auto& option : catalog->options)
            {
                const auto* current =
                    FindOptionSettingsEverywhere(settings, option.dto.name);
                fields.as_array()->emplace_back(MakeOptionPropertyField(
                    std::string{"pi.options." + option.dto.name},
                    option.dto,
                    current));
            }
        }
        (*fs_obj)[std::string_view("fields")] = std::move(fields);

        yyjson::value sequence(Array());
        for (const auto& task : settings.pi.tasks)
        {
            yyjson::value item(Object());
            auto          item_obj = item.as_object();
            (*item_obj)[std::string_view("id")] = JsonString(task.task_name);
            (*item_obj)[std::string_view("componentGuid")] =
                JsonString("69f20008-0000-4000-8000-000000000001");

            yyjson::value item_fields(Array());
            if (catalog)
            {
                const auto* catalog_task = FindTask(*catalog, task.task_name);
                if (catalog_task)
                {
                    for (const auto& option_name : catalog_task->dto.option)
                    {
                        const auto* option = FindOption(*catalog, option_name);
                        if (!option)
                        {
                            continue;
                        }
                        const auto* current =
                            FindOptionSettings(task.options, option_name);
                        item_fields.as_array()->emplace_back(
                            MakeOptionPropertyField(
                                std::string{"options." + option_name},
                                option->dto,
                                current));
                    }
                }
            }
            (*item_obj)[std::string_view("fields")] = std::move(item_fields);

            yyjson::value item_settings(Object());
            auto          s_obj = item_settings.as_object();
            (*s_obj)[std::string_view("enabled")] = task.enabled;
            (*s_obj)[std::string_view("options")] =
                OptionSettingsArray(task.options);
            (*item_obj)[std::string_view("settings")] =
                std::move(item_settings);
            (*item_obj)[std::string_view("enabled")] = task.enabled;

            sequence.as_array()->emplace_back(std::move(item));
        }
        (*fs_obj)[std::string_view("sequence")] = std::move(sequence);
        (*obj)[std::string_view("formSequence")] = std::move(form_sequence);

        yyjson::value values(Object());
        auto          values_obj = values.as_object();
        auto          accepted = SerializeAcceptedSettings(settings);
        auto          accepted_obj = accepted.as_object();
        (*values_obj)[std::string_view("adapter")] =
            (*accepted_obj)[std::string_view("adapter")];
        if (catalog)
        {
            (*values_obj)[std::string_view("pi")] =
                (*accepted_obj)[std::string_view("pi")];
        }
        (*obj)[std::string_view("values")] = std::move(values);

        yyjson::value catalog_json(Object());
        if (catalog)
        {
            auto catalog_obj = catalog_json.as_object();
            (*catalog_obj)[std::string_view("controllers")] =
                CandidateArray(catalog->controllers);
            (*catalog_obj)[std::string_view("resources")] =
                CandidateArray(catalog->resources);
            (*catalog_obj)[std::string_view("tasks")] =
                CandidateArray(catalog->tasks);
            (*catalog_obj)[std::string_view("presets")] =
                CandidateArray(catalog->presets);
            (*catalog_obj)[std::string_view("groups")] =
                CandidateArray(catalog->groups);
        }
        (*obj)[std::string_view("catalog")] = std::move(catalog_json);
        (*obj)[std::string_view("state")] = Object();

        yyjson::value diagnostics_json(Array());
        for (const auto& diagnostic : diagnostics)
        {
            AddDiagnosticJson(diagnostics_json, diagnostic);
        }
        for (const auto& orphan : settings.pi.orphan_paths)
        {
            PiDiagnosticDto diagnostic;
            diagnostic.severity = "warning";
            diagnostic.code = "orphan";
            diagnostic.message = "Accepted setting references missing PI data";
            diagnostic.path = orphan;
            AddDiagnosticJson(diagnostics_json, diagnostic);
        }
        (*obj)[std::string_view("diagnostics")] = std::move(diagnostics_json);

        yyjson::value migration(Object());
        yyjson::value orphan_paths(Array());
        for (const auto& orphan : settings.pi.orphan_paths)
        {
            orphan_paths.as_array()->emplace_back(JsonString(orphan));
        }
        (*migration.as_object())[std::string_view("orphanPaths")] =
            std::move(orphan_paths);
        (*obj)[std::string_view("migration")] = std::move(migration);
        return document;
    }

    void ApplySetValueChange(
        AcceptedSettingsDto& settings,
        const yyjson::value& change,
        const PiCatalog*     catalog)
    {
        auto obj = change.as_object();
        if (!obj || !obj->contains(std::string_view("payload")))
        {
            return;
        }
        auto payload = (*obj)[std::string_view("payload")].as_object();
        if (!payload)
        {
            return;
        }
        auto path_opt = OptionalString(*payload, "valuePath");
        if (!path_opt)
        {
            path_opt = OptionalString(*payload, "path");
        }
        auto path = path_opt.value_or("");
        if (path.empty() || !payload->contains(std::string_view("value")))
        {
            return;
        }
        const auto& value = (*payload)[std::string_view("value")];
        if (path == "adapter.interfacePath" && value.is_string())
        {
            settings.adapter.interface_path =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "adapter.executionPolicy.failFast" && value.is_bool())
        {
            settings.adapter.execution_policy.fail_fast =
                value.as_bool().value_or(true);
            return;
        }
        if (path == "pi.controllerName" && value.is_string())
        {
            settings.pi.controller_name =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "pi.resourceName" && value.is_string())
        {
            settings.pi.resource_name =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "pi.presetName" && value.is_string())
        {
            settings.pi.preset_name =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "pi.tasks")
        {
            yyjson::value wrapper(Object());
            (*wrapper.as_object())[std::string_view("tasks")] = value;
            settings.pi.tasks =
                ParseTaskSettingsArray(*wrapper.as_object(), "tasks");
            RemoveOrphanPrefix(settings, "pi.tasks:");
            return;
        }
        constexpr std::string_view kOptionPrefix = "pi.options.";
        if (path.starts_with(kOptionPrefix) && catalog)
        {
            ApplyOptionValueChange(
                settings,
                *catalog,
                std::string_view(path).substr(kOptionPrefix.size()),
                value);
            return;
        }
        constexpr std::string_view kTaskPrefix = "pi.tasks.";
        constexpr std::string_view kTaskOptionSeparator = ".options.";
        if (path.starts_with(kTaskPrefix) && catalog)
        {
            auto tail = std::string_view(path).substr(kTaskPrefix.size());
            auto option_pos = tail.find(kTaskOptionSeparator);
            if (option_pos != std::string_view::npos)
            {
                auto task_name = tail.substr(0, option_pos);
                auto option_name =
                    tail.substr(option_pos + kTaskOptionSeparator.size());
                auto* task = FindSelectedTask(settings, task_name);
                if (!task && HasTask(*catalog, task_name))
                {
                    MaapiPiTaskSettingsDto selected;
                    selected.task_name = std::string(task_name);
                    settings.pi.tasks.emplace_back(std::move(selected));
                    task = &settings.pi.tasks.back();
                }
                if (task)
                {
                    if (value.is_null())
                    {
                        RemoveOption(task->options, option_name);
                        return;
                    }
                    if (const auto* option = FindOption(*catalog, option_name))
                    {
                        auto parsed = ParseOptionValue(*option, value);
                        if (parsed)
                        {
                            RemoveOption(task->options, option_name);
                            UpsertOption(task->options, std::move(*parsed));
                        }
                    }
                }
                return;
            }

            constexpr std::string_view kEnabledSuffix = ".enabled";
            if (tail.ends_with(kEnabledSuffix) && value.is_bool())
            {
                auto task_name =
                    tail.substr(0, tail.size() - kEnabledSuffix.size());
                auto* task = FindSelectedTask(settings, task_name);
                if (!task && HasTask(*catalog, task_name))
                {
                    MaapiPiTaskSettingsDto selected;
                    selected.task_name = std::string(task_name);
                    settings.pi.tasks.emplace_back(std::move(selected));
                    task = &settings.pi.tasks.back();
                }
                if (task)
                {
                    task->enabled = value.as_bool().value_or(true);
                }
                return;
            }
        }
    }

    void ApplyPreset(
        AcceptedSettingsDto& settings,
        const PiCatalog&     catalog,
        std::string_view     preset_name)
    {
        const auto preset = std::find_if(
            catalog.presets.begin(),
            catalog.presets.end(),
            [&](const PiPreset& item) { return item.dto.name == preset_name; });
        if (preset == catalog.presets.end())
        {
            settings.pi.orphan_paths.emplace_back(
                std::string{"pi.presetName:"} + std::string{preset_name});
            return;
        }
        settings.pi.preset_name = std::string(preset_name);
        settings.pi.tasks.clear();
        for (const auto& task : preset->dto.task)
        {
            if (!HasTask(catalog, task.name))
            {
                settings.pi.orphan_paths.emplace_back(
                    std::string{"pi.tasks:"} + task.name);
                continue;
            }
            MaapiPiTaskSettingsDto task_settings;
            task_settings.task_name = task.name;
            task_settings.enabled = task.enabled;
            settings.pi.tasks.push_back(std::move(task_settings));
        }
    }
} // namespace Das::Plugins::DasMaaPi
