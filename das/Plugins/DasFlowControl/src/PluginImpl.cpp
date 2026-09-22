#define DAS_BUILD_SHARED

#include "PluginImpl.h"

#include "FlowControlErrorLensRegistration.h"
#include "FlowControlTaskComponents.h"

#include <das/DasApi.h>

#include <array>
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

    static constexpr std::array features{
        PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY,
        PluginInterface::DAS_PLUGIN_FEATURE_ERROR_LENS};
    if (index >= features.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *p_out_feature = features[index];
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

    try
    {
        if (index == 0)
        {
            auto* factory = new DasFlowControlTaskComponentFactory(
                GetTaskComponentCreator());
            factory->AddRef();
            *pp_out_interface =
                static_cast<PluginInterface::IDasTaskComponentFactory*>(
                    factory);
            return DAS_S_OK;
        }
        if (index == 1)
        {
            auto lens =
                Plugins::DasFlowControl::CreateRegisteredFlowControlErrorLens();
            if (!lens)
            {
                return DAS_E_OUT_OF_MEMORY;
            }
            return lens->QueryInterface(
                DasIidOf<PluginInterface::IDasErrorLens>(),
                reinterpret_cast<void**>(pp_out_interface));
        }
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }

    return DAS_E_OUT_OF_RANGE;
}

DAS_NS_END
