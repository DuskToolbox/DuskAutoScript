#define DAS_BUILD_SHARED

#include "PluginImpl.h"

#include "MaapiAgentComponentFactory.h"
#include "MaapiAgentTaskComponentFactory.h"
#include "MaapiAuthoringSessionFactory.h"
#include "MaapiTask.h"

#include <array>
#include <new>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
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
            PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY,
            PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY,
            PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY};
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
            if (index == 2)
            {
                auto* factory = new MaapiAgentComponentFactory();
                factory->AddRef();
                *pp_out_interface =
                    static_cast<PluginInterface::IDasComponentFactory*>(
                        factory);
                return DAS_S_OK;
            }
            if (index == 3)
            {
                auto* factory = new MaapiAgentTaskComponentFactory();
                factory->AddRef();
                *pp_out_interface =
                    static_cast<PluginInterface::IDasTaskComponentFactory*>(
                        factory);
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
