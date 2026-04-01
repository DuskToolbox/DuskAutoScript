#define DAS_BUILD_SHARED

#include "PluginImpl.h"
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>

#include <array>
#include <string>

DAS_NS_BEGIN

// === DasTouchMockImpl ===

DasTouchMockImpl::DasTouchMockImpl(uint16_t session_id)
    : session_id_(session_id)
{
}

DasResult DasTouchMockImpl::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    // 返回 IDasTouch 的 IID
    *p_out_guid = DasIidOf<PluginInterface::IDasTouch>();
    return DAS_S_OK;
}

DasResult DasTouchMockImpl::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (!pp_out_name)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    // 创建类名字符串
    static const char* class_name = "Das.TouchMock";
    return CreateIDasReadOnlyStringFromUtf8(class_name, pp_out_name);
}

DasResult DasTouchMockImpl::Click(int32_t x, int32_t y)
{
    // Mock: 记录点击位置，返回成功
    std::ignore = x;
    std::ignore = y;
    DAS_LOG_INFO("Mock Click called");
    return DAS_S_OK;
}

DasResult DasTouchMockImpl::Swipe(
    PluginInterface::DasPoint from,
    PluginInterface::DasPoint to,
    int32_t                   duration_ms)
{
    // Mock: 记录滑动，返回成功
    std::ignore = from;
    std::ignore = to;
    std::ignore = duration_ms;
    DAS_LOG_INFO("Mock Swipe called");
    return DAS_S_OK;
}

// === DasQueryComponentImpl ===

DasQueryComponentImpl::DasQueryComponentImpl(uint16_t session_id)
    : session_id_(session_id)
{
}

DasResult DasQueryComponentImpl::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *p_out_guid = DasIidOf<PluginInterface::IDasComponent>();
    return DAS_S_OK;
}

DasResult DasQueryComponentImpl::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (!pp_out_name)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    static const char* class_name = "Das.QueryComponentImpl";
    return CreateIDasReadOnlyStringFromUtf8(class_name, pp_out_name);
}

DasResult DasQueryComponentImpl::Dispatch(
    IDasReadOnlyString*                  p_function_name,
    ExportInterface::IDasVariantVector*  p_arguments,
    ExportInterface::IDasVariantVector** pp_out_result)
{
    std::ignore = p_arguments;
    if (!p_function_name || !pp_out_result)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    const char* method_ptr = nullptr;
    if (DAS::IsFailed(p_function_name->GetUtf8(&method_ptr)))
    {
        return DAS_E_INVALID_POINTER;
    }
    if (!method_ptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    std::string method = method_ptr;

    if (method == "queryMainProcessString")
    {
        return HandleQueryMainProcessString(pp_out_result);
    }

    if (method == "queryMainProcessVariantVector")
    {
        return HandleQueryMainProcessVariantVector(pp_out_result);
    }

    return DAS_E_INVALID_ARGUMENT;
}

DasResult DasQueryComponentImpl::HandleQueryMainProcessString(
    ExportInterface::IDasVariantVector** pp_out_result)
{
    if (!pp_out_result)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 调用 C API 获取主进程中注册的 IDasReadOnlyString
    IDasBase* raw_obj = nullptr;
    DasResult hr =
        DasQueryMainProcessInterface(DasIidOf<IDasReadOnlyString>(), &raw_obj);
    if (DAS::IsFailed(hr) || !raw_obj)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "DasQueryMainProcessInterface failed, hr = {:#x}",
            static_cast<uint32_t>(hr));
        DAS_LOG_ERROR(err_msg.c_str());
        return hr;
    }

    // 获取字符串内容
    const char* str = nullptr;
    hr = static_cast<IDasReadOnlyString*>(raw_obj)->GetUtf8(&str);
    if (DAS::IsFailed(hr))
    {
        DAS_LOG_ERROR("GetUtf8 failed on main process string");
        raw_obj->Release();
        return hr;
    }

    // 打印获取到的字符串（用于验证）
    std::string log_msg = DAS_FMT_NS::format(
        "queryMainProcessString got: {}",
        str ? str : "(null)");
    DAS_LOG_INFO(log_msg.c_str());

    // 创建结果 VariantVector 并回传读取到的字符串
    DAS::DasPtr<IDasReadOnlyString> return_string;
    hr = CreateIDasReadOnlyStringFromUtf8(str, return_string.Put());
    if (DAS::IsFailed(hr))
    {
        DAS_LOG_ERROR("CreateIDasReadOnlyStringFromUtf8 failed");
        raw_obj->Release();
        return hr;
    }

    raw_obj->Release();

    hr = CreateIDasVariantVector(pp_out_result);
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    hr = (*pp_out_result)->PushBackString(return_string.Get());
    return hr;
}

