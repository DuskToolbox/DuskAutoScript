#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H

#include <das/Core/ForeignInterfaceHost/ComponentFactoryManager.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/RuntimeProvider.h>
#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasPtr.hpp>
#include <das/DasSharedRef.hpp>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN
// Disable C4251 warning for DLL export with STL types
#ifdef _MSC_VER
#pragma warning(disable : 4251)
#endif

/**
 * @brief Feature 类型到接口 IID 的映射信息
 */
struct FeatureInfo
{
    uint64_t                               feature_index = 0;
    Das::PluginInterface::DasPluginFeature feature_type;  // Feature 类型枚举
    DasGuid                                iid;           // 对应的接口 IID
    DasPtr<IDasBase>                       interface_ptr; // 创建的接口指针
    Core::IPC::ObjectId object_id{};    // 在 RemoteObjectRegistry 中的对象 ID
    uint16_t            session_id = 0; // 所属会话 ID
    std::string         plugin_name;    // 所属插件名称
    DasGuid             plugin_guid;    // 所属插件 GUID
};

/**
 * @brief 外部注册的 runtime provider 条目（进程内注入扩展点）。
 *
 * LoadPlugin 选 provider 时按 language/load_mode 匹配注册表，命中优先于
 * 内置 CreateRuntimeProvider。供测试注入 fake runtime（包成 IRuntimeProvider）
 * 或未来第三方扩展使用。
 */
struct RegisteredRuntimeProvider
{
    ForeignInterfaceLanguage          language{};
    LoadMode                          load_mode{LoadMode::InProcess};
    std::shared_ptr<IRuntimeProvider> provider;
};

/**
 * @brief 已加载插件的信息
 */
struct LoadedPlugin
{
    std::filesystem::path                           plugin_path;
    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    std::shared_ptr<PluginPackageDesc>              desc;
    std::vector<FeatureInfo>                        features;
    /// IPC 加载的 owner_session_id（运行时卸载/索引用），0 表示非 IPC 加载。
    /// 区别于 FeatureInfo.session_id（feature 维度的 session 归属），
    /// LoadedPlugin.session_id 是 plugin 维度的 IPC owner_session_id。
    uint16_t session_id = 0;
};

/**
 * @brief 插件管理器
 *
 * 负责：
 * - 加载/卸载插件
 * - 枚举插件的 Features
 * - 将插件对象注册到 RemoteObjectRegistry
 * - 通过 GUID 主索引和 Feature-type 索引查找对象
 */
class PluginManager
{
public:
    /**
     * @brief 构造函数
     * @param settings_manager 设置管理器引用
     * @param ipc_context IPC 上下文（共享所有权）
     */
    explicit PluginManager(
        Das::Core::SettingsManager::SettingsManager& settings_manager,
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>
            ipc_context);

    ~PluginManager();

    /**
     * @brief 获取关联的 SettingsManager 引用
     */
    Das::Core::SettingsManager::SettingsManager& GetSettingsManager()
    {
        return settings_manager_;
    }

    /**
     * @brief 初始化插件管理器
     * @param session_id 当前会话 ID
     * @return DAS_S_OK 成功
     */
    DasResult Initialize(uint16_t session_id);

    /**
     * @brief 关闭插件管理器，卸载所有插件
     *
     * 方案 A 两阶段实现（CR-03 / INV-03 锁层级修复）：
     * 阶段 1 在 mutex_ 锁内清空所有索引（这些操作不阻塞、不碰
     * callback_mutex_）；阶段 2 释放 mutex_ 后调用
     * ipc_context_->ResetHostLifecycleCallbacks()（经 IIpcContext 虚方法
     * dispatch 到 IpcContext::ResetHostLifecycleCallbacks，遍历真实
     * launchers_ 清空 on_process_exit_slot_ / on_heartbeat_timeout_slot_）。
     * 纯 RAII 析构 drain（非两段式，语义像 ~ofstream 的 flush）。该方法
     * 幂等：二次调用空转安全（~PluginManager 会再次调用以 drain 在途回调）。
     */
    DasResult Shutdown();

    /**
     * @brief 设置远程对象注册表引用
     */
    void SetRegistry(Core::IPC::RemoteObjectRegistry& registry);

    /**
     * @brief 设置 Host 可执行文件路径
     * @param path DasHost 可执行文件路径
     */
    void SetHostExePath(const std::string& path);

