#define DAS_BUILD_SHARED

#include "PluginImpl.h"
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/DasString.hpp>

#include <array>

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
        PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY};
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
    size_t index,
    IDasBase** pp_out_interface)
{
    if (!pp_out_interface)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (index > 0)
    {
        return DAS_E_OUT_OF_RANGE;
    }

    // 创建 DasTouchMockImpl 实例
    auto* touch_impl = new DasTouchMockImpl(session_id_);
    touch_impl->AddRef();
    *pp_out_interface = static_cast<PluginInterface::IDasTouch*>(touch_impl);

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
