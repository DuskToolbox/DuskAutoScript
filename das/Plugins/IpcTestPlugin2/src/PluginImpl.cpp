#include "PluginImpl.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/fmt.h>

#define DAS_BUILD_SHARED

DAS_NS_BEGIN

// === DasComponentImpl ===

DasComponentImpl::DasComponentImpl(uint16_t session_id)
    : session_id_(session_id)
{
}

DasResult DasComponentImpl::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
        return DAS_E_INVALID_ARGUMENT;
    // 返回 IDasComponent 的 IID
    *p_out_guid = DasIidOf<PluginInterface::IDasComponent>();
    return DAS_S_OK;
}

DasResult DasComponentImpl::GetRuntimeClassName(IDasReadOnlyString** pp_out_name)
{
    if (!pp_out_name)
        return DAS_E_INVALID_ARGUMENT;
    static const char* class_name = "Das.ComponentImpl";
    return CreateIDasReadOnlyStringFromUtf8(class_name, pp_out_name);
}

DasResult DasComponentImpl::Dispatch(
    IDasReadOnlyString*                  p_function_name,
    ExportInterface::IDasVariantVector*  p_arguments,
    ExportInterface::IDasVariantVector** pp_out_result)
{
    if (!p_function_name || !pp_out_result)
        return DAS_E_INVALID_ARGUMENT;

    // Get method name from string
    const char* method_ptr = nullptr;
    if (DAS::IsFailed(p_function_name->GetUtf8(&method_ptr)))
        return DAS_E_INVALID_POINTER;
    if (!method_ptr)
        return DAS_E_INVALID_POINTER;

    std::string method = method_ptr;

    if (method == "echo")
        return HandleEcho(p_arguments, pp_out_result);
    else if (method == "compute")
        return HandleCompute(p_arguments, pp_out_result);
    else if (method == "getSessionInfo")
        return HandleGetSessionInfo(p_arguments, pp_out_result);

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
    std::ignore = session_id_;
    return DAS_S_OK;
}

// === DasComponentFactoryImpl ===

DasComponentFactoryImpl::DasComponentFactoryImpl(uint16_t session_id)
    : session_id_(session_id)
{
}

DasResult DasComponentFactoryImpl::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
        return DAS_E_INVALID_ARGUMENT;
    // 返回 IDasComponentFactory 的 IID
    *p_out_guid = DasIidOf<PluginInterface::IDasComponentFactory>();
    return DAS_S_OK;
}

DasResult DasComponentFactoryImpl::GetRuntimeClassName(IDasReadOnlyString** pp_out_name)
{
    if (!pp_out_name)
        return DAS_E_INVALID_ARGUMENT;
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
        return DAS_E_INVALID_ARGUMENT;

    if (component_iid != DasIidOf<PluginInterface::IDasComponent>())
        return DAS_E_NO_IMPLEMENTATION;

    auto* instance = new DasComponentImpl(session_id_);
    instance->AddRef();
    *pp_out_component = instance;
    return DAS_S_OK;
}

// === IpcTestPlugin2 ===

void IpcTestPlugin2::SetSessionId(uint16_t session_id)
{
    session_id_ = session_id;
}

DasResult IpcTestPlugin2::EnumFeature(
    const size_t                       index,
    PluginInterface::DasPluginFeature* p_out_feature)
{
    if (!p_out_feature)
        return DAS_E_INVALID_ARGUMENT;

    if (index == 0)
    {
        *p_out_feature = PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY;
        return DAS_S_OK;
    }

    return DAS_E_OUT_OF_RANGE;
}

DasResult IpcTestPlugin2::CreateFeatureInterface(
    size_t index,
    IDasBase** pp_out_interface)
{
    if (!pp_out_interface)
        return DAS_E_INVALID_ARGUMENT;

    if (index == 0)
    {
        auto* factory = new DasComponentFactoryImpl(session_id_);
        factory->AddRef();
        *pp_out_interface = factory;
        return DAS_S_OK;
    }

    return DAS_E_OUT_OF_RANGE;
}

DasResult IpcTestPlugin2::CanUnloadNow(bool* p_can_unload)
{
    if (!p_can_unload)
        return DAS_E_INVALID_ARGUMENT;
    *p_can_unload = true;
    return DAS_S_OK;
}

DAS_NS_END
