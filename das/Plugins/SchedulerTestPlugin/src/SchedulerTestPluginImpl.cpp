#include "SchedulerTestPluginImpl.h"

#include <algorithm>
#include <any>
#include <array>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <string>
#include <string_view>

// dll 全局声明：定义在 DllMain.cpp（DasTestPlugin_SetSharedState 同 TU）。
// FactoryTaskPluginPackage 默认构造时取自此全局。
extern FactoryTaskSharedState* g_test_shared_state;

DAS_NS_BEGIN

namespace
{
    // 插件内 IDasJson 构造：IDasJsonImpl 的符号未从 DasCore.dll 导出，
    // 插件 dll 无法链接。改用导出的 ParseDasJsonFromString 经序列化往返
    // 创建 IDasJson（与 DasFlowControl/DasMaaPi 插件一致）。
    DasPtr<Das::ExportInterface::IDasJson> WrapJson(yyjson::value value)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return {};
        }
        DasPtr<Das::ExportInterface::IDasJson> result;
        ParseDasJsonFromString(serialized->c_str(), result.Put());
        return result;
    }
} // namespace

// === FactoryBackedTask ===

DasResult FactoryBackedTask::QueryInterface(const DasGuid& iid, void** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (iid == DasIidOf<IDasBase>())
    {
        *pp = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<IDasTypeInfo>())
    {
        *pp = static_cast<IDasTypeInfo*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<Das::PluginInterface::IDasTask>())
    {
        *pp = static_cast<Das::PluginInterface::IDasTask*>(this);
        AddRef();
        return DAS_S_OK;
    }
    *pp = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult FactoryBackedTask::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = FactoryTaskGuid;
    return DAS_S_OK;
}

DasResult FactoryBackedTask::GetRuntimeClassName(IDasReadOnlyString** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8("FactoryBackedTask", pp);
}

DasResult FactoryBackedTask::Do(
    Das::PluginInterface::IDasStopToken*,
    Das::ExportInterface::IDasJson*,
    Das::ExportInterface::IDasJson* p_task_settings_json)
{
    std::string key1_value;
    if (p_task_settings_json != nullptr)
    {
        DasPtr<IDasReadOnlyString> key;
        if (DAS_S_OK == CreateIDasReadOnlyStringFromUtf8("key1", key.Put())
            && key)
        {
            DasPtr<IDasReadOnlyString> value;
            if (DAS_S_OK
                    == p_task_settings_json->GetStringByName(
                        key.Get(),
                        value.Put())
                && value)
            {
                const char* raw = nullptr;
                if (DAS_S_OK == value->GetUtf8(&raw) && raw)
                {
                    key1_value = raw;
                }
            }
        }
    }
    {
        ipc_scoped_lock lock(state_->mutex);
        state_->add_executed_id(instance_id_);
        state_->set_last_props_key1_value(key1_value);
        state_->do_entered = true;
    }
    state_->cv.notify_all();

    ipc_scoped_lock lock(state_->mutex);
    state_->cv.wait(
        lock,
        [this] { return !state_->block_do || state_->unblock_do; });
    return DAS_S_OK;
}

DasResult FactoryBackedTask::GetNextExecutionTime(
    Das::ExportInterface::DasDate* p_out_date)
{
    if (!p_out_date)
    {
        return DAS_E_INVALID_POINTER;
    }

    *p_out_date = {2030, 1, 1, 0, 0, static_cast<uint8_t>(instance_id_)};
    return DAS_S_OK;
}

// === FakeAuthoringSession ===

DasResult FakeAuthoringSession::QueryInterface(const DasGuid& iid, void** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (iid == DasIidOf<IDasBase>())
    {
        *pp = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<IDasTypeInfo>())
    {
        *pp = static_cast<IDasTypeInfo*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<Das::PluginInterface::IDasTaskAuthoringSession>())
    {
        *pp =
            static_cast<Das::PluginInterface::IDasTaskAuthoringSession*>(this);
        AddRef();
        return DAS_S_OK;
    }
    *pp = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult FakeAuthoringSession::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = FactoryAuthoringSessionGuid;
    return DAS_S_OK;
}

DasResult FakeAuthoringSession::GetRuntimeClassName(IDasReadOnlyString** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8("FakeAuthoringSession", pp);
}

