#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H

#include <das/Core/ForeignInterfaceHost/ComponentFactoryManager.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <filesystem>
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
    Das::PluginInterface::DasPluginFeature feature_type;  // Feature 类型枚举
    DasGuid                                iid;           // 对应的接口 IID
    DasPtr<IDasBase>                       interface_ptr; // 创建的接口指针
    Core::IPC::ObjectId object_id;   // 在 RemoteObjectRegistry 中的对象 ID
    uint16_t            session_id;  // 所属会话 ID
    std::string         plugin_name; // 所属插件名称
    DasGuid             plugin_guid; // 所属插件 GUID
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
class DAS_API PluginManager
{
public:
    /**
     * @brief 构造函数，接收 SettingsManager 引用
     * @param settings_manager 设置管理器引用
     */
    explicit PluginManager(
        Das::Core::SettingsManager::SettingsManager& settings_manager);

    ~PluginManager() = default;

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
     * @param runtime 语言运行时（可选）
     * @return DAS_S_OK 成功
     */
    DasResult Initialize(
        uint16_t                        session_id,
        DasPtr<IForeignLanguageRuntime> runtime = nullptr);

    /**
     * @brief 关闭插件管理器，卸载所有插件
     */
    DasResult Shutdown();

    /**
     * @brief 设置语言运行时
     */
    DasResult SetRuntime(DasPtr<IForeignLanguageRuntime> runtime);

    /**
     * @brief 设置远程对象注册表引用
     */
    void SetRegistry(Core::IPC::RemoteObjectRegistry& registry);

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

    Das::Core::SettingsManager::SettingsManager& settings_manager_;
    mutable std::mutex                           mutex_;
    uint16_t                                     session_id_ = 0;
    DasPtr<IForeignLanguageRuntime>              runtime_;
    Core::IPC::RemoteObjectRegistry*             registry_ = nullptr;
    std::unordered_map<DasGuid, LoadedPlugin>    loaded_plugins_;
    std::unordered_map<std::string, DasGuid>     path_to_guid_;
    std::unordered_map<
        Das::PluginInterface::DasPluginFeature,
        std::vector<FeatureInfo*>>
                            feature_type_index_;
    ComponentFactoryManager component_factory_mgr_;
};

#ifdef _MSC_VER
#pragma warning(default : 4251)
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H
