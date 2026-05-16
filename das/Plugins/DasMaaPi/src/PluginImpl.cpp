#define DAS_BUILD_SHARED

#include "PluginImpl.h"

#include <das/DasApi.h>

#include <array>
#include <new>

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
        PluginInterface::IDasStopToken*,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson*)
    {
        return DAS_E_NO_IMPLEMENTATION;
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

    DasResult MaapiAuthoringSession::GetDocument(
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_document_json)
    {
        if (!pp_out_document_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        auto wrapped = WrapJson(MakeAdapterOnlyDocument());
        *pp_out_document_json = wrapped.Get();
        (*pp_out_document_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::ApplyChange(
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        yyjson::value accepted(Das::Utils::MakeYyjsonObject());
        yyjson::value adapter(Das::Utils::MakeYyjsonObject());
        yyjson::value execution_policy(Das::Utils::MakeYyjsonObject());
        (*execution_policy.as_object())[std::string_view("failFast")] = true;
        (*adapter.as_object())[std::string_view("executionPolicy")] =
            std::move(execution_policy);
        (*accepted.as_object())[std::string_view("adapter")] =
            std::move(adapter);

        auto obj = result.as_object();
        (*obj)[std::string_view("acceptedProperties")] = std::move(accepted);
        (*obj)[std::string_view("sourceFingerprint")] =
            "maapi-adapter-scaffold";
        (*obj)[std::string_view("migration")] = Das::Utils::MakeYyjsonObject();

        auto wrapped = WrapJson(std::move(result));
        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::Compile(
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("ok")] = false;
        (*obj)[std::string_view("summary")] =
            "Maa ProjectInterface path is not configured";
        (*obj)[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();

        auto wrapped = WrapJson(std::move(result));
        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSessionFactory::CreateSession(
        const DasGuid&,
        ExportInterface::IDasJson*,
        PluginInterface::IDasTaskAuthoringSession** pp_out_session)
    {
        if (!pp_out_session)
        {
            return DAS_E_INVALID_POINTER;
        }
        auto* session = new MaapiAuthoringSession();
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
