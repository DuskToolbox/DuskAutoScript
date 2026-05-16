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
        bool OptionalBool(
            const TObject&   obj,
            std::string_view key,
            bool             fallback)
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
                    settings.pi.preset_name =
                        OptionalString(*pi, "presetName");
                    settings.pi.orphan_paths = StringArray(*pi, "orphanPaths");
                    if (pi->contains(std::string_view("tasks")))
                    {
                        if (auto tasks =
                                (*pi)[std::string_view("tasks")].as_array())
                        {
                            for (auto it = tasks->begin(); it != tasks->end();
                                 ++it)
                            {
                                if (auto task_obj = it->as_object())
                                {
                                    MaapiPiTaskSettingsDto task;
                                    task.task_name =
                                        OptionalString(*task_obj, "taskName")
                                            .value_or("");
                                    task.enabled = OptionalBool(
                                        *task_obj,
                                        "enabled",
                                        true);
                                    if (!task.task_name.empty())
                                    {
                                        settings.pi.tasks.emplace_back(
                                            std::move(task));
                                    }
                                }
                            }
                        }
                    }
                }
            }
            return settings;
        }

        void AddDiagnosticJson(yyjson::value& arr, const PiDiagnosticDto& item)
        {
            yyjson::value diag(Object());
            auto          obj = diag.as_object();
            (*obj)[std::string_view("severity")] =
                JsonString(item.severity);
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
                (*obj)[std::string_view("name")] =
                    JsonString(item.dto.name);
                if (item.dto.label)
                {
                    (*obj)[std::string_view("label")] =
                        JsonString(*item.dto.label);
                }
                arr.as_array()->emplace_back(std::move(candidate));
            }
            return arr;
        }

        yyjson::value MakeField(
            std::string_view id,
            std::string_view kind,
            std::string_view label,
            std::string_view value_path,
            std::string_view source_level,
            std::string_view pi_ref = {})
        {
            yyjson::value field(Object());
            auto          obj = field.as_object();
            (*obj)[std::string_view("id")] = JsonString(id);
            (*obj)[std::string_view("kind")] = JsonString(kind);
            (*obj)[std::string_view("label")] = JsonString(label);
            (*obj)[std::string_view("valuePath")] = JsonString(value_path);
            (*obj)[std::string_view("sourceLevel")] = JsonString(source_level);
            if (!pi_ref.empty())
            {
                (*obj)[std::string_view("piRef")] = JsonString(pi_ref);
            }
            return field;
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
                arr.as_array()->emplace_back(std::move(value));
            }
            return arr;
        }

        bool HasTask(const PiCatalog& catalog, std::string_view name)
        {
            return std::any_of(
                catalog.tasks.begin(),
                catalog.tasks.end(),
                [&](const PiTask& task) {
                    return task.dto.name == name;
                });
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

    yyjson::value SerializeAcceptedSettings(
        const AcceptedSettingsDto& settings)
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
        (*adapter_obj)[std::string_view("executionPolicy")] =
            std::move(policy);
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
        (*obj)[std::string_view("version")] = 1;
        (*obj)[std::string_view("kind")] = "formSequence";
        (*obj)[std::string_view("revision")] = revision;

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

        yyjson::value form_sequence(Array());
        form_sequence.as_array()->emplace_back(MakeField(
            "adapter.interfacePath",
            "path",
            "ProjectInterface path",
            "adapter.interfacePath",
            "adapter"));
        form_sequence.as_array()->emplace_back(MakeField(
            "adapter.executionPolicy.failFast",
            "switch",
            "Fail fast",
            "adapter.executionPolicy.failFast",
            "adapter"));

        yyjson::value schema(Object());
        yyjson::value fields(Array());
        fields.as_array()->emplace_back(MakeField(
            "adapter.interfacePath",
            "path",
            "ProjectInterface path",
            "adapter.interfacePath",
            "adapter"));

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

            auto controller = MakeField(
                "pi.controllerName",
                "select",
                "Controller",
                "pi.controllerName",
                "pi",
                "controller");
            (*controller.as_object())[std::string_view("candidates")] =
                CandidateArray(catalog->controllers);
            fields.as_array()->emplace_back(std::move(controller));
            form_sequence.as_array()->emplace_back(MakeField(
                "pi.controllerName",
                "select",
                "Controller",
                "pi.controllerName",
                "pi",
                "controller"));

            auto resource = MakeField(
                "pi.resourceName",
                "select",
                "Resource",
                "pi.resourceName",
                "pi",
                "resource");
            (*resource.as_object())[std::string_view("candidates")] =
                CandidateArray(catalog->resources);
            fields.as_array()->emplace_back(std::move(resource));
            form_sequence.as_array()->emplace_back(MakeField(
                "pi.resourceName",
                "select",
                "Resource",
                "pi.resourceName",
                "pi",
                "resource"));

            auto preset = MakeField(
                "pi.presetName",
                "select",
                "Preset",
                "pi.presetName",
                "preset",
                "preset");
            (*preset.as_object())[std::string_view("candidates")] =
                CandidateArray(catalog->presets);
            fields.as_array()->emplace_back(std::move(preset));

            for (const auto& option : catalog->options)
            {
                auto field = MakeField(
                    std::string{"pi.options." + option.dto.name},
                    option.dto.type,
                    option.dto.label.value_or(option.dto.name),
                    std::string{"pi.options." + option.dto.name},
                    "pi",
                    std::string{"option:" + option.dto.name});
                auto field_obj = field.as_object();
                (*field_obj)[std::string_view("optionName")] =
                    JsonString(option.dto.name);
                (*field_obj)[std::string_view("sourceLevel")] =
                    std::find_if(
                        catalog->global_options.begin(),
                        catalog->global_options.end(),
                        [&](const PiOption& item) {
                            return item.dto.name == option.dto.name;
                        })
                            == catalog->global_options.end()
                        ? "task"
                        : "global";
                fields.as_array()->emplace_back(std::move(field));
            }
        }
        (*schema.as_object())[std::string_view("fields")] = std::move(fields);
        (*obj)[std::string_view("schema")] = std::move(schema);

        yyjson::value view(Object());
        (*view.as_object())[std::string_view("formSequence")] =
            std::move(form_sequence);
        (*obj)[std::string_view("view")] = std::move(view);
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
        (*obj)[std::string_view("diagnostics")] =
            std::move(diagnostics_json);

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
        const yyjson::value& change)
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
    }

    void ApplyPreset(
        AcceptedSettingsDto& settings,
        const PiCatalog&     catalog,
        std::string_view     preset_name)
    {
        const auto preset = std::find_if(
            catalog.presets.begin(),
            catalog.presets.end(),
            [&](const PiPreset& item) {
                return item.dto.name == preset_name;
            });
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
