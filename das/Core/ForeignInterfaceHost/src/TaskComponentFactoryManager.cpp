#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <mutex>
#include <unordered_set>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    std::optional<DasGuid> TryMakeGuid(std::string_view value)
    {
        try
        {
            return MakeDasGuid(value);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    yyjson::value CopyJsonValue(const yyjson::value& value)
    {
        yyjson::value result;
        result = value;
        return result;
    }
} // namespace

DasResult TaskComponentFactoryManager::OnPluginLoaded(
    const DasGuid&                                   plugin_guid,
    std::span<FeatureInfo* const>                    factory_features,
    const std::optional<TaskComponentsManifestDesc>& task_components)
{
    if (!task_components.has_value())
    {
        return DAS_S_OK;
    }

    std::unordered_map<DasGuid, FactoryEntry> plugin_factories;
    for (auto* feature : factory_features)
    {
        if (feature == nullptr || !feature->interface_ptr)
        {
            DAS_CORE_LOG_WARN(
                "Invalid task component factory feature: missing interface "
                "pointer");
            continue;
        }

        DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
        const auto qi_result = feature->interface_ptr->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>(),
            reinterpret_cast<void**>(factory.Put()));
        if (DAS::IsFailed(qi_result) || !factory)
        {
            DAS_CORE_LOG_WARN(
                "Invalid task component factory feature: "
                "QueryInterface(IDasTaskComponentFactory) failed, result={}",
                static_cast<int>(qi_result));
            continue;
        }

        DasGuid    factory_guid{};
        const auto get_guid_result = factory->GetGuid(&factory_guid);
        if (DAS::IsFailed(get_guid_result))
        {
            DAS_CORE_LOG_WARN(
                "Invalid task component factory feature: GetGuid failed, "
                "result={}",
                static_cast<int>(get_guid_result));
            continue;
        }

        if (plugin_factories.contains(factory_guid))
        {
            DAS_CORE_LOG_WARN("Invalid task component factory feature: duplicate factoryGuid={}", factory_guid);
            return DAS_E_DUPLICATE_ELEMENT;
        }

        plugin_factories.emplace(
            factory_guid,
            FactoryEntry{plugin_guid, std::move(factory)});
    }

    std::vector<std::pair<DasGuid, ComponentRoute>> staged_routes;
    if (!task_components->factories)
    {
        DAS_CORE_LOG_WARN("Invalid taskComponents manifest: missing factories array");
        return DAS_E_INVALID_ARGUMENT;
    }
    if (!task_components->components)
    {
        DAS_CORE_LOG_WARN("Invalid taskComponents manifest: missing components object");
        return DAS_E_INVALID_ARGUMENT;
    }
    staged_routes.reserve(task_components->components->size());

    std::unordered_set<DasGuid> declared_factories;
    for (const auto& declared_factory_guid_text : *task_components->factories)
    {
        const auto declared_factory_guid =
            TryMakeGuid(declared_factory_guid_text);
        if (!declared_factory_guid)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: invalid factoryGuid={}", declared_factory_guid_text);
            return DAS_E_INVALID_ARGUMENT;
        }
        if (!plugin_factories.contains(*declared_factory_guid))
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: factoryGuid={} has no loaded task component factory", *declared_factory_guid);
            return DAS_E_NOT_FOUND;
        }
        declared_factories.insert(*declared_factory_guid);
    }

    for (const auto& [component_guid_text, entry] :
         *task_components->components)
    {
        const auto component_guid = TryMakeGuid(component_guid_text);
        if (!component_guid)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: invalid componentGuid={}", component_guid_text);
            return DAS_E_INVALID_ARGUMENT;
        }

        if (!entry.factory_guid)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: missing factoryGuid for componentGuid={}", *component_guid);
            return DAS_E_INVALID_ARGUMENT;
        }

        const auto factory_guid = TryMakeGuid(*entry.factory_guid);
        if (!factory_guid)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: invalid factoryGuid={} for componentGuid={}", *entry.factory_guid, *component_guid);
            return DAS_E_INVALID_ARGUMENT;
        }
        if (!declared_factories.contains(*factory_guid))
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: undeclared factoryGuid={} for componentGuid={}", *factory_guid, *component_guid);
            return DAS_E_INVALID_ARGUMENT;
        }
        if (!entry.definition)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: missing definition for componentGuid={}", *component_guid);
            return DAS_E_INVALID_ARGUMENT;
        }

        auto factory_it = plugin_factories.find(*factory_guid);
        if (factory_it == plugin_factories.end())
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: factoryGuid={} has no loaded task component factory for componentGuid={}", *factory_guid, *component_guid);
            return DAS_E_NOT_FOUND;
        }

        DasPtr<Das::PluginInterface::IDasTaskComponent> probe;
        const auto                                      create_result =
            factory_it->second.factory->CreateComponent(
                *component_guid,
                probe.Put());
        if (DAS::IsFailed(create_result) || !probe)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: CreateComponent failed for componentGuid={} factoryGuid={} result={}", *component_guid, *factory_guid, static_cast<int>(create_result));
            return DAS::IsFailed(create_result) ? create_result
                                                : DAS_E_INVALID_POINTER;
        }

        staged_routes.emplace_back(
            *component_guid,
            ComponentRoute{
                plugin_guid,
                *factory_guid,
                CopyJsonValue(*entry.definition)});
    }

    std::unique_lock lock{mutex_};
    for (const auto& [factory_guid, factory_entry] : plugin_factories)
    {
        auto existing_it = factories_.find(factory_guid);
        if (existing_it != factories_.end()
            && existing_it->second.plugin_guid != plugin_guid)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: duplicate factoryGuid={} already registered by another plugin", factory_guid);
            return DAS_E_DUPLICATE_ELEMENT;
        }
    }
    for (const auto& [component_guid, route] : staged_routes)
    {
        auto existing_it = routes_.find(component_guid);
        if (existing_it != routes_.end()
            && existing_it->second.plugin_guid != route.plugin_guid)
        {
            DAS_CORE_LOG_WARN("Invalid taskComponents manifest: duplicate componentGuid={} already registered by another plugin", component_guid);
            return DAS_E_DUPLICATE_ELEMENT;
        }
    }

    for (auto it = routes_.begin(); it != routes_.end();)
    {
        if (it->second.plugin_guid == plugin_guid)
        {
            it = routes_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = factories_.begin(); it != factories_.end();)
    {
        if (it->second.plugin_guid == plugin_guid)
        {
            it = factories_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto& [factory_guid, factory_entry] : plugin_factories)
    {
        factories_.emplace(factory_guid, std::move(factory_entry));
    }
    for (auto& [component_guid, route] : staged_routes)
    {
        routes_.emplace(component_guid, std::move(route));
    }

    DAS_CORE_LOG_INFO(
        "Registered task component factories for plugin: factories={}, "
        "components={}",
        plugin_factories.size(),
        staged_routes.size());
    return DAS_S_OK;
}

DasResult TaskComponentFactoryManager::OnPluginUnloading(
    const DasGuid& plugin_guid)
{
    std::unique_lock lock{mutex_};

    for (auto it = routes_.begin(); it != routes_.end();)
    {
        if (it->second.plugin_guid == plugin_guid)
        {
            it = routes_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = factories_.begin(); it != factories_.end();)
    {
        if (it->second.plugin_guid == plugin_guid)
        {
            it = factories_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    DAS_CORE_LOG_INFO("Removed task component factories for plugin");
    return DAS_S_OK;
}

DasResult TaskComponentFactoryManager::CreateComponent(
    const DasGuid&                            component_guid,
    Das::PluginInterface::IDasTaskComponent** pp_out_component)
{
    DAS_UTILS_CHECK_POINTER(pp_out_component)

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    DasGuid                                                factory_guid{};
    {
        std::shared_lock lock{mutex_};
        auto             route_it = routes_.find(component_guid);
        if (route_it == routes_.end())
        {
            DAS_CORE_LOG_WARN("No task component route found for componentGuid={}", component_guid);
            return DAS_E_NOT_FOUND;
        }

        factory_guid = route_it->second.factory_guid;
        auto factory_it = factories_.find(factory_guid);
        if (factory_it == factories_.end() || !factory_it->second.factory)
        {
            DAS_CORE_LOG_WARN("No task component factory found for factoryGuid={} componentGuid={}", factory_guid, component_guid);
            return DAS_E_NOT_FOUND;
        }
        factory = factory_it->second.factory;
    }

    DasOutPtr<Das::PluginInterface::IDasTaskComponent> result(pp_out_component);
    const auto                                         create_result =
        factory->CreateComponent(component_guid, result.Put());
    if (DAS::IsOk(create_result))
    {
        result.Keep();
    }
    else
    {
        DAS_CORE_LOG_WARN("Task component CreateComponent failed for componentGuid={} factoryGuid={} result={}", component_guid, factory_guid, static_cast<int>(create_result));
    }
    return create_result;
}

std::vector<TaskComponentDefinitionInfo>
TaskComponentFactoryManager::EnumerateDefinitions() const
{
    std::shared_lock lock{mutex_};

    std::vector<TaskComponentDefinitionInfo> definitions;
    definitions.reserve(routes_.size());
    for (const auto& [component_guid, route] : routes_)
    {
        definitions.push_back(
            TaskComponentDefinitionInfo{
                route.plugin_guid,
                route.factory_guid,
                component_guid,
                CopyJsonValue(route.definition)});
    }
    return definitions;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
