#include <das/Plugins/DasGraphTask/DasGraphTaskPluginPackage.h>

#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/Utils/CommonUtils.hpp>

#include <new>

// {E8D1A2B3-4C5F-6D7E-8A9B-0C1D2E3F4A5B}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskPluginPackage,
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

namespace Das::Plugins::DasGraphTask
{

    uint32_t DAS_STD_CALL DasGraphTaskPluginPackage::AddRef()
    {
        return ++ref_count_;
    }

    uint32_t DAS_STD_CALL DasGraphTaskPluginPackage::Release()
    {
        const auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult DAS_STD_CALL
    DasGraphTaskPluginPackage::QueryInterface(const DasGuid& iid, void** pp_out)
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

        AddRef();
        *pp_out_interface =
            static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(this);
        return DAS_S_OK;
    }

    DasResult DasGraphTaskPluginPackage::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<DasGraphTaskPluginPackage>();
        return DAS_S_OK;
    }

    DasResult DasGraphTaskPluginPackage::CanUnloadNow(bool* p_can_unload)
    {
        if (p_can_unload == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_can_unload = true;
        return DAS_S_OK;
    }

    DasResult DasGraphTaskPluginPackage::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.DasGraphTask.DasGraphTaskPluginPackage",
            pp_out_name);
    }

    DasResult DasGraphTaskPluginPackage::CreateComponent(
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

    DasResult DasGraphTaskPluginPackage::SetTaskComponentHost(
        Das::PluginInterface::IDasTaskComponentHost* p_host)
    {
        host_ = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }

} // namespace Das::Plugins::DasGraphTask
