#ifndef DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct FeatureInfo;

struct TaskComponentDefinitionInfo
{
    DasGuid       plugin_guid{};
    DasGuid       factory_guid{};
    DasGuid       component_guid{};
    yyjson::value definition{};
};

/**
 * @brief Manifest-routed task component factory registry.
 *
 * The manager owns routes discovered from real plugin package features and
 * parsed taskComponents manifest declarations. Component instances remain
 * lazy-created through CreateComponent(componentGuid).
 */
class TaskComponentFactoryManager
{
public:
    TaskComponentFactoryManager();
    virtual ~TaskComponentFactoryManager();

    DasResult OnPluginLoaded(
        const DasGuid&                                   plugin_guid,
        std::span<FeatureInfo* const>                    factory_features,
        const std::optional<TaskComponentsManifestDesc>& task_components);

    DasResult OnPluginUnloading(const DasGuid& plugin_guid);

    DasResult CreateComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component);

    [[nodiscard]]
    virtual std::vector<TaskComponentDefinitionInfo> EnumerateDefinitions()
        const;

private:
    struct FactoryEntry
    {
        DasGuid                                                plugin_guid{};
        DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    };

    struct ComponentRoute
    {
        DasGuid       plugin_guid{};
        DasGuid       factory_guid{};
        yyjson::value definition{};
    };

    DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;
    std::unordered_map<DasGuid, FactoryEntry>           factories_;
    std::unordered_map<DasGuid, ComponentRoute>         routes_;

    mutable std::shared_mutex mutex_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H
