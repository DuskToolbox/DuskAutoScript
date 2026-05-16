#define DAS_BUILD_SHARED

#include "PluginImpl.h"

#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Plugins/DasMaaPi/PiParser.h>

#include <array>
#include <new>
#include <optional>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
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

        std::optional<yyjson::value> ReadJson(
            ExportInterface::IDasJson* json)
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
            const AcceptedSettingsDto&          settings,
            std::vector<PiDiagnosticDto>&       diagnostics)
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
            const AcceptedSettingsDto&          settings,
            int64_t                             revision,
            std::vector<PiDiagnosticDto>&       diagnostics)
        {
            auto catalog = TryParseCatalog(settings, diagnostics);
            return ProjectAuthoringDocument(
                settings,
                catalog ? &*catalog : nullptr,
                diagnostics,
                revision);
        }

        yyjson::value BuildApplyResult(
            const AcceptedSettingsDto&          settings,
            int64_t                             revision,
            std::vector<PiDiagnosticDto>        diagnostics)
        {
            auto catalog = TryParseCatalog(settings, diagnostics);
            yyjson::value result(Das::Utils::MakeYyjsonObject());
            auto          obj = result.as_object();
            (*obj)[std::string_view("acceptedProperties")] =
                SerializeAcceptedSettings(settings);
            (*obj)[std::string_view("migration")] =
                Das::Utils::MakeYyjsonObject();

            if (catalog)
            {
                (*obj)[std::string_view("sourceFingerprint")] =
                    JsonString(catalog->name + ":" + catalog->version);
            }
            else
            {
                (*obj)[std::string_view("sourceFingerprint")] =
                    "maapi-unparsed";
            }

            (*obj)[std::string_view("document")] =
                ProjectAuthoringDocument(
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
                (*item_obj)[std::string_view("code")] =
                    JsonString(diagnostic.code);
                (*item_obj)[std::string_view("message")] =
                    JsonString(diagnostic.message);
                if (diagnostic.source)
                {
                    (*item_obj)[std::string_view("source")] =
                        JsonString(*diagnostic.source);
                }
                diagnostics_json.as_array()->emplace_back(std::move(item));
            }
            (*obj)[std::string_view("diagnostics")] =
                std::move(diagnostics_json);
            return result;
        }
    } // namespace

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

    DasResult MaapiTask::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson* p_task_settings_json)
    {
        if (!p_task_settings_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        auto request = ReadJson(p_task_settings_json);
        if (!request || !request->is_object())
        {
            return DAS_E_INVALID_JSON;
        }
        auto parsed = ParseExecutionEnvelope(*request);
        if (DAS::IsFailed(parsed.result))
        {
            return parsed.result;
        }

        auto result = MaaRuntime::Run(
            parsed.envelope,
            MaaApiBoundaryForRuntime(),
            stop_token);
        return result.das_result;
    }

    DasResult MaapiTask::GetNextExecutionTime(
        ExportInterface::DasDate* p_out_date)
    {
        if (!p_out_date)
        {
            return DAS_E_INVALID_POINTER;
        }
        return DAS_E_NO_IMPLEMENTATION;
    }

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
        auto wrapped = WrapJson(BuildDocument(settings_, revision_, diagnostics));
        *pp_out_document_json = wrapped.Get();
        (*pp_out_document_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::ApplyChange(
        ExportInterface::IDasJson* p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (auto request = ReadJson(p_request_json))
        {
            if (auto obj = request->as_object())
            {
                const auto kind =
                    obj->contains(std::string_view("kind"))
                        ? (*obj)[std::string_view("kind")]
                              .as_string()
                              .value_or("")
                        : std::string_view{};
                if (kind == "setValue")
                {
                    std::vector<PiDiagnosticDto> diagnostics;
                    auto catalog = TryParseCatalog(settings_, diagnostics);
                    ApplySetValueChange(
                        settings_,
                        *request,
                        catalog ? &*catalog : nullptr);
                }
                else if (kind == "applyPreset")
                {
                    std::vector<PiDiagnosticDto> diagnostics;
                    auto catalog = TryParseCatalog(settings_, diagnostics);
                    if (catalog
                        && obj->contains(std::string_view("payload")))
                    {
                        if (auto payload =
                                (*obj)[std::string_view("payload")]
                                    .as_object())
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

        auto wrapped =
            WrapJson(BuildApplyResult(settings_, revision_, {}));
        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::Compile(
        ExportInterface::IDasJson* p_request_json,
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
                    (*obj)[std::string_view("purpose")]
                        .as_string()
                        .value_or("preview"));
            }
        }

        std::vector<PiDiagnosticDto> diagnostics;
        auto catalog = TryParseCatalog(settings_, diagnostics);
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

    DasResult MaapiAuthoringSessionFactory::CreateSession(
        const DasGuid&,
        ExportInterface::IDasJson* p_context_json,
        PluginInterface::IDasTaskAuthoringSession** pp_out_session)
    {
        if (!pp_out_session)
        {
            return DAS_E_INVALID_POINTER;
        }
        AcceptedSettingsDto settings;
        if (auto context = ReadJson(p_context_json))
        {
            settings = ParseAcceptedSettings(*context);
        }

        auto* session = new MaapiAuthoringSession(std::move(settings));
        session->AddRef();
        *pp_out_session = session;
        return DAS_S_OK;
    }

    DasResult DasMaaPiPlugin::EnumFeature(
        size_t                             index,
        PluginInterface::DasPluginFeature* p_out_feature)
    {
        if (!p_out_feature)
        {
            return DAS_E_INVALID_POINTER;
        }

        static constexpr std::array features{
            PluginInterface::DAS_PLUGIN_FEATURE_TASK,
            PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY};
        if (index >= features.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        *p_out_feature = features[index];
        return DAS_S_OK;
    }

    DasResult DasMaaPiPlugin::CreateFeatureInterface(
        size_t     index,
        IDasBase** pp_out_interface)
    {
        if (!pp_out_interface)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_interface = nullptr;

        try
        {
            if (index == 0)
            {
                auto* task = new MaapiTask();
                task->AddRef();
                *pp_out_interface =
                    static_cast<PluginInterface::IDasTask*>(task);
                return DAS_S_OK;
            }
            if (index == 1)
            {
                auto* factory = new MaapiAuthoringSessionFactory();
                factory->AddRef();
                *pp_out_interface = static_cast<
                    PluginInterface::IDasTaskAuthoringSessionFactory*>(factory);
                return DAS_S_OK;
            }
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }

        return DAS_E_OUT_OF_RANGE;
    }

    DasResult DasMaaPiPlugin::CanUnloadNow(bool* p_can_unload)
    {
        if (!p_can_unload)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_can_unload = true;
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
