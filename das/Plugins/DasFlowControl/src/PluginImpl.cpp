#define DAS_BUILD_SHARED

#include "PluginImpl.h"

#include "FlowControlTaskComponents.h"

#include <das/DasApi.h>

#include <new>

DAS_NS_BEGIN

DasResult DasFlowControlPlugin::EnumFeature(
    size_t                             index,
    PluginInterface::DasPluginFeature* p_out_feature)
{
    if (p_out_feature == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (index != 0)
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *p_out_feature = PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
    return DAS_S_OK;
}

DasResult DasFlowControlPlugin::CreateFeatureInterface(
    size_t     index,
    IDasBase** pp_out_interface)
{
    if (pp_out_interface == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_interface = nullptr;
    if (index != 0)
    {
        return DAS_E_OUT_OF_RANGE;
    }

    try
    {
        auto* factory = new DasFlowControlTaskComponentFactory();
        factory->AddRef();
        *pp_out_interface =
            static_cast<PluginInterface::IDasTaskComponentFactory*>(factory);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_NS_END