DasResult DasQueryComponentImpl::HandleQueryMainProcessVariantVector(
    ExportInterface::IDasVariantVector** pp_out_result)
{
    if (!pp_out_result)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 调用 C API 获取主进程中注册的 IDasVariantVector
    IDasBase* raw_obj = nullptr;
    DasResult hr = DasQueryMainProcessInterface(
        DasIidOf<ExportInterface::IDasVariantVector>(),
        &raw_obj);
    if (DAS::IsFailed(hr) || !raw_obj)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "DasQueryMainProcessInterface(IDasVariantVector) failed, "
            "hr = {:#x}",
            static_cast<uint32_t>(hr));
        DAS_LOG_ERROR(err_msg.c_str());
        return hr;
    }

    // 转换为 IDasVariantVector 并读取数据
    auto* variant_vec =
        static_cast<ExportInterface::IDasVariantVector*>(raw_obj);

    int64_t int_val = 0;
    hr = variant_vec->GetInt(0, &int_val);
    if (DAS::IsFailed(hr))
    {
        DAS_LOG_ERROR("GetInt(0) failed on main process VariantVector");
        raw_obj->Release();
        return hr;
    }

    std::string log_msg = DAS_FMT_NS::format(
        "queryMainProcessVariantVector got Int[0] = {}",
        int_val);
    DAS_LOG_INFO(log_msg.c_str());

    raw_obj->Release();

    // 创建结果 VariantVector 并回传读取到的值
    hr = CreateIDasVariantVector(pp_out_result);
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    hr = (*pp_out_result)->PushBackInt(int_val);
    return hr;
}

// === DasQueryComponentFactoryImpl ===

DasQueryComponentFactoryImpl::DasQueryComponentFactoryImpl(uint16_t session_id)
    : session_id_(session_id)
{
}

DasResult DasQueryComponentFactoryImpl::GetGuid(DasGuid* p_out_guid)
{
    if (!p_out_guid)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *p_out_guid = DasIidOf<PluginInterface::IDasComponentFactory>();
    return DAS_S_OK;
}

DasResult DasQueryComponentFactoryImpl::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (!pp_out_name)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    static const char* class_name = "Das.QueryComponentFactoryImpl";
    return CreateIDasReadOnlyStringFromUtf8(class_name, pp_out_name);
}

DasResult DasQueryComponentFactoryImpl::IsSupported(
    const DasGuid& component_iid)
{
    if (component_iid == DasIidOf<PluginInterface::IDasComponent>())
    {
        return DAS_S_OK;
    }
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult DasQueryComponentFactoryImpl::CreateInstance(
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

    auto* instance = new DasQueryComponentImpl(session_id_);
    instance->AddRef();
    *pp_out_component = instance;
    return DAS_S_OK;
}

// === IpcTestPlugin1 ===

void IpcTestPlugin1::SetSessionId(uint16_t session_id)
{
    session_id_ = session_id;
}

DasResult IpcTestPlugin1::EnumFeature(
    const size_t                       index,
    PluginInterface::DasPluginFeature* p_out_feature)
{
    static std::array features{
        PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY,
        PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY};
    try
    {
        const auto result = features.at(index);
        *p_out_feature = result;
        return DAS_S_OK;
    }
    catch (const std::out_of_range& ex)
    {
        DAS_LOG_ERROR(ex.what());
        return DAS_E_OUT_OF_RANGE;
    }
}

DasResult IpcTestPlugin1::CreateFeatureInterface(
    size_t     index,
    IDasBase** pp_out_interface)
{
    if (!pp_out_interface)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (index > 1)
    {
        return DAS_E_OUT_OF_RANGE;
    }

    if (index == 0)
    {
        // 创建 DasTouchMockImpl 实例
        auto* touch_impl = new DasTouchMockImpl(session_id_);
        touch_impl->AddRef();
        *pp_out_interface =
            static_cast<PluginInterface::IDasTouch*>(touch_impl);
    }
    else
    {
        // index == 1: 创建 DasQueryComponentFactoryImpl
        auto* factory = new DasQueryComponentFactoryImpl(session_id_);
        factory->AddRef();
        *pp_out_interface =
            static_cast<PluginInterface::IDasComponentFactory*>(factory);
    }

    return DAS_S_OK;
}

DasResult IpcTestPlugin1::CanUnloadNow(bool* p_can_unload)
{
    if (!p_can_unload)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *p_can_unload = true;
    return DAS_S_OK;
}

DAS_NS_END
