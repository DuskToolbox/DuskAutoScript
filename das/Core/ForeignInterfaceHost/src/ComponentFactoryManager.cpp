#include <das/Core/ForeignInterfaceHost/ComponentFactoryManager.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasComponent.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DasResult ComponentFactoryManager::OnPluginLoaded(
    const DasGuid&                plugin_guid,
    std::span<FeatureInfo* const> factory_features)
{
    if (factory_features.empty())
    {
        return DAS_S_OK;
    }

    std::unique_lock lock{mutex_};

    for (auto* feat : factory_features)
    {
        if (!feat->interface_ptr)
        {
            continue;
        }

        DasPtr<Das::PluginInterface::IDasComponentFactory> factory;
        auto qi_result = feat->interface_ptr->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasComponentFactory>(),
            reinterpret_cast<void**>(factory.Put()));
        if (DAS::IsFailed(qi_result) || !factory)
        {
            DAS_CORE_LOG_WARN(
                "Failed to QI IDasComponentFactory from plugin feature");
            continue;
        }

        factories_[plugin_guid] = std::move(factory);
        DAS_CORE_LOG_INFO("Registered component factory for plugin");
        break;
    }

    return DAS_S_OK;
}

DasResult ComponentFactoryManager::OnPluginUnloading(const DasGuid& plugin_guid)
{
    std::unique_lock lock{mutex_};

    auto factory_it = factories_.find(plugin_guid);
    if (factory_it == factories_.end())
    {
        return DAS_S_OK;
    }

    auto& removing_factory = factory_it->second;

    for (auto it = routing_table_.begin(); it != routing_table_.end();)
    {
        if (it->second.Get() == removing_factory.Get())
        {
            it = routing_table_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    factories_.erase(factory_it);
    DAS_CORE_LOG_INFO("Removed component factory for plugin");
    return DAS_S_OK;
}

DasResult ComponentFactoryManager::CreateComponent(
    const DasGuid&                        component_iid,
    Das::PluginInterface::IDasComponent** pp_out_component)
{
    DAS_UTILS_CHECK_POINTER(pp_out_component)

    DasOutPtr<Das::PluginInterface::IDasComponent> result(pp_out_component);

    {
        std::shared_lock lock{mutex_};
        auto             it = routing_table_.find(component_iid);
        if (it != routing_table_.end())
        {
            auto cr = it->second->CreateInstance(component_iid, result.Put());
            if (DAS::IsOk(cr))
            {
                result.Keep();
            }
            return cr;
        }
    }

    {
        std::unique_lock lock{mutex_};
        auto             it = routing_table_.find(component_iid);
        if (it != routing_table_.end())
        {
            auto cr = it->second->CreateInstance(component_iid, result.Put());
            if (DAS::IsOk(cr))
            {
                result.Keep();
            }
            return cr;
        }

        for (auto& [guid, factory] : factories_)
        {
            if (factory->IsSupported(component_iid) == DAS_S_OK)
            {
                routing_table_[component_iid] = factory;
                auto cr = factory->CreateInstance(component_iid, result.Put());
                if (DAS::IsOk(cr))
                {
                    result.Keep();
                }
                return cr;
            }
        }
    }

    DAS_CORE_LOG_WARN("No factory supports component_iid");
    return DAS_E_NOT_FOUND;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
