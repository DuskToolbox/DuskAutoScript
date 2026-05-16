#define DAS_BUILD_SHARED

#include "MaapiAgentComponentFactory.h"

#include "MaapiAgentComponent.h"

#include <das/DasString.hpp>

#include <new>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    DasResult MaapiAgentComponentFactory::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiAgentComponentFactory>();
        return DAS_S_OK;
    }

    DasResult MaapiAgentComponentFactory::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.AgentComponentFactory",
            pp_out_name);
    }

    DasResult MaapiAgentComponentFactory::IsSupported(
        const DasGuid& component_iid)
    {
        return component_iid == DasIidOf<MaapiAgentComponent>()
                   ? DAS_S_OK
                   : DAS_E_NOT_FOUND;
    }

    DasResult MaapiAgentComponentFactory::CreateInstance(
        const DasGuid&                   component_iid,
        PluginInterface::IDasComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        if (component_iid != DasIidOf<MaapiAgentComponent>())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto* component = new MaapiAgentComponent();
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
