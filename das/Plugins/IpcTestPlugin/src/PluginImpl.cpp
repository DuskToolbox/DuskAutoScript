#include "das/DasTypes.hpp"
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>

#define DAS_BUILD_SHARED

#include "PluginImpl.h"

#include <array>
#include <stdexcept>

DAS_NS_BEGIN

DasResult IpcTestPlugin::EnumFeature(
    const size_t                       index,
    PluginInterface::DasPluginFeature* p_out_feature)
{
    static std::array features{
        PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY};
    try
    {
        const auto result = features.at(index);
        *p_out_feature = result;
        return result;
    }
    catch (const std::out_of_range& ex)
    {
        DAS_LOG_ERROR(ex.what());
        return DAS_E_OUT_OF_RANGE;
    }
}

DasResult IpcTestPlugin::CreateFeatureInterface(
    size_t index,
    void** pp_out_interface)
{
    std::ignore = index;
    pp_out_interface = nullptr;
    return DAS_E_NO_IMPLEMENTATION;
}

static std::atomic_int32_t g_ref_count;

DasResult IpcTestPlugin::CanUnloadNow(bool* p_can_unload)
{
    *p_can_unload = g_ref_count == 0;
    return DAS_S_OK;
}

void IpcTestPluginAddRef() { g_ref_count++; }

void IpcTestPluginRelease() { g_ref_count--; }

DAS_NS_END
