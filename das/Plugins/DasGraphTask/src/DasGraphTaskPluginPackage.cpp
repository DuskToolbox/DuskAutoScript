#include <das/Plugins/DasGraphTask/DasGraphTaskPluginPackage.h>

#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/Utils/CommonUtils.hpp>

#include <new>

// Factory GUID: {E8D1A2B3-4C5F-6D7E-8A9B-0C1D2E3F4A5B}
// Must match DasGraphTask.json taskComponents.factories for manifest lookup.
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskComponentFactory,
    0xE8D1A2B3,
    0x4C5F,
    0x6D7E,
    0x8A,
    0x9B,
    0x0C,
    0x1D,
    0x2E,
    0x3F,
    0x4A,
    0x5B)

// Package GUID: {A1B2C3D4-E5F6-4A7B-8C9D-0E1F2A3B4C5D}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskPluginPackage,
    0xA1B2C3D4,
    0xE5F6,
    0x4A7B,
    0x8C,
    0x9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D)

namespace Das::Plugins::DasGraphTask
{

    // === DasGraphTaskComponentFactory ===

    DasResult DasGraphTaskComponentFactory::CreateComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;

        const DasGuid graph_task_guid = DasIidOf<DasGraphTaskImpl>();
        if (!(component_guid == graph_task_guid))
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto* component = new DasGraphTaskImpl(host_.Get());
            component->AddRef();
            *pp_out_component =
                static_cast<Das::PluginInterface::IDasTaskComponent*>(
                    component);
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    DasResult DasGraphTaskComponentFactory::SetTaskComponentHost(
        Das::PluginInterface::IDasTaskComponentHost* p_host)
    {
        host_ = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }

    // === DasGraphTaskPluginPackage ===

    DasResult DasGraphTaskPluginPackage::EnumFeature(
        size_t                                  index,
        Das::PluginInterface::DasPluginFeature* p_out_feature)
    {
        if (p_out_feature == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (index != 0)
        {
            return DAS_E_OUT_OF_RANGE;
        }

        *p_out_feature =
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
        return DAS_S_OK;
    }

    DasResult DasGraphTaskPluginPackage::CreateFeatureInterface(
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
            auto* factory = new DasGraphTaskComponentFactory{};
            factory->AddRef();
            *pp_out_interface =
                static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(
                    factory);
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }

} // namespace Das::Plugins::DasGraphTask
