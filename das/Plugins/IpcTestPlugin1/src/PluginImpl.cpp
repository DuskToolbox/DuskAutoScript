#define DAS_BUILD_SHARED

#include "PluginImpl.h"
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>

#include <array>
#include <cstring>
#include <string>

DAS_NS_BEGIN

namespace
{
    constexpr DasGuid kIpcTaskComponentGuid{
        0x68F10701,
        0x0000,
        0x4000,
        {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

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

    bool GuidEquals(const DasGuid& lhs, const DasGuid& rhs)
    {
        return std::memcmp(&lhs, &rhs, sizeof(DasGuid)) == 0;
    }
} // namespace

// === DasTouchMockImpl ===

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

    if (method == "queryMainProcessStringByName")
    {
        return HandleQueryMainProcessStringByName(p_arguments, pp_out_result);
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
    DAS::DasPtr<IDasBase> raw_obj;
    DasResult             hr = DasQueryMainProcessInterface(
        DasIidOf<IDasReadOnlyString>(),
        raw_obj.Put());
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
    hr = static_cast<IDasReadOnlyString*>(raw_obj.Get())->GetUtf8(&str);
    if (DAS::IsFailed(hr))
    {
        DAS_LOG_ERROR("GetUtf8 failed on main process string");
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
        return hr;
    }

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
    DAS::DasPtr<IDasBase> raw_obj;
    DasResult             hr = DasQueryMainProcessInterface(
        DasIidOf<ExportInterface::IDasVariantVector>(),
        raw_obj.Put());
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
        static_cast<ExportInterface::IDasVariantVector*>(raw_obj.Get());

    int64_t int_val = 0;
    hr = variant_vec->GetInt(0, &int_val);
    if (DAS::IsFailed(hr))
    {
        DAS_LOG_ERROR("GetInt(0) failed on main process VariantVector");
        return hr;
    }

    std::string log_msg = DAS_FMT_NS::format(
        "queryMainProcessVariantVector got Int[0] = {}",
        int_val);
    DAS_LOG_INFO(log_msg.c_str());

    // 创建结果 VariantVector 并回传读取到的值
    hr = CreateIDasVariantVector(pp_out_result);
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    hr = (*pp_out_result)->PushBackInt(int_val);
    return hr;
}

DasResult DasQueryComponentImpl::HandleQueryMainProcessStringByName(
    ExportInterface::IDasVariantVector*  p_arguments,
    ExportInterface::IDasVariantVector** pp_out_result)
{
    if (!pp_out_result || !p_arguments)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 从参数中提取 service name
    DAS::DasPtr<IDasReadOnlyString> name_str;
    DasResult hr = p_arguments->GetString(0, name_str.Put());
    if (DAS::IsFailed(hr) || !name_str)
    {
        DAS_LOG_ERROR("queryMainProcessStringByName: GetString(0) failed");
        return hr;
    }

    const char* name_ptr = nullptr;
    hr = name_str->GetUtf8(&name_ptr);
    if (DAS::IsFailed(hr) || !name_ptr)
    {
        DAS_LOG_ERROR("queryMainProcessStringByName: GetUtf8 failed");
        return hr;
    }

    std::string service_name = name_ptr;

    // 通过名称查询主进程中注册的接口
    DAS::DasPtr<IDasBase> raw_obj;
    hr =
        DasQueryMainProcessInterfaceByName(service_name.c_str(), raw_obj.Put());
    if (DAS::IsFailed(hr) || !raw_obj)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "DasQueryMainProcessInterfaceByName('{}') failed, hr = {:#x}",
            service_name,
            static_cast<uint32_t>(hr));
        DAS_LOG_ERROR(err_msg.c_str());
        return hr;
    }

    // 获取字符串内容
    const char* str = nullptr;
    hr = static_cast<IDasReadOnlyString*>(raw_obj.Get())->GetUtf8(&str);
    if (DAS::IsFailed(hr))
    {
        DAS_LOG_ERROR("GetUtf8 failed on main process string (ByName)");
        return hr;
    }

    std::string log_msg = DAS_FMT_NS::format(
        "queryMainProcessStringByName('{}') got: {}",
        service_name,
        str ? str : "(null)");
    DAS_LOG_INFO(log_msg.c_str());

    // 创建结果 VariantVector 并回传读取到的字符串
    std::string result_value = str ? str : "";

    DAS::DasPtr<IDasReadOnlyString> return_string;
    hr = CreateIDasReadOnlyStringFromUtf8(
        result_value.c_str(),
        return_string.Put());
    if (DAS::IsFailed(hr))
    {
        DAS_LOG_ERROR("CreateIDasReadOnlyStringFromUtf8 failed (ByName)");
        return hr;
    }

    hr = CreateIDasVariantVector(pp_out_result);
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    hr = (*pp_out_result)->PushBackString(return_string.Get());
    return hr;
}

// === DasQueryComponentFactoryImpl ===

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

    auto* instance = new DasQueryComponentImpl();
    instance->AddRef();
    *pp_out_component = instance;
    return DAS_S_OK;
}

// === IpcTaskAuthoringSessionImpl ===

