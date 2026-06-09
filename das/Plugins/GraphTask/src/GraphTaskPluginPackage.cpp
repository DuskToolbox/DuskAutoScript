#include <das/Plugins/GraphTask/GraphTaskPluginPackage.h>

#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/Plugins/GraphTask/GraphTaskImpl.h>
#include <das/Utils/CommonUtils.hpp>

#include <new>

// {E8D1A2B3-4C5F-6D7E-8A9B-0C1D2E3F4A5B}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::GraphTask,
    GraphTaskPluginPackage,
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

namespace Das::Plugins::GraphTask
{

    uint32_t DAS_STD_CALL GraphTaskPluginPackage::AddRef()
    {
        return ++ref_count_;
    }

    uint32_t DAS_STD_CALL GraphTaskPluginPackage::Release()
    {
        const auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult DAS_STD_CALL
    GraphTaskPluginPackage::QueryInterface(const DasGuid& iid, void** pp_out)
    {
        if (pp_out == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out = static_cast<IDasBase*>(
                static_cast<Das::PluginInterface::IDasPluginPackage*>(this));
            AddRef();
            return DAS_S_OK;
        }
        if (iid == DasIidOf<Das::PluginInterface::IDasPluginPackage>())
        {
            *pp_out =
                static_cast<Das::PluginInterface::IDasPluginPackage*>(this);
            AddRef();
            return DAS_S_OK;
        }
        if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>())
        {
            *pp_out =
                static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(
                    this);
            AddRef();
            return DAS_S_OK;
        }

        *pp_out = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult GraphTaskPluginPackage::EnumFeature(
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

    DasResult GraphTaskPluginPackage::CreateFeatureInterface(
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

        AddRef();
        *pp_out_interface =
            static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(this);
        return DAS_S_OK;
    }

    DasResult GraphTaskPluginPackage::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<GraphTaskPluginPackage>();
        return DAS_S_OK;
    }

    DasResult GraphTaskPluginPackage::CanUnloadNow(bool* p_can_unload)
    {
        if (p_can_unload == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_can_unload = true;
        return DAS_S_OK;
    }

    DasResult GraphTaskPluginPackage::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.GraphTask.GraphTaskPluginPackage",
            pp_out_name);
    }

    DasResult GraphTaskPluginPackage::CreateComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;

        const DasGuid graph_task_guid = DasIidOf<GraphTaskImpl>();
        if (!(component_guid == graph_task_guid))
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto* component = new GraphTaskImpl(host_.Get());
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

    DasResult GraphTaskPluginPackage::SetTaskComponentHost(
        Das::PluginInterface::IDasTaskComponentHost* p_host)
    {
        host_ = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }

} // namespace Das::Plugins::GraphTask
