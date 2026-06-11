#define DAS_BUILD_SHARED

#include "MaapiAuthoringSession.h"

#include "PluginUtils.h"

#include <das/Plugins/DasMaaPi/PiCompiler.h>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    MaapiAuthoringSession::MaapiAuthoringSession(AcceptedSettingsDto settings)
        : settings_(std::move(settings))
    {
    }

    DasResult MaapiAuthoringSession::GetDocument(
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_document_json)
    {
        if (!pp_out_document_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        std::vector<PiDiagnosticDto> diagnostics;
        auto                         wrapped =
            WrapJson(BuildDocument(settings_, revision_, diagnostics));
        *pp_out_document_json = wrapped.Get();
        (*pp_out_document_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::ApplyChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        // Parse catalog once — shared across setValue/applyPreset and port
        // derivation
        std::vector<PiDiagnosticDto> diagnostics;
        auto catalog = TryParseCatalog(settings_, diagnostics);

        if (auto request = ReadJson(p_request_json))
        {
            if (auto obj = request->as_object())
            {
                const auto kind =
                    obj->contains(std::string_view("kind"))
                        ? (*obj)[std::string_view("kind")].as_string().value_or(
                              "")
                        : std::string_view{};
                if (kind == "setValue")
                {
                    ApplySetValueChange(
                        settings_,
                        *request,
                        catalog ? &*catalog : nullptr);
                }
                else if (kind == "applyPreset")
                {
                    if (catalog && obj->contains(std::string_view("payload")))
                    {
                        if (auto payload =
                                (*obj)[std::string_view("payload")].as_object())
                        {
                            if (payload->contains(
                                    std::string_view("presetName")))
                            {
                                ApplyPreset(
                                    settings_,
                                    *catalog,
                                    (*payload)[std::string_view("presetName")]
                                        .as_string()
                                        .value_or(""));
                            }
                        }
                    }
                }
            }
        }
        ++revision_;

        auto result_json = BuildApplyResult(settings_, revision_, {});

        // Port derivation (D-05/D-06/D-07)
        if (catalog && !settings_.pi.tasks.empty())
        {
            const auto& task_name = settings_.pi.tasks[0].task_name;
            if (!task_name.empty())
            {
                auto port_definitions =
                    DerivePortDefinitions(*catalog, task_name);
                auto port_map = DerivePortMap(*catalog, task_name);

                // Inject dynamic_ports into the result (D-07)
                if (auto result_obj = result_json.as_object())
                {
                    (*result_obj)[std::string_view("dynamic_ports")] =
                        BuildPortDefinitionsJson(port_definitions);

                    // Inject portDefinitions and portMap into
                    // acceptedProperties
                    if (result_obj->contains(
                            std::string_view("acceptedProperties")))
                    {
                        if (auto accepted =
                                (*result_obj)[std::string_view(
                                                  "acceptedProperties")]
                                    .as_object())
                        {
                            (*accepted)[std::string_view("portDefinitions")] =
                                BuildPortDefinitionsJson(port_definitions);

                            yyjson::value port_map_json(
                                Das::Utils::MakeYyjsonObject());
                            auto pm_obj = port_map_json.as_object();
                            for (const auto& [port_id, param_name] : port_map)
                            {
                                (*pm_obj)[std::string_view(port_id)] =
                                    std::make_pair(
                                        std::string_view(param_name),
                                        yyjson::copy_string);
                            }
                            (*accepted)[std::string_view("portMap")] =
                                std::move(port_map_json);
                        }
                    }
                }
            }
        }

        auto wrapped = WrapJson(std::move(result_json));
        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::Compile(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        std::string purpose = "preview";
        if (auto request = ReadJson(p_request_json))
        {
            if (auto obj = request->as_object();
                obj && obj->contains(std::string_view("purpose"))
                && (*obj)[std::string_view("purpose")].is_string())
            {
                purpose = std::string(
                    (*obj)[std::string_view("purpose")].as_string().value_or(
                        "preview"));
            }
        }

        std::vector<PiDiagnosticDto> diagnostics;
        auto          catalog = TryParseCatalog(settings_, diagnostics);
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        if (!catalog)
        {
            auto obj = result.as_object();
            (*obj)[std::string_view("ok")] = false;
            (*obj)[std::string_view("canExecute")] = false;
            (*obj)[std::string_view("summary")] =
                Das::Utils::MakeYyjsonObject();
            yyjson::value diagnostics_json(Das::Utils::MakeYyjsonArray());
            for (const auto& diagnostic : diagnostics)
            {
                yyjson::value item(Das::Utils::MakeYyjsonObject());
                auto          item_obj = item.as_object();
                (*item_obj)[std::string_view("severity")] =
                    JsonString(diagnostic.severity);
                (*item_obj)[std::string_view("code")] =
                    JsonString(diagnostic.code);
                (*item_obj)[std::string_view("message")] =
                    JsonString(diagnostic.message);
                diagnostics_json.as_array()->emplace_back(std::move(item));
            }
            (*obj)[std::string_view("diagnostics")] =
                std::move(diagnostics_json);
        }
        else
        {
            result = SerializeCompileResult(
                CompileMaapi(settings_, *catalog, purpose),
                purpose);
        }

        auto wrapped = WrapJson(std::move(result));
        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
