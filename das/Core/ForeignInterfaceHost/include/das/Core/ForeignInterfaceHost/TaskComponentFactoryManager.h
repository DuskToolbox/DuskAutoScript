#ifndef DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasPtr.hpp>
#include <das/DasSharedRef.hpp>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <memory>
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

    /// 本尊 host 的借用访问（host_ 语义不变）：供 PluginManager 在加载
    /// 插件包时经 IDasPluginPackage::SetTaskComponentHost 注入给包。
    [[nodiscard]]
    Das::PluginInterface::IDasTaskComponentHost* GetTaskComponentHost() const;

    /// 注入主进程 IPC 上下文：host 本尊随即显式注册进 DOM（显式 pin，
    /// 作为停机链确定性注销的记账锚点）。未注入时注册/注销链全部跳过。
    void SetIpcContext(
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>
            ipc_context);

    /// 停机链：确定性注销 host 本尊的 DOM 条目（幂等；析构函数兜底再调，
    /// registered 标志守卫保证二次注销无害）
    void Shutdown();

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

    // 注入的主进程 IPC 上下文（共享引用保证注销时 DOM 仍存活）
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_;

    mutable std::shared_mutex mutex_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H
