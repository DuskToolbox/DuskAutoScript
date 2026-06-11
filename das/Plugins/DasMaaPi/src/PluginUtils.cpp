#include "PluginUtils.h"

#include <das/DasApi.h>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    DasPtr<ExportInterface::IDasJson> WrapJson(yyjson::value value)
    {
        auto serialized = Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return {};
        }
        DasPtr<ExportInterface::IDasJson> result;
        ParseDasJsonFromString(serialized->c_str(), result.Put());
        return result;
    }

    yyjson::value JsonString(std::string_view value)
    {
        return yyjson::value(std::string(value));
    }

    std::optional<yyjson::value> ReadJson(ExportInterface::IDasJson* json)
    {
        if (!json)
        {
            return std::nullopt;
        }
        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return std::nullopt;
        }
        const char* raw = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&raw)) || raw == nullptr)
        {
            return std::nullopt;
        }
        return Utils::ParseYyjsonFromString(raw);
    }

    std::optional<PiCatalog> TryParseCatalog(
        const AcceptedSettingsDto&    settings,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        if (!settings.adapter.interface_path
            || settings.adapter.interface_path->empty())
        {
            return std::nullopt;
        }

        auto result = ParseProjectInterface(
            {.interface_path = *settings.adapter.interface_path});
        diagnostics.insert(
            diagnostics.end(),
            result.catalog.diagnostics.begin(),
            result.catalog.diagnostics.end());
        if (!result.ok)
        {
            return std::nullopt;
        }
        return std::move(result.catalog);
    }

    yyjson::value BuildDocument(
        const AcceptedSettingsDto&    settings,
        int64_t                       revision,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        auto catalog = TryParseCatalog(settings, diagnostics);
        return ProjectAuthoringDocument(
            settings,
            catalog ? &*catalog : nullptr,
            diagnostics,
            revision);
    }

    yyjson::value BuildApplyResult(
        const AcceptedSettingsDto&   settings,
        int64_t                      revision,
        std::vector<PiDiagnosticDto> diagnostics)
    {
        auto          catalog = TryParseCatalog(settings, diagnostics);
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("acceptedProperties")] =
            SerializeAcceptedSettings(settings);
        (*obj)[std::string_view("migration")] = Das::Utils::MakeYyjsonObject();

        if (catalog)
        {
            (*obj)[std::string_view("sourceFingerprint")] =
                JsonString(catalog->name + ":" + catalog->version);
        }
        else
        {
            (*obj)[std::string_view("sourceFingerprint")] = "maapi-unparsed";
        }

        (*obj)[std::string_view("document")] = ProjectAuthoringDocument(
            settings,
            catalog ? &*catalog : nullptr,
            diagnostics,
            revision);

        yyjson::value diagnostics_json(Das::Utils::MakeYyjsonArray());
        for (const auto& diagnostic : diagnostics)
        {
            yyjson::value item(Das::Utils::MakeYyjsonObject());
            auto          item_obj = item.as_object();
            (*item_obj)[std::string_view("severity")] =
                JsonString(diagnostic.severity);
            (*item_obj)[std::string_view("code")] = JsonString(diagnostic.code);
            (*item_obj)[std::string_view("message")] =
                JsonString(diagnostic.message);
            if (diagnostic.source)
            {
                (*item_obj)[std::string_view("source")] =
                    JsonString(*diagnostic.source);
            }
            diagnostics_json.as_array()->emplace_back(std::move(item));
        }
        (*obj)[std::string_view("diagnostics")] = std::move(diagnostics_json);
        return result;
    }

    std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>
    DerivePortDefinitions(
        const PiCatalog&   catalog,
        const std::string& task_name)
    {
        std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto> ports;

        // Find the task in catalog
        const PiTask* task = nullptr;
        for (const auto& t : catalog.tasks)
        {
            if (t.dto.name == task_name)
            {
                task = &t;
                break;
            }
        }
        if (!task)
        {
            return ports;
        }

        // Iterate over the task's option references
        for (const auto& option_name : task->dto.option)
        {
            const PiOption* option = FindOption(catalog, option_name);
            if (!option)
            {
                continue;
            }

            const auto& dto = option->dto;

            // D-05 type mapping
            auto map_port_type = [](const PiOptionDto& opt) -> std::string
            {
                if (opt.type == "select")
                {
                    return "string";
                }
                if (opt.type == "checkbox")
                {
                    return "array<string>";
                }
                if (opt.type == "switch")
                {
                    return "bool";
                }
                if (opt.type == "input")
                {
                    if (!opt.inputs.empty() && opt.inputs[0].pipeline_type)
                    {
                        const auto& pt = *opt.inputs[0].pipeline_type;
                        if (pt == "int")
                        {
                            return "int";
                        }
                        if (pt == "bool")
                        {
                            return "bool";
                        }
                    }
                    return "string";
                }
                // Unknown type — safe fallback (T-03-05-01)
                return "string";
            };

            Core::GraphRuntime::Dto::GraphPortDefinitionDto port;
            port.port_id = dto.name;
            port.display_label = dto.label ? *dto.label : dto.name;
            port.port_type = map_port_type(dto);
            port.is_required = true;
            port.default_value = Das::Utils::MakeYyjsonObject();

            // Extract default_value
            if (dto.type == "select" && !dto.default_cases.empty())
            {
                port.default_value = yyjson::value(dto.default_cases[0]);
            }
            else if (
                dto.type == "input" && !dto.inputs.empty()
                && dto.inputs[0].default_value)
            {
                port.default_value =
                    yyjson::value(*dto.inputs[0].default_value);
            }
            else if (dto.type == "switch")
            {
                port.default_value = false;
            }

            ports.push_back(std::move(port));

            // For select: generate child ports for each case
            if (dto.type == "select")
            {
                for (const auto& case_item : dto.cases)
                {
                    Core::GraphRuntime::Dto::GraphPortDefinitionDto child_port;
                    child_port.port_id = dto.name + "_" + case_item.name;
                    child_port.display_label =
                        case_item.label ? *case_item.label : case_item.name;
                    child_port.port_type = "string";
                    child_port.is_required = false;
                    child_port.default_value = Das::Utils::MakeYyjsonObject();
                    ports.push_back(std::move(child_port));
                }
            }
        }

        return ports;
    }

    std::map<std::string, std::string> DerivePortMap(
        const PiCatalog&   catalog,
        const std::string& task_name)
    {
        std::map<std::string, std::string> port_map;

        const PiTask* task = nullptr;
        for (const auto& t : catalog.tasks)
        {
            if (t.dto.name == task_name)
            {
                task = &t;
                break;
            }
        }
        if (!task)
        {
            return port_map;
        }

        for (const auto& option_name : task->dto.option)
        {
            const PiOption* option = FindOption(catalog, option_name);
            if (!option)
            {
                continue;
            }

            port_map[option_name] = option_name;

            if (option->dto.type == "select")
            {
                for (const auto& case_item : option->dto.cases)
                {
                    port_map[option_name + "_" + case_item.name] = option_name;
                }
            }
        }

        return port_map;
    }

    yyjson::value BuildPortDefinitionsJson(
        const std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>&
            ports)
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        auto          array = arr.as_array();

        for (const auto& port : ports)
        {
            yyjson::value obj(Das::Utils::MakeYyjsonObject());
            auto          o = obj.as_object();
            (*o)[std::string_view("portId")] = std::make_pair(
                std::string_view(port.port_id),
                yyjson::copy_string);
            (*o)[std::string_view("displayLabel")] = std::make_pair(
                std::string_view(port.display_label),
                yyjson::copy_string);
            (*o)[std::string_view("portType")] = std::make_pair(
                std::string_view(port.port_type),
                yyjson::copy_string);
            (*o)[std::string_view("isRequired")] = port.is_required;

            // default_value: copy as yyjson value
            (*o)[std::string_view("defaultValue")] =
                Das::Utils::CloneYyjsonValue(port.default_value);

            yyjson::value tags(Das::Utils::MakeYyjsonArray());
            (*o)[std::string_view("tags")] = std::move(tags);

            array->emplace_back(std::move(obj));
        }

        return arr;
    }

    yyjson::value MakeAdapterOnlyDocument()
    {
        yyjson::value document(Das::Utils::MakeYyjsonObject());
        auto          obj = document.as_object();
        (*obj)[std::string_view("version")] = 1;
        (*obj)[std::string_view("kind")] = "formSequence";
        (*obj)[std::string_view("revision")] = 0;

        yyjson::value values(Das::Utils::MakeYyjsonObject());
        yyjson::value adapter(Das::Utils::MakeYyjsonObject());
        yyjson::value execution_policy(Das::Utils::MakeYyjsonObject());
        (*execution_policy.as_object())[std::string_view("failFast")] = true;
        (*adapter.as_object())[std::string_view("executionPolicy")] =
            std::move(execution_policy);
        (*values.as_object())[std::string_view("adapter")] = std::move(adapter);
        (*obj)[std::string_view("values")] = std::move(values);

        yyjson::value view(Das::Utils::MakeYyjsonObject());
        yyjson::value form_sequence(Das::Utils::MakeYyjsonArray());

        yyjson::value project_path(Das::Utils::MakeYyjsonObject());
        auto          project_path_obj = project_path.as_object();
        (*project_path_obj)[std::string_view("id")] = "adapter.projectPath";
        (*project_path_obj)[std::string_view("kind")] = "path";
        (*project_path_obj)[std::string_view("label")] =
            "MaaFramework project path";
        (*project_path_obj)[std::string_view("required")] = true;
        form_sequence.as_array()->emplace_back(std::move(project_path));

        yyjson::value fail_fast(Das::Utils::MakeYyjsonObject());
        auto          fail_fast_obj = fail_fast.as_object();
        (*fail_fast_obj)[std::string_view("id")] =
            "adapter.executionPolicy.failFast";
        (*fail_fast_obj)[std::string_view("kind")] = "switch";
        (*fail_fast_obj)[std::string_view("label")] = "Fail fast";
        form_sequence.as_array()->emplace_back(std::move(fail_fast));

        (*view.as_object())[std::string_view("formSequence")] =
            std::move(form_sequence);
        (*obj)[std::string_view("view")] = std::move(view);

        yyjson::value schema(Das::Utils::MakeYyjsonObject());
        (*schema.as_object())[std::string_view("acceptedSettingsVersion")] = 1;
        (*obj)[std::string_view("schema")] = std::move(schema);
        (*obj)[std::string_view("catalog")] = Das::Utils::MakeYyjsonObject();
        (*obj)[std::string_view("state")] = Das::Utils::MakeYyjsonObject();
        (*obj)[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();
        (*obj)[std::string_view("migration")] = Das::Utils::MakeYyjsonObject();
        return document;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
