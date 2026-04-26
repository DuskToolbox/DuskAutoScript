#ifndef DAS_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <shared_mutex>
#include <span>
#include <unordered_map>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct FeatureInfo;

/**
 * @brief 组件工厂管理器
 *
 * 负责发现已加载插件提供的 IDasComponentFactory 实例，
 * 并通过 lazy probe routing table 按 component IID 路由 CreateComponent 调用。
 */
class ComponentFactoryManager
{
public:
    ComponentFactoryManager() = default;

    /**
     * @brief 插件加载通知 — 聚合 COMPONENT_FACTORY 工厂指针
     * @param plugin_guid 插件 GUID
     * @param factory_features 该插机的 COMPONENT_FACTORY FeatureInfo 列表
     */
    DasResult OnPluginLoaded(
        const DasGuid&                plugin_guid,
        std::span<FeatureInfo* const> factory_features);

    /**
     * @brief 插件卸载通知 — 从 factories_ 和 routing_table_ 中清除对应工厂
     * @param plugin_guid 插件 GUID
     */
    DasResult OnPluginUnloading(const DasGuid& plugin_guid);

    /**
     * @brief 路由 CreateComponent 调用到正确的工厂
     * @param component_iid 请求的组件 IID
     * @param pp_out_component 输出组件指针
     */
    DasResult CreateComponent(
        const DasGuid&                        component_iid,
        Das::PluginInterface::IDasComponent** pp_out_component);

private:
    /// component IID -> factory (cached after first probe, O(1) lookup)
    std::unordered_map<
        DasGuid,
        DasPtr<Das::PluginInterface::IDasComponentFactory>>
        routing_table_;

    /// plugin GUID -> factory (for removal on unload)
    std::unordered_map<
        DasGuid,
        DasPtr<Das::PluginInterface::IDasComponentFactory>>
        factories_;

    mutable std::shared_mutex mutex_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_COMPONENTFACTORYMANAGER_H
