#ifndef DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <das/DasRef.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <functional>
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

/// entryId→条目解析的最小字段集（D-06 后端注入通道的数据形状）。
/// 不搬 TaskScheduler 的 RepositoryEntryDto 整体——ForeignInterfaceHost
/// 不得反向 include TaskScheduler 头，算法侧只消费编译注入所需字段面。
struct RepositoryEntrySummary
{
    int64_t       entry_id = 0;
    std::string   task_type_guid;
    yyjson::value accepted_properties;
    std::string   availability_state;
    std::string   availability_reason;
};

/// 后端解析能力一：entryId→条目最小字段集（数据源在 TaskScheduler 的
/// 条目存储，初始化期经注入函数对象倒置依赖）。
using RepositoryEntryResolver =
    std::function<std::optional<RepositoryEntrySummary>(int64_t)>;

/// 后端解析能力二：任务类型 guid+context_json→authoring 会话（数据源在
/// TaskScheduler 的能力注册表 + 插件会话工厂，同上注入）。
using AuthoringSessionResolver = std::function<DasResult(
    const DasGuid&,
    Das::ExportInterface::IDasJson*,
    Das::PluginInterface::IDasTaskAuthoringSession**)>;

/// 为 creator 后端具名错误码构建已注册的 ErrorLens（en/zh-cn 人话条目，
/// 照 FlowControlErrorLensRegistration 先例；码表账本=本头文件消费侧）。
[[nodiscard]]
DasPtr<Das::PluginInterface::IDasBasicErrorLens>
CreateRegisteredTaskComponentBackendErrorLens();

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
    /// 插件包时经 IDasPluginPackage::SetTaskComponentCreator 注入给包。
    [[nodiscard]]
    Das::PluginInterface::IDasTaskComponentCreator* GetTaskComponentCreator()
        const;

    /// 注入主进程 IPC 上下文：host 本尊随即显式注册进 DOM（显式 pin，
    /// 作为停机链确定性注销的记账锚点）。未注入时注册/注销链全部跳过。
    void SetIpcContext(
        Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context);

    /// 初始化期注入后端两条解析能力（D-06，照 SetIpcContext 构造注入
    /// 形状：一次调用、成员持有）。数据源在 TaskScheduler（条目存储+
    /// 能力注册表），经函数对象倒置依赖。缺注入时对应门返回具名
    /// DAS_E_* 码（遇缺即错零兜底，D-03）。
    void SetBackendResolvers(
        RepositoryEntryResolver  entry_resolver,
        AuthoringSessionResolver session_resolver);

    /// entryId→条目最小字段集（后端能力一的消费门）。后端未注入返
    /// DAS_E_REPOSITORY_ENTRY_RESOLVER_MISSING；条目不存在返
    /// DAS_E_NOT_FOUND。
    DasResult ResolveRepositoryEntry(
        int64_t                 entry_id,
        RepositoryEntrySummary* pp_out_summary);

    /// 任务类型→authoring 会话（后端能力二的消费门，host 本尊
    /// CreateAuthoringSession 的委托终点）。后端未注入返
    /// DAS_E_AUTHORING_SESSION_RESOLVER_MISSING。
    DasResult CreateAuthoringSession(
        const DasGuid&                                   task_guid,
        Das::ExportInterface::IDasJson*                  p_context_json,
        Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session);

    /// 停机链：确定性注销 host 本尊的 DOM 条目（幂等；析构函数兜底再调，
    /// registered 标志守卫保证二次注销无害）
    void Shutdown();

    [[nodiscard]]
    virtual std::vector<TaskComponentDefinitionInfo> EnumerateDefinitions()
        const;

private:
    /// host 本尊经注入上下文显式注册进 DOM（幂等守卫：已注册或未注入时
    /// 空转；下转具体上下文失败的外来实现同样跳过）
    void RegisterHostObject();

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

    DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host_;
    std::unordered_map<DasGuid, FactoryEntry>              factories_;
    std::unordered_map<DasGuid, ComponentRoute>            routes_;

    // 注入的主进程 IPC 上下文（共享引用保证注销时 DOM 仍存活；
    // 注入前为空）
    DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_;

    // host 本尊显式注册进 DOM 后的 ObjectId 与幂等守卫：停机/析构链
    // 确定性注销的记账锚点（未注册或已注销时守卫为 false）
    DAS::Core::IPC::ObjectId host_object_id_{};
    bool                     host_registered_ = false;

    // 后端两条解析能力（初始化期注入；空=主进程装配缺口，消费门返具名错）
    RepositoryEntryResolver  entry_resolver_;
    AuthoringSessionResolver session_resolver_;

    mutable std::shared_mutex mutex_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_TASKCOMPONENTFACTORYMANAGER_H