    /**
     * @brief 注册外部 runtime provider（进程内注入扩展点）。
     *
     * LoadPlugin 选 provider 时先查注册表（按 language/load_mode 匹配），
     * 命中优先使用注册的 provider，否则走内置 CreateRuntimeProvider。
     * 供测试注入 fake runtime（包成 IRuntimeProvider）或未来第三方扩展。
     */
    void RegisterRuntimeProvider(
        ForeignInterfaceLanguage          language,
        LoadMode                          load_mode,
        std::shared_ptr<IRuntimeProvider> provider);

    /**
     * @brief 加载插件
     * @param path 插件路径
     * @param pp_out_package 输出的插件包接口（可选）
     * @return DAS_S_OK 成功
     */
    DasResult LoadPlugin(
        const std::filesystem::path&              path,
        Das::PluginInterface::IDasPluginPackage** pp_out_package = nullptr);

    /**
     * @brief 卸载插件
     * @param path 插件路径
     * @return DAS_S_OK 成功
     */
    DasResult UnloadPlugin(const std::filesystem::path& path);

    /**
     * @brief 获取已加载的插件
     * @param path 插件路径
     * @param pp_out_package 输出的插件包接口
     * @return DAS_S_OK 成功，DAS_E_NOT_FOUND 插件未加载
     */
    DasResult GetPlugin(
        const std::filesystem::path&              path,
        Das::PluginInterface::IDasPluginPackage** pp_out_package);

    /**
     * @brief 获取所有已加载的插件路径
     */
    std::vector<std::filesystem::path> GetLoadedPluginPaths() const;

    /**
     * @brief 注册插件的所有对象到 RemoteObjectRegistry
     * @param path 插件路径
     * @return DAS_S_OK 成功
     */
    DasResult RegisterPluginObjects(const std::filesystem::path& path);

    /**
     * @brief 注销插件的所有对象
     * @param path 插件路径
     * @return DAS_S_OK 成功
     */
    DasResult UnregisterPluginObjects(const std::filesystem::path& path);

    /**
     * @brief 通过 Feature 类型获取对象接口
     * @param feature Feature 类型
     * @param iid 请求的接口 IID
     * @param pp_out_object 输出对象指针
     * @return DAS_S_OK 成功，DAS_E_NOT_FOUND 未找到
     */
    DasResult GetObjectByFeature(
        Das::PluginInterface::DasPluginFeature feature,
        const DasGuid&                         iid,
        void**                                 pp_out_object);

    /**
     * @brief 按类型获取已注册的 Feature 列表
     * @param type Feature 类型
     * @return 指向 FeatureInfo 指针数组的 span
     */
    std::span<FeatureInfo* const> GetFeaturesByType(
        Das::PluginInterface::DasPluginFeature type) const;

    /**
     * @brief Create a fresh feature interface for a loaded plugin

     * * feature.
     * @param plugin_guid Plugin GUID that owns the feature

     * * @param feature_index Index passed to
     *
     * IDasPluginPackage::CreateFeatureInterface
     * @param iid Requested
     * interface IID
     * @param pp_out_object Output interface pointer
 *

     * * @return DAS_S_OK on success, DAS_E_NOT_FOUND when plugin missing
 */
    DasResult CreateFeatureInterface(
        const DasGuid& plugin_guid,
        uint64_t       feature_index,
        const DasGuid& iid,
        void**         pp_out_object);

    /**
     * @brief 获取指定插件的所有 Feature
     * @param path 插件路径
     * @param out_features 输出 Feature 信息列表
     * @return DAS_S_OK 成功，DAS_E_NOT_FOUND 插件未加载
     */
    DasResult GetPluginFeatures(
        const std::filesystem::path& path,
        std::vector<FeatureInfo>&    out_features) const;

    /**
     * @brief 检查插件是否已加载
     */
    bool IsPluginLoaded(const std::filesystem::path& path) const;

    /**
     * @brief 获取已加载插件数量
     */
    size_t GetLoadedPluginCount() const;

    /**
     * @brief 获取指定 GUID 插件的 PluginPackageDesc
     * @param guid 插件 GUID
     * @return PluginPackageDesc* 插件描述指针，未找到返回 nullptr
     */
    PluginPackageDesc* FindPluginPackageByGuid(const DasGuid& guid);

    ComponentFactoryManager& GetComponentFactoryManager();

    TaskComponentFactoryManager& GetTaskComponentFactoryManager();

    ErrorLensManager& GetErrorLensManager();

private:
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /**
     * @brief 获取 Feature 对应的接口 IID
     */
    DasGuid GetIidForFeature(
        Das::PluginInterface::DasPluginFeature feature) const;

    /**
     * @brief 获取 Feature 的显示名称
     */
    std::string GetFeatureName(
        Das::PluginInterface::DasPluginFeature feature) const;