DasResult IpcTaskAuthoringSessionImpl::GetDocument(
    ExportInterface::IDasJson*,
    ExportInterface::IDasJson** pp_out_document_json)
{
    if (!pp_out_document_json)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto document = Utils::MakeYyjsonObject();
    auto obj = *document.as_object();
    obj[std::string_view("version")] = 1;
    obj[std::string_view("kind")] = "formSequence";
    obj[std::string_view("revision")] = 0;
    obj[std::string_view("values")] = Utils::MakeYyjsonObject();
    obj[std::string_view("view")] = Utils::MakeYyjsonObject();
    obj[std::string_view("schema")] = Utils::MakeYyjsonObject();
    obj[std::string_view("catalog")] = Utils::MakeYyjsonObject();
    obj[std::string_view("state")] = Utils::MakeYyjsonObject();
    obj[std::string_view("diagnostics")] = Utils::MakeYyjsonArray();
    obj[std::string_view("migration")] = Utils::MakeYyjsonObject();

    auto wrapped = WrapJson(std::move(document));
    *pp_out_document_json = wrapped.Get();
    (*pp_out_document_json)->AddRef();
    return DAS_S_OK;
}

DasResult IpcTaskAuthoringSessionImpl::ApplyChange(
    ExportInterface::IDasJson*,
    ExportInterface::IDasJson** pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto result = Utils::MakeYyjsonObject();
    auto obj = *result.as_object();
    auto accepted = Utils::MakeYyjsonObject();
    (*accepted.as_object())[std::string_view("ipcAuthoringValue")] =
        "accepted";
    obj[std::string_view("acceptedProperties")] = std::move(accepted);
    obj[std::string_view("sourceFingerprint")] = "ipc-test-authoring";
    obj[std::string_view("migration")] = Utils::MakeYyjsonObject();

    auto wrapped = WrapJson(std::move(result));
    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult IpcTaskAuthoringSessionImpl::Compile(
    ExportInterface::IDasJson*,
    ExportInterface::IDasJson** pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto result = Utils::MakeYyjsonObject();
    auto obj = *result.as_object();
    obj[std::string_view("ok")] = true;
    obj[std::string_view("previewOnly")] = true;

    auto wrapped = WrapJson(std::move(result));
    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult IpcTaskAuthoringSessionFactoryImpl::CreateSession(
    const DasGuid&,
    ExportInterface::IDasJson*,
    PluginInterface::IDasTaskAuthoringSession** pp_out_session)
{
    if (!pp_out_session)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto* session = new IpcTaskAuthoringSessionImpl();
    session->AddRef();
    *pp_out_session = session;
    return DAS_S_OK;
}

// === IpcTaskComponentImpl ===

DasResult IpcTaskComponentImpl::ApplySettingsChange(
    ExportInterface::IDasJson*,
    ExportInterface::IDasJson** pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto result = Utils::MakeYyjsonObject();
    auto accepted = Utils::MakeYyjsonObject();
    (*accepted.as_object())[std::string_view("ipcSetting")] = "accepted";
    (*result.as_object())[std::string_view("acceptedSettings")] =
        std::move(accepted);

    auto wrapped = WrapJson(std::move(result));
    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult IpcTaskComponentImpl::Do(
    PluginInterface::IDasStopToken*,
    ExportInterface::IDasJson*,
    ExportInterface::IDasJson*,
    ExportInterface::IDasJson*,
    ExportInterface::IDasJson** pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto result = Utils::MakeYyjsonObject();
    auto obj = *result.as_object();
    obj[std::string_view("status")] = "completed";
    auto outputs = Utils::MakeYyjsonObject();
    (*outputs.as_object())[std::string_view("ipcResult")] = "ok";
    obj[std::string_view("outputs")] = std::move(outputs);
    auto signals = Utils::MakeYyjsonArray();
    signals.as_array()->emplace_back("next");
    obj[std::string_view("signals")] = std::move(signals);

    auto wrapped = WrapJson(std::move(result));
    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult IpcTaskComponentFactoryImpl::CreateComponent(
    const DasGuid&                       component_guid,
    PluginInterface::IDasTaskComponent** pp_out_component)
{
    if (!pp_out_component)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_component = nullptr;

    if (!GuidEquals(component_guid, kIpcTaskComponentGuid))
    {
        return DAS_E_NOT_FOUND;
    }

    auto* component = new IpcTaskComponentImpl();
    component->AddRef();
    *pp_out_component = component;
    return DAS_S_OK;
}

// === IpcTestPlugin1 ===

DasResult IpcTestPlugin1::EnumFeature(
    const size_t                       index,
    PluginInterface::DasPluginFeature* p_out_feature)
{
    static std::array features{
        PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY,
        PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY,
        PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY,
        PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY};
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
    if (index > 3)
    {
        return DAS_E_OUT_OF_RANGE;
    }

    if (index == 0)
    {
        // 创建 DasTouchMockImpl 实例
        auto* touch_impl = new DasTouchMockImpl();
        touch_impl->AddRef();
        *pp_out_interface =
            static_cast<PluginInterface::IDasTouch*>(touch_impl);
    }
    else if (index == 1)
    {
        // index == 1: 创建 DasQueryComponentFactoryImpl
        auto* factory = new DasQueryComponentFactoryImpl();
        factory->AddRef();
        *pp_out_interface =
            static_cast<PluginInterface::IDasComponentFactory*>(factory);
    }
    else if (index == 2)
    {
        auto* factory = new IpcTaskAuthoringSessionFactoryImpl();
        factory->AddRef();
        *pp_out_interface = static_cast<
            PluginInterface::IDasTaskAuthoringSessionFactory*>(factory);
    }
    else
    {
        auto* factory = new IpcTaskComponentFactoryImpl();
        factory->AddRef();
        *pp_out_interface =
            static_cast<PluginInterface::IDasTaskComponentFactory*>(factory);
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