DasResult FakeAuthoringSession::GetDocument(
    Das::ExportInterface::IDasJson*,
    Das::ExportInterface::IDasJson** pp_out_document_json)
{
    if (!pp_out_document_json)
    {
        return DAS_E_INVALID_POINTER;
    }
    {
        ipc_scoped_lock lock(state_->mutex);
        ++state_->get_document_count;
    }

    yyjson::value document(Das::Utils::MakeYyjsonObject());
    auto          obj = document.as_object();
    (*obj)[std::string_view("version")] = 1;
    (*obj)[std::string_view("kind")] = "formSequence";
    (*obj)[std::string_view("revision")] = 0;
    (*obj)[std::string_view("values")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    (*obj)[std::string_view("view")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    (*obj)[std::string_view("schema")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    {
        yyjson::value catalog(Das::Utils::MakeYyjsonObject());
        (*catalog.as_object())[std::string_view("providerField")] = "preserved";
        (*obj)[std::string_view("catalog")] = std::move(catalog);
    }
    (*obj)[std::string_view("state")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    (*obj)[std::string_view("diagnostics")] =
        yyjson::value(Das::Utils::MakeYyjsonArray());
    (*obj)[std::string_view("migration")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    auto wrapped = WrapJson(std::move(document));
    *pp_out_document_json = wrapped.Get();
    (*pp_out_document_json)->AddRef();
    return DAS_S_OK;
}

DasResult FakeAuthoringSession::ApplyChange(
    Das::ExportInterface::IDasJson*,
    Das::ExportInterface::IDasJson** pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }
    {
        ipc_scoped_lock lock(state_->mutex);
        ++state_->apply_change_count;
        if (!state_->apply_ok)
        {
            return DAS_E_FAIL;
        }
    }

    yyjson::value result(Das::Utils::MakeYyjsonObject());
    yyjson::value props(Das::Utils::MakeYyjsonObject());
    (*props.as_object())[std::string_view("key1")] = "accepted";
    (*result.as_object())[std::string_view("acceptedProperties")] =
        std::move(props);
    (*result.as_object())[std::string_view("sourceFingerprint")] =
        "fake-source";
    (*result.as_object())[std::string_view("migration")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    auto wrapped = WrapJson(std::move(result));
    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult FakeAuthoringSession::Compile(
    Das::ExportInterface::IDasJson*  p_request_json,
    Das::ExportInterface::IDasJson** pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }
    {
        ipc_scoped_lock lock(state_->mutex);
        ++state_->compile_count;
        if (p_request_json)
        {
            DasPtr<IDasReadOnlyString> key;
            if (DAS_S_OK
                    == CreateIDasReadOnlyStringFromUtf8("purpose", key.Put())
                && key)
            {
                DasPtr<IDasReadOnlyString> purpose;
                if (DAS_S_OK
                        == p_request_json->GetStringByName(
                            key.Get(),
                            purpose.Put())
                    && purpose)
                {
                    const char* raw = nullptr;
                    if (DAS_S_OK == purpose->GetUtf8(&raw) && raw)
                    {
                        state_->set_last_compile_purpose(raw);
                    }
                }
            }
        }
    }
    yyjson::value result(Das::Utils::MakeYyjsonObject());
    yyjson::value execution_input(Das::Utils::MakeYyjsonObject());
    (*execution_input.as_object())[std::string_view("key1")] = "compiled";
    {
        ipc_scoped_lock lock(state_->mutex);
        (*result.as_object())[std::string_view("ok")] = state_->compile_ok;
        (*result.as_object())[std::string_view("canExecute")] =
            state_->compile_ok;
    }
    (*result.as_object())[std::string_view("executionInput")] =
        std::move(execution_input);
    {
        yyjson::value summary(Das::Utils::MakeYyjsonObject());
        yyjson::value task_names(Das::Utils::MakeYyjsonArray());
        (*task_names.as_array()).emplace_back("factoryTask");
        (*summary.as_object())[std::string_view("taskNames")] =
            std::move(task_names);
        (*summary.as_object())[std::string_view("requiresAgentRuntime")] =
            false;
        (*result.as_object())[std::string_view("summary")] = std::move(summary);
    }
    {
        yyjson::value diagnostics(Das::Utils::MakeYyjsonArray());
        yyjson::value diagnostic(Das::Utils::MakeYyjsonObject());
        (*diagnostic.as_object())[std::string_view("severity")] = "info";
        (*diagnostic.as_object())[std::string_view("code")] = "fake.compile";
        (*diagnostic.as_object())[std::string_view("message")] =
            "Fake compile completed";
        (*diagnostics.as_array()).emplace_back(std::move(diagnostic));
        (*result.as_object())[std::string_view("diagnostics")] =
            std::move(diagnostics);
    }
    (*result.as_object())[std::string_view("providerDebug")] = "internal-only";
    auto wrapped = WrapJson(std::move(result));
    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

// === FakeAuthoringSessionFactory ===

DasResult FakeAuthoringSessionFactory::QueryInterface(
    const DasGuid& iid,
    void**         pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (iid == DasIidOf<IDasBase>())
    {
        *pp = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<IDasTypeInfo>())
    {
        *pp = static_cast<IDasTypeInfo*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid
        == DasIidOf<Das::PluginInterface::IDasTaskAuthoringSessionFactory>())
    {
        *pp =
            static_cast<Das::PluginInterface::IDasTaskAuthoringSessionFactory*>(
                this);
        AddRef();
        return DAS_S_OK;
    }
    *pp = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult FakeAuthoringSessionFactory::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = factory_guid_;
    return DAS_S_OK;
}

DasResult FakeAuthoringSessionFactory::GetRuntimeClassName(
    IDasReadOnlyString** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8("FakeAuthoringSessionFactory", pp);
}

DasResult FakeAuthoringSessionFactory::CreateSession(
    const DasGuid&,
    Das::ExportInterface::IDasJson*                  p_context_json,
    Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
{
    if (!pp_out_session)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (!can_create_session_)
    {
        ipc_scoped_lock lock(state_->mutex);
        ++state_->decoy_authoring_create_count;
        return DAS_E_FAIL;
    }
    int64_t     revision = -1;
    int64_t     entry_id = -1;
    int64_t     task_id = -1;
    bool        had_task_id = false;
    std::string key1_value;
    if (p_context_json)
    {
        DasPtr<IDasReadOnlyString> key;
        if (DAS_S_OK == CreateIDasReadOnlyStringFromUtf8("revision", key.Put())
            && key)
        {
            p_context_json->GetIntByName(key.Get(), &revision);
        }

        key.Reset();
        if (DAS_S_OK == CreateIDasReadOnlyStringFromUtf8("entryId", key.Put())
            && key)
        {
            p_context_json->GetIntByName(key.Get(), &entry_id);
        }

        key.Reset();
        if (DAS_S_OK == CreateIDasReadOnlyStringFromUtf8("taskId", key.Put())
            && key)
        {
            had_task_id =
                DAS_S_OK == p_context_json->GetIntByName(key.Get(), &task_id);
        }

        key.Reset();
        if (DAS_S_OK
                == CreateIDasReadOnlyStringFromUtf8("properties", key.Put())
            && key)
        {
            DasPtr<Das::ExportInterface::IDasJson> properties;
            if (DAS_S_OK
                    == p_context_json->GetObjectRefByName(
                        key.Get(),
                        properties.Put())
                && properties)
            {
                DasPtr<IDasReadOnlyString> prop_key;
                if (DAS_S_OK
                        == CreateIDasReadOnlyStringFromUtf8(
                            "key1",
                            prop_key.Put())
                    && prop_key)
                {
                    DasPtr<IDasReadOnlyString> prop_value;
                    if (DAS_S_OK
                            == properties->GetStringByName(
                                prop_key.Get(),
                                prop_value.Put())
                        && prop_value)
                    {
                        const char* raw = nullptr;
                        if (DAS_S_OK == prop_value->GetUtf8(&raw) && raw)
                        {
                            key1_value = raw;
                        }
                    }
                }
            }
        }
    }
    {
        ipc_scoped_lock lock(state_->mutex);
        ++state_->authoring_session_count;
        state_->last_context_entry_id = entry_id;
        state_->last_context_task_id = task_id;
        state_->last_context_had_task_id = had_task_id;
        state_->last_context_revision = revision;
        state_->set_last_props_key1_value(key1_value);
    }

    auto* session = new FakeAuthoringSession(state_);
    session->AddRef();
    *pp_out_session = session;
    return DAS_S_OK;
}

// === FakeTaskComponent ===

DasResult FakeTaskComponent::QueryInterface(const DasGuid& iid, void** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (iid == DasIidOf<IDasBase>())
    {
        *pp = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<IDasTypeInfo>())
    {
        *pp = static_cast<IDasTypeInfo*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponent>())
    {
        *pp = static_cast<Das::PluginInterface::IDasTaskComponent*>(this);
        AddRef();
        return DAS_S_OK;
    }
    *pp = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult FakeTaskComponent::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = FactoryTaskComponentImplGuid;
    return DAS_S_OK;
}

DasResult FakeTaskComponent::GetRuntimeClassName(IDasReadOnlyString** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8("FakeTaskComponent", pp);
}

DasResult FakeTaskComponent::ApplySettingsChange(
    Das::ExportInterface::IDasJson*,
    Das::ExportInterface::IDasJson** pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }
    auto result = WrapJson(Das::Utils::MakeYyjsonObject());
    *pp_out_result_json = result.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult FakeTaskComponent::Do(
    Das::PluginInterface::IDasStopToken*,
    Das::ExportInterface::IDasReadOnlyPortMap*,
    Das::ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (!pp_out_port_map)
    {
        return DAS_E_INVALID_POINTER;
    }
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
    DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    *pp_out_port_map = output_map.Get();
    (*pp_out_port_map)->AddRef();
    return DAS_S_OK;
}

// === FakeTaskComponentFactory ===

DasResult FakeTaskComponentFactory::QueryInterface(
    const DasGuid& iid,
    void**         pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (iid == DasIidOf<IDasBase>())
    {
        *pp = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<IDasTypeInfo>())
    {
        *pp = static_cast<IDasTypeInfo*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>())
    {
        *pp =
            static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(this);
        AddRef();
        return DAS_S_OK;
    }
    *pp = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult FakeTaskComponentFactory::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = FactoryTaskComponentFactoryGuid;
    return DAS_S_OK;
}

DasResult FakeTaskComponentFactory::GetRuntimeClassName(IDasReadOnlyString** pp)
{
    if (!pp)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8("FakeTaskComponentFactory", pp);
}

DasResult FakeTaskComponentFactory::CreateComponent(
    const DasGuid&                            component_guid,
    Das::PluginInterface::IDasTaskComponent** pp_out_component)
{
    if (!pp_out_component)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_component = nullptr;

    static constexpr std::array kSupportedComponents{
        DasGuid{
            0x68F10001,
            0x0000,
            0x4000,
            {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        DasGuid{
            0x68F10002,
            0x0000,
            0x4000,
            {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}},
        DasGuid{
            0x68F10003,
            0x0000,
            0x4000,
            {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}},
        DasGuid{
            0x68F10004,
            0x0000,
            0x4000,
            {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
        DasGuid{
            0x68F10005,
            0x0000,
            0x4000,
            {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05}},
        DasGuid{
            0x68F10006,
            0x0000,
            0x4000,
            {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06}}};

    const auto supported = std::any_of(
        kSupportedComponents.begin(),
        kSupportedComponents.end(),
        [&component_guid](const DasGuid& supported_guid)
        { return component_guid == supported_guid; });
    if (!supported)
    {
        return DAS_E_NOT_FOUND;
    }

    auto* component = new FakeTaskComponent();
    component->AddRef();
    *pp_out_component = component;
    return DAS_S_OK;
}

// === FactoryTaskPluginPackage ===

FactoryTaskPluginPackage::FactoryTaskPluginPackage()
    : state_(g_test_shared_state)
{
}

DasResult FactoryTaskPluginPackage::QueryInterface(
    const DasGuid& iid,
    void**         pp_out)
{
    if (!pp_out)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (iid == DasIidOf<IDasBase>())
    {
        *pp_out = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }
    if (iid == DasIidOf<Das::PluginInterface::IDasPluginPackage>())
    {
        *pp_out = static_cast<Das::PluginInterface::IDasPluginPackage*>(this);
        AddRef();
        return DAS_S_OK;
    }
    *pp_out = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult FactoryTaskPluginPackage::EnumFeature(
    uint64_t                                index,
    Das::PluginInterface::DasPluginFeature* p_out_feature)
{
    if (!p_out_feature)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (index == 0)
    {
        *p_out_feature = Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK;
        return DAS_S_OK;
    }
    if (index == 1)
    {
        *p_out_feature =
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;
        return DAS_S_OK;
    }
    if (index == 2)
    {
        *p_out_feature =
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;
        return DAS_S_OK;
    }
    if (index == 3)
    {
        *p_out_feature =
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult FactoryTaskPluginPackage::CreateFeatureInterface(
    uint64_t   index,
    IDasBase** pp_out_interface)
{
    if (!pp_out_interface)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (index == 0)
    {
        DasOutPtr<IDasBase> result(pp_out_interface);
        auto*               task = new FactoryBackedTask(state_);
        result.Set(task);
        result.Keep();
        return DAS_S_OK;
    }
    if (index == 1)
    {
        DasOutPtr<IDasBase> result(pp_out_interface);
        auto*               factory = new FakeAuthoringSessionFactory(
            state_,
            DecoyAuthoringFactoryGuid,
            false);
        result.Set(factory);
        result.Keep();
        return DAS_S_OK;
    }
    if (index == 2)
    {
        DasOutPtr<IDasBase> result(pp_out_interface);
        auto*               factory = new FakeAuthoringSessionFactory(
            state_,
            FactoryAuthoringFactoryGuid,
            true);
        result.Set(factory);
        result.Keep();
        return DAS_S_OK;
    }
    if (index == 3)
    {
        DasOutPtr<IDasBase> result(pp_out_interface);
        auto*               factory = new FakeTaskComponentFactory();
        result.Set(factory);
        result.Keep();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DAS_NS_END
