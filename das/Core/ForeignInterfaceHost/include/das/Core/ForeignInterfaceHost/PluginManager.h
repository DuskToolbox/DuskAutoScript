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

// Forward declarations for test friend access
class PluginManagerGuidTest_OnHostProcessExit_DirectCall_CleansUpIndex_Test;
class PluginManagerGuidTest_OnHeartbeatTimeout_DirectCall_CleansUpIndex_Test;
class PluginManagerGuidTest_HeartbeatTimeout_RealThread_CleansUpIndex_Test;
class PluginManagerGuidTest_Shutdown_DoesNotHoldMutexDuringStop_Test;

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
 * @brief 已加载插件的信息
 */
struct LoadedPlugin
{
    std::filesystem::path                           plugin_path;
    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    std::shared_ptr<PluginPackageDesc>              desc;
    std::vector<FeatureInfo>                        features;
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
     * 阶段 1 在 mutex_ 锁内收集 launcher 的 DasPtr 副本 + 清空所有索引
     * （这些操作不阻塞、不碰 callback_mutex_）；阶段 2 释放 mutex_ 后
     * 调用 ClearCallbacks + Stop（可阻塞）。锁序保持 callback_mutex_ ->
     * mutex_ 正向，消除旧版持 mutex_ 调 ClearCallbacks/Stop 的 AB-BA
     * 死锁向量。该方法幂等：host_launchers_ 已空时二次调用空转安全
     * （~PluginManager 会再次调用以 drain 在途回调）。
     */
    DasResult Shutdown();

    /**
     * @brief Inject a remote host factory for focused runtime-provider tests.
     */
    void SetRemotePluginHostFactoryForTest(
        std::function<std::unique_ptr<IRemotePluginHost>()> factory);

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

    /**
     * @brief Inject a feature directly into the type index for testing.
     * Only for use in unit tests that need to simulate loaded features
     * without a real plugin DLL.
     */
    void RegisterTestFeature(
        Das::PluginInterface::DasPluginFeature type,
        const DasGuid&                         plugin_guid,
        IDasBase*                              interface_ptr);

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
     * 清理 loaded_plugins_、path_to_guid_、host_launchers_ 三个索引。
     * 进程退出和心跳超时两条路径最终都调用此方法。
     * 调用方必须已持有 mutex_。
     *
     * INV-02: 本方法只 host_launchers_.erase()，不得调用 ClearCallbacks/Stop
     * （会重入 callback_mutex_ 或违反锁层级 callback_mutex_ -> mutex_）。
     * ClearCallbacks/Stop 只能在 Shutdown/UnloadPluginIpc 的锁外阶段调用。
     */
    void CleanupPluginByGuid(DasGuid plugin_guid);

    /**
     * @brief Host 进程退出回调
     * @param plugin_guid 关联的插件 GUID
     * @param exit_code 进程退出码
     * @note 在 io_context 线程上执行（exit-watcher 协程 co_spawn 在
     *       io_context 上），内部获取 mutex_ 保护。
     *       锁序：callback_mutex_（HostLauncher exit-watcher slot）->
     *       mutex_（INV-03）。与 OnHeartbeatTimeout 路径锁序对称。
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
    // INV-04: HostLauncher 被本表（按 GUID）+ ConnectionManager::hosts_
    // （按 session_id）双重 DasPtr 持有。erase 本表一份后计数仍 >= 1，
    // HostLauncher 不析构，callback_mutex_ 不析构。这是 ClearCallbacks
    // drain 屏障成立的隐蔽依赖，明文化以保障后续维护不破坏该约束。
    std::unordered_map<DasGuid, DasPtr<DAS::Core::IPC::IHostLauncher>>
                host_launchers_;
    std::string host_exe_path_;

    // Allow specific unit tests to access private members for index cleanup
    // verification. Forward declarations are at top of file (global namespace).
    // 测试名带 DirectCall 后缀反映其"同线程直调"语义（WR-04，plan 03 task 2a
    // 重命名 OnHeartbeatTimeout_CleansUpIndex ->
    // OnHeartbeatTimeout_DirectCall_CleansUpIndex 及 OnHostProcessExit
    // 对称重命名）。
    friend class ::
        PluginManagerGuidTest_OnHostProcessExit_DirectCall_CleansUpIndex_Test;
    friend class ::
        PluginManagerGuidTest_OnHeartbeatTimeout_DirectCall_CleansUpIndex_Test;
    // plan 03 task 2b：新增真实跨线程测试（HeartbeatTimeout_RealThread +
    // Shutdown_DoesNotHoldMutexDuringStop），需访问 loaded_plugins_ /
    // path_to_guid_ / host_launchers_ 私有成员注入测试状态。
    friend class ::
        PluginManagerGuidTest_HeartbeatTimeout_RealThread_CleansUpIndex_Test;
    friend class ::
        PluginManagerGuidTest_Shutdown_DoesNotHoldMutexDuringStop_Test;
};

#ifdef _MSC_VER
#pragma warning(default : 4251)
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H