    /**
     * @brief 规范化插件路径（用于查找）
     */
    std::filesystem::path NormalizePath(
        const std::filesystem::path& path) const;

    /**
     * @brief 通过 GUID 查找已加载插件
     */
    LoadedPlugin* FindPluginByGuid(const DasGuid& guid);

    /**
     * @brief 通过 IPC 卸载插件（关闭 Host 进程）
     * @note 必须在 mutex_ 锁外调用（Stop 可能阻塞等待进程退出）
     */
    DasResult UnloadPluginIpc(const DasGuid& guid, LoadedPlugin& plugin);

    /**
     * @brief 统一按 GUID 清理插件索引
     * @param plugin_guid 插件 GUID
     *
     * 清理 loaded_plugins_、path_to_guid_ 两个索引。
     * 进程退出和心跳超时两条路径最终都调用此方法。
     * 调用方必须已持有 mutex_。
     *
     * INV-02: 本方法只 erase 索引，不得调用 ClearCallbacks/Stop
     * （会重入 callback_mutex_ 或违反锁层级 callback_mutex_ -> mutex_）。
     * ClearCallbacks/Stop 只能在 Shutdown/UnloadPluginIpc 的锁外阶段调用。
     * HostLauncher 清理由 IpcContext/ConnectionManager 按 session 自行管理，
     * 本方法不再持有 launcher 索引。
     */
    void CleanupPluginByGuid(DasGuid plugin_guid);

protected:
    /**
     * @brief Host 进程退出回调
     * @param plugin_guid 关联的插件 GUID
     * @param exit_code 进程退出码
     * @note 在 io_context 线程上执行（exit-watcher 协程 co_spawn 在
     *       io_context 上），内部获取 mutex_ 保护。
     *       锁序：callback_mutex_（HostLauncher exit-watcher slot）->
     *       mutex_（INV-03）。与 OnHeartbeatTimeout 路径锁序对称。
     *
     * 这两个回调方法以 protected 暴露，允许测试子类直接触发以验证索引
     * 清理逻辑；生产子类（如果将来有）也可利用这两个 hook。
     */
    void OnHostProcessExit(DasGuid plugin_guid, int exit_code);

    /**
     * @brief IPC 心跳超时回调
     * @param plugin_guid 关联的插件 GUID
     * @note 在 ConnectionManager::heartbeat_thread_ 上执行（非 io_context
     *       线程），内部获取 mutex_ 保护。
     *       锁序：callback_mutex_（HostLauncher）-> mutex_（INV-03）。
     *       heartbeat_thread_ 由 ConnectionManager 持有并 join（非
     * io_context）。
     */
    void OnHeartbeatTimeout(DasGuid plugin_guid);

    // ----- 子类扩展点：所有内部状态字段 -----
    // 全部字段以 protected 暴露，允许子类（生产或测试）按需读写状态、
    // 加锁调用回调方法、注入 factory 等。当前唯一子类是 TestablePluginManager
    // (测试专用)，生产子类若将来加入可直接复用这些扩展点。
    Das::Core::SettingsManager::SettingsManager& settings_manager_;
    mutable std::mutex                           mutex_;
    uint16_t                                     session_id_ = 0;
    std::function<std::unique_ptr<IRemotePluginHost>()>
                                              remote_plugin_host_factory_;
    Core::IPC::RemoteObjectRegistry*          registry_ = nullptr;
    std::unordered_map<DasGuid, LoadedPlugin> loaded_plugins_;
    std::unordered_map<std::string, DasGuid>  path_to_guid_;
    std::unordered_map<
        Das::PluginInterface::DasPluginFeature,
        std::vector<FeatureInfo*>>
                                feature_type_index_;
    ComponentFactoryManager     component_factory_mgr_;
    TaskComponentFactoryManager task_component_factory_mgr_;
    ErrorLensManager            error_lens_mgr_;

    // IPC 相关成员
    Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_;
    std::string                                                 host_exe_path_;

    // 外部注册的 runtime provider（进程内注入扩展点）。LoadPlugin 选 provider
    // 时先查此表（language/load_mode 匹配），命中优先于内置
    // CreateRuntimeProvider。
    std::vector<RegisteredRuntimeProvider> registered_providers_;

private:
    /**
     * @brief 按 language/load_mode 查找已注册的第三方 runtime provider。
     * @return 匹配的 provider（共享所有权），未匹配返回 nullptr。
     */
    std::shared_ptr<IRuntimeProvider> FindRegisteredRuntimeProvider(
        ForeignInterfaceLanguage language,
        LoadMode                 load_mode) const;
};

#ifdef _MSC_VER
#pragma warning(default : 4251)
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H
