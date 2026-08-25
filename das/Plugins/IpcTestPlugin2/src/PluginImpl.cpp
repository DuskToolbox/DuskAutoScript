#include "PluginImpl.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/fmt.h>

#define DAS_BUILD_SHARED

DAS_NS_BEGIN

namespace
{
    // 跨 host 测试约定的 component guid：与 IpcTestPlugin1 侧的同名常量
    // 逐字节一致（TestTaskComponentHostFeature 只认这个 guid）。
    constexpr DasGuid IPC_HELD_TASK_COMPONENT_GUID{
        0x26B2F2BC,
        0xFC73,
        0x4527,
        {0xB3, 0x9B, 0x89, 0xB4, 0x85, 0x3B, 0x1D, 0xE0}};
} // namespace

// === DasComponentImpl ===

DasResult DasComponentImpl::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    // 返回 IDasComponent 的 IID
    *p_out_guid = DasIidOf<PluginInterface::IDasComponent>();
    return DAS_S_OK;
}

DasResult DasComponentImpl::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (!pp_out_name)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    static const char* class_name = "Das.ComponentImpl";
    return CreateIDasReadOnlyStringFromUtf8(class_name, pp_out_name);
}

DasResult DasComponentImpl::Dispatch(
    IDasReadOnlyString*                  p_function_name,
    ExportInterface::IDasVariantVector*  p_arguments,
    ExportInterface::IDasVariantVector** pp_out_result)
{
    if (!p_function_name || !pp_out_result)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // Get method name from string
    const char* method_ptr = nullptr;
    if (DAS::IsFailed(p_function_name->GetUtf8(&method_ptr)))
    {
        return DAS_E_INVALID_POINTER;
    }
    if (!method_ptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    const std::string method = method_ptr;

    if (method == "echo")
    {
        return HandleEcho(p_arguments, pp_out_result);
    }
    else if (method == "compute")
    {
        return HandleCompute(p_arguments, pp_out_result);
    }
    else if (method == "getSessionInfo")
    {
        return HandleGetSessionInfo(p_arguments, pp_out_result);
    }

    return DAS_E_INVALID_ARGUMENT;
}

DasResult DasComponentImpl::HandleEcho(
    ExportInterface::IDasVariantVector*  args,
    ExportInterface::IDasVariantVector** out)
{
    // TODO: Implement echo - return input string with prefix
    std::ignore = args;
    std::ignore = out;
    return DAS_S_OK;
}

DasResult DasComponentImpl::HandleCompute(
    ExportInterface::IDasVariantVector*  args,
    ExportInterface::IDasVariantVector** out)
{
    // TODO: Implement compute - perform calculation and return result as
    // IDasJson
    std::ignore = args;
    std::ignore = out;
    return DAS_S_OK;
}

DasResult DasComponentImpl::HandleGetSessionInfo(
    ExportInterface::IDasVariantVector*  args,
    ExportInterface::IDasVariantVector** out)
{
    // TODO: Return session_id as part of result
    std::ignore = args;
    std::ignore = out;
    return DAS_S_OK;
}

// === DasComponentFactoryImpl ===

DasResult DasComponentFactoryImpl::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    // 返回 IDasComponentFactory 的 IID
    *p_out_guid = DasIidOf<PluginInterface::IDasComponentFactory>();
    return DAS_S_OK;
}

DasResult DasComponentFactoryImpl::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (!pp_out_name)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    static const char* class_name = "Das.ComponentFactoryImpl";
    return CreateIDasReadOnlyStringFromUtf8(class_name, pp_out_name);
}

DasResult DasComponentFactoryImpl::IsSupported(const DasGuid& component_iid)
{
    // Check if the requested component IID is supported
    // For testing, we support IDasComponent
    if (component_iid == DasIidOf<PluginInterface::IDasComponent>())
    {
        return DAS_S_OK;
    }
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult DasComponentFactoryImpl::CreateInstance(
    const DasGuid&                   component_iid,
    PluginInterface::IDasComponent** pp_out_component)
{
    if (!pp_out_component)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (component_iid != DasIidOf<PluginInterface::IDasComponent>())
    {
        return DAS_E_NO_IMPLEMENTATION;
    }

    auto* instance = new DasComponentImpl();
    instance->AddRef();
    *pp_out_component = instance;
    return DAS_S_OK;
}

// === EchoTaskComponent ===

DasResult EchoTaskComponent::ApplySettingsChange(
    ExportInterface::IDasJson*  p_request_json,
    ExportInterface::IDasJson** pp_out_result_json)
{
    if (!p_request_json || !pp_out_result_json)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 回显：请求 JSON 原样写入出参（跨 host 大消息链的靶）。
    DasPtr<IDasReadOnlyString> request_text;
    DasResult hr = p_request_json->ToString(0, request_text.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    const char* request_utf8 = nullptr;
    hr = request_text->GetUtf8(&request_utf8);
    if (DAS::IsFailed(hr) || !request_utf8)
    {
        return DAS_E_INVALID_POINTER;
    }
    return ParseDasJsonFromString(request_utf8, pp_out_result_json);
}

DasResult EchoTaskComponent::Do(
    PluginInterface::IDasStopToken*,
    ExportInterface::IDasReadOnlyPortMap*,
    ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (pp_out_port_map)
    {
        *pp_out_port_map = nullptr;
    }
    return DAS_E_NO_IMPLEMENTATION;
}

// === TestTaskComponentHostFeature ===

DasResult TestTaskComponentHostFeature::CreateTaskComponent(
    const DasGuid&                       component_guid,
    PluginInterface::IDasTaskComponent** pp_out_component)
{
    if (!pp_out_component)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *pp_out_component = nullptr;

    if (component_guid != IPC_HELD_TASK_COMPONENT_GUID)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    auto* component = new EchoTaskComponent();
    component->AddRef();
    *pp_out_component = component;
    return DAS_S_OK;
}

// === IpcTestPlugin2 ===

DasResult IpcTestPlugin2::EnumFeature(
    const size_t                       index,
    PluginInterface::DasPluginFeature* p_out_feature)
{
    if (!p_out_feature)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (index == 0)
    {
        *p_out_feature = PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY;
        return DAS_S_OK;
    }

    if (index == 1)
    {
        // feature 对象实现 IDasTaskComponentHost——现有 feature 枚举里
        // 最贴近"提供 task component 创建能力"的取值。
        *p_out_feature =
            PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
        return DAS_S_OK;
    }

    return DAS_E_OUT_OF_RANGE;
}

DasResult IpcTestPlugin2::CreateFeatureInterface(
    size_t     index,
    IDasBase** pp_out_interface)
{
    if (!pp_out_interface)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (index == 0)
    {
        auto* factory = new DasComponentFactoryImpl();
        factory->AddRef();
        *pp_out_interface = factory;
        return DAS_S_OK;
    }

    if (index == 1)
    {
        auto* host_feature = new TestTaskComponentHostFeature();
        host_feature->AddRef();
        *pp_out_interface =
            static_cast<PluginInterface::IDasTaskComponentHost*>(host_feature);
        return DAS_S_OK;
    }

    return DAS_E_OUT_OF_RANGE;
}

DAS_NS_END
