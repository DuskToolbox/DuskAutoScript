#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief Feature 名称到接口 IID 的映射信息
 */
struct FeatureInfo
{
    std::string         feature_name;  // Feature 名称（如 "CAPTURE_FACTORY"）
    DasGuid             iid;           // 对应的接口 IID
    DasPtr<IDasBase>    interface_ptr; // 创建的接口指针
    Core::IPC::ObjectId object_id;     // 在 RemoteObjectRegistry 中的对象 ID
    uint16_t            session_id;    // 所属会话 ID
    std::string         plugin_name;   // 所属插件名称
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
 * - 通过 Feature 名称查找对象
 */
class PluginManager
{
public:
    /**
     * @brief 获取单例实例
     */
    static PluginManager& GetInstance();

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
     * @brief 通过 Feature 名称获取对象接口
     * @param feature_name Feature 名称（如 "CAPTURE_FACTORY", "ERROR_LENS"）
     * @param iid 请求的接口 IID
     * @param pp_out_object 输出对象指针
     * @return DAS_S_OK 成功，DAS_E_NOT_FOUND 未找到
     */
    DasResult GetObjectByFeature(
        const std::string& feature_name,
        const DasGuid&     iid,
        void**             pp_out_object);

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
     * @brief 获取所有可用的 Feature 名称
     * @param out_features 输出 Feature 名称列表
     */
    void GetAllFeatures(std::vector<std::string>& out_features) const;

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

private:
    PluginManager() = default;
    ~PluginManager() = default;

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

    mutable std::mutex                            mutex_;
    uint16_t                                      session_id_ = 0;
    DasPtr<IForeignLanguageRuntime>               runtime_;
    std::unordered_map<std::string, LoadedPlugin> loaded_plugins_;
    std::unordered_map<std::string, FeatureInfo*>
        feature_map_; // feature_name -> FeatureInfo
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGER_H
