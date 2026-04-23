#include <das/Core/ForeignInterfaceHost/PluginManager.h>

#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/Logger/Logger.h>
#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

PluginManager::PluginManager(
    Das::Core::SettingsManager::SettingsManager& settings_manager,
    Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
    : settings_manager_(settings_manager), ipc_context_{std::move(ipc_context)}
{
}

DasResult PluginManager::Initialize(
    uint16_t                        session_id,
    DasPtr<IForeignLanguageRuntime> runtime)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (session_id_ != 0 && session_id_ != session_id)
    {
        DAS_CORE_LOG_WARN(
            "PluginManager already initialized with session_id={}",
            session_id_);
        return DAS_E_OBJECT_ALREADY_INIT;
    }

    session_id_ = session_id;
    if (runtime)
    {
        runtime_ = std::move(runtime);
    }

    DAS_CORE_LOG_INFO(
        "PluginManager initialized with session_id={}",
        session_id_);
    return DAS_S_OK;
}

DasResult PluginManager::Shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 停止所有 HostLauncher
    // 先清除进程退出回调，避免 Stop 触发回调
    for (auto& [guid, launcher] : host_launchers_)
    {
        if (launcher)
        {
            auto* concrete =
                static_cast<DAS::Core::IPC::HostLauncher*>(launcher.Get());
            if (concrete)
            {
                concrete->ClearCallbacks();
            }
            if (launcher->IsRunning())
            {
                DAS_CORE_LOG_INFO("Shutdown: stopping Host launcher");
                launcher->Stop();
            }
        }
    }
    host_launchers_.clear();

    // 卸载所有插件
    for (auto& [guid, plugin] : loaded_plugins_)
    {
        // 先注销对象
        if (registry_)
        {
            for (auto& feature : plugin.features)
            {
                registry_->UnregisterObject(feature.object_id);
            }
        }

        // 通知 ComponentFactoryManager 释放工厂引用
        component_factory_mgr_.OnPluginUnloading(guid);
    }

    feature_type_index_.clear();
    path_to_guid_.clear();
    loaded_plugins_.clear();
    runtime_.Reset();
    session_id_ = 0;

    DAS_CORE_LOG_INFO("PluginManager shutdown complete");
    return DAS_S_OK;
}

DasResult PluginManager::SetRuntime(DasPtr<IForeignLanguageRuntime> runtime)
{
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_ = std::move(runtime);
    return DAS_S_OK;
}

void PluginManager::SetRegistry(Core::IPC::RemoteObjectRegistry& registry)
{
    registry_ = &registry;
}

void PluginManager::SetHostExePath(const std::string& path)
{
    host_exe_path_ = path;
}

DasResult PluginManager::LoadPlugin(
    const std::filesystem::path&              path,
    Das::PluginInterface::IDasPluginPackage** pp_out_package)
{
    // 阶段1: 锁内快速检查 + manifest 解析
    std::filesystem::path              normalized_path;
    std::string                        path_str;
    std::shared_ptr<PluginPackageDesc> desc;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!runtime_)
        {
            DAS_CORE_LOG_ERROR("No runtime set for loading plugins");
            return DAS_E_OBJECT_NOT_INIT;
        }

        normalized_path = NormalizePath(path);
        path_str = normalized_path.string();

        // 路径去重检查
        if (path_to_guid_.contains(path_str))
        {
            DAS_CORE_LOG_WARN("Plugin already loaded: {}", path_str);
            if (pp_out_package)
            {
                auto it = loaded_plugins_.find(path_to_guid_[path_str]);
                if (it != loaded_plugins_.end() && it->second.package)
                {
                    *pp_out_package = it->second.package.Get();
                    (*pp_out_package)->AddRef();
                }
            }
            return DAS_S_FALSE;
        }

        // 解析 manifest JSON 获取 PluginPackageDesc（含 guid）
        desc = std::make_shared<PluginPackageDesc>();

        // Manifest 路径解析：支持两种插件模式
        // 1. 目录模式：path 是目录，manifest 在 <dir>/<dirname>.json 或
        // <dir>/manifest.json
        // 2. 扁平文件模式：path 本身就是 .json manifest 文件
        std::filesystem::path manifest_path;
        if (std::filesystem::is_directory(normalized_path))
        {
            // 目录模式：与 PluginScanner::FindManifest 逻辑一致
            auto dirname = normalized_path.filename().string();
            auto primary = normalized_path / (dirname + ".json");
            if (std::filesystem::exists(primary))
            {
                manifest_path = primary;
            }
            else
            {
                auto fallback = normalized_path / "manifest.json";
                if (std::filesystem::exists(fallback))
                {
                    manifest_path = fallback;
                }
            }
        }
        else if (normalized_path.extension() == ".json")
        {
            // 扁平文件模式：path 本身就是 manifest
            manifest_path = normalized_path;
        }

        if (!manifest_path.empty() && std::filesystem::exists(manifest_path))
        {
            std::ifstream ifs(manifest_path);
            if (ifs.is_open())
            {
                try
                {
                    auto json_data = nlohmann::json::parse(ifs);
                    from_json(json_data, *desc);
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_WARN(
                        "Failed to parse manifest for {}: {}",
                        path_str,
                        e.what());
                }
            }
        }
    }

    // 阶段2: 锁外执行加载（进程内或 IPC）
    static const std::unordered_set<ForeignInterfaceLanguage>
        kInProcessLanguages = {
            ForeignInterfaceLanguage::Cpp,
            ForeignInterfaceLanguage::Python,
        };

    if (kInProcessLanguages.contains(desc->language)
        && desc->load_mode != LoadMode::Ipc)
    {
        // 进程内路径（白名单内语言且未显式指定 IPC 模式）
        auto result = runtime_->LoadPlugin(normalized_path);
        if (!result)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to load plugin {}: error={}",
                path_str,
                static_cast<int>(result.error()));
            return result.error();
        }

        LoadedPlugin plugin;
        plugin.plugin_path = normalized_path;

        DasPtr<Das::PluginInterface::IDasPluginPackage> package;
        auto qi_result = (*result)->QueryInterface(
            DAS_IID_PLUGIN_PACKAGE,
            reinterpret_cast<void**>(package.Put()));
        if (DAS::IsFailed(qi_result))
        {
            DAS_CORE_LOG_ERROR(
                "Plugin does not implement IDasPluginPackage: {}",
                path_str);
            return DAS_E_NO_INTERFACE;
        }
        plugin.package = std::move(package);
        plugin.desc = desc;

        // 枚举插件的 Features 并创建接口
        uint64_t index = 0;
        while (true)
        {
            Das::PluginInterface::DasPluginFeature feature{};
            auto enum_result = plugin.package->EnumFeature(index, &feature);
            if (enum_result != DAS_S_OK)
            {
                break;
            }

            FeatureInfo feature_info;
            feature_info.feature_type = feature;
            feature_info.iid = GetIidForFeature(feature);
            feature_info.session_id = session_id_;
            feature_info.plugin_name = normalized_path.stem().string();
            feature_info.plugin_guid = desc->guid;

            DasPtr<IDasBase> p_interface = nullptr;
            auto create_result = plugin.package->CreateFeatureInterface(
                index,
                p_interface.Put());
            if (create_result == DAS_S_OK)
            {
                feature_info.interface_ptr = p_interface;
            }

            plugin.features.push_back(std::move(feature_info));
            ++index;
        }

        // 阶段3: 锁内更新索引
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // GUID 冲突检测（双检：锁外加载期间可能有并发加载）
            if (loaded_plugins_.contains(desc->guid))
            {
                DAS_CORE_LOG_ERROR(
                    "Plugin GUID conflict: path={}, existing plugin with same GUID",
                    path_str);
                return DAS_E_DUPLICATE_ELEMENT;
            }

            const auto guid = desc->guid;
            loaded_plugins_[guid] = std::move(plugin);
            path_to_guid_[path_str] = guid;

            DAS_CORE_LOG_INFO(
                "Loaded plugin: {} with {} features",
                path_str,
                loaded_plugins_[guid].features.size());

            if (pp_out_package && loaded_plugins_[guid].package)
            {
                *pp_out_package = loaded_plugins_[guid].package.Get();
                (*pp_out_package)->AddRef();
            }

            // Notify ComponentFactoryManager about factory features
            {
                std::vector<FeatureInfo*> plugin_factories;
                for (auto& feat : loaded_plugins_[guid].features)
                {
                    if (feat.feature_type
                            == Das::PluginInterface::
                                DAS_PLUGIN_FEATURE_COMPONENT_FACTORY
                        && feat.interface_ptr)
                    {
                        plugin_factories.push_back(&feat);
                    }
                }
                if (!plugin_factories.empty())
                {
                    component_factory_mgr_.OnPluginLoaded(
                        guid,
                        plugin_factories);
                }
            }
        }

        return DAS_S_OK;
    }

    // IPC 路径：委托给 Host 进程加载
    if (host_exe_path_.empty())
    {
        DAS_CORE_LOG_ERROR(
            "Cannot load plugin '{}' via IPC: no Host exe path set",
            path_str);
        return DAS_E_NO_IMPLEMENTATION;
    }
    return LoadPluginViaIpc(normalized_path, desc->guid, desc);
}

DasResult PluginManager::UnloadPlugin(const std::filesystem::path& path)
{
    std::unique_lock<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    // 路径 -> GUID 查找
    auto path_it = path_to_guid_.find(path_str);
    if (path_it == path_to_guid_.end())
    {
        DAS_CORE_LOG_WARN("Plugin not loaded: {}", path_str);
        return DAS_E_NOT_FOUND;
    }

    auto guid = path_it->second;
    auto plug_it = loaded_plugins_.find(guid);
    if (plug_it == loaded_plugins_.end())
    {
        DAS_CORE_LOG_WARN("Plugin not loaded: {}", path_str);
        return DAS_E_NOT_FOUND;
    }

    bool can_unload = false;
    if (plug_it->second.package)
    {
        auto unload_result = plug_it->second.package->CanUnloadNow(&can_unload);
        if (unload_result != DAS_S_OK || !can_unload)
        {
            DAS_CORE_LOG_WARN("Plugin cannot be unloaded now: {}", path_str);
            return DAS_E_FAIL;
        }
    }

    // 检查是否为 IPC 加载的插件
    if (host_launchers_.contains(guid))
    {
        // 从 loaded_plugins_ 取出 plugin（UnloadPluginIpc 负责 erase）
        LoadedPlugin ipc_plugin = std::move(plug_it->second);
        loaded_plugins_.erase(plug_it);
        path_to_guid_.erase(path_it);
        lock.unlock();
        return UnloadPluginIpc(guid, ipc_plugin);
    }

    // 进程内路径：先从 feature_type_index_ 中移除指针（在 erase
    // loaded_plugins_ 之前）
    component_factory_mgr_.OnPluginUnloading(guid);

    for (auto& feature : plug_it->second.features)
    {
        auto type_it = feature_type_index_.find(feature.feature_type);
        if (type_it != feature_type_index_.end())
        {
            auto& vec = type_it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), &feature), vec.end());
        }
    }

    // 注销对象
    for (auto& feature : plug_it->second.features)
    {
        if (!Core::IPC::IsNullObjectId(feature.object_id))
        {
            registry_->UnregisterObject(feature.object_id);
        }
    }

    loaded_plugins_.erase(plug_it);
    path_to_guid_.erase(path_it);

    DAS_CORE_LOG_INFO("Unloaded plugin: {}", path_str);
    return DAS_S_OK;
}

DasResult PluginManager::GetPlugin(
    const std::filesystem::path&              path,
    Das::PluginInterface::IDasPluginPackage** pp_out_package)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!pp_out_package)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    auto path_it = path_to_guid_.find(path_str);
    if (path_it == path_to_guid_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    auto it = loaded_plugins_.find(path_it->second);
    if (it == loaded_plugins_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    if (!it->second.package)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_package = it->second.package.Get();
    (*pp_out_package)->AddRef();
    return DAS_S_OK;
}

std::vector<std::filesystem::path> PluginManager::GetLoadedPluginPaths() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::filesystem::path> paths;
    paths.reserve(loaded_plugins_.size());

    for (const auto& [guid, plugin] : loaded_plugins_)
    {
        paths.push_back(plugin.plugin_path);
    }

    return paths;
}

DasResult PluginManager::RegisterPluginObjects(
    const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    auto path_it = path_to_guid_.find(path_str);
    if (path_it == path_to_guid_.end())
    {
        DAS_CORE_LOG_WARN("Plugin not loaded: {}", path_str);
        return DAS_E_NOT_FOUND;
    }

    auto it = loaded_plugins_.find(path_it->second);
    if (it == loaded_plugins_.end())
    {
        DAS_CORE_LOG_WARN("Plugin not loaded: {}", path_str);
        return DAS_E_NOT_FOUND;
    }

    auto&    registry = *registry_;
    uint32_t local_id = 1;

    for (auto& feature : it->second.features)
    {
        if (!feature.interface_ptr)
        {
            continue;
        }

        // 创建 ObjectId
        Core::IPC::ObjectId obj_id{};
        obj_id.session_id = session_id_;
        obj_id.generation = 1;
        obj_id.local_id = local_id++;

        uint32_t interface_id = registry.ComputeInterfaceId(feature.iid);
        auto     register_result = registry.RegisterObject(
            obj_id,
            feature.iid,
            interface_id,
            session_id_,
            GetFeatureName(feature.feature_type),
            1);

        if (register_result == DAS_S_OK)
        {
            feature.object_id = obj_id;
            feature_type_index_[feature.feature_type].push_back(&feature);

            DAS_CORE_LOG_INFO(
                "Registered feature {}",
                GetFeatureName(feature.feature_type));
        }
        else
        {
            DAS_CORE_LOG_WARN(
                "Failed to register feature {}: error={}",
                GetFeatureName(feature.feature_type),
                static_cast<int>(register_result));
        }
    }

    return DAS_S_OK;
}

DasResult PluginManager::UnregisterPluginObjects(
    const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    auto path_it = path_to_guid_.find(path_str);
    if (path_it == path_to_guid_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    auto it = loaded_plugins_.find(path_it->second);
    if (it == loaded_plugins_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    auto& registry = *registry_;

    for (auto& feature : it->second.features)
    {
        if (!Core::IPC::IsNullObjectId(feature.object_id))
        {
            registry.UnregisterObject(feature.object_id);
            feature.object_id = Core::IPC::ObjectId{};
        }

        // 从 feature_type_index_ 中移除
        auto type_it = feature_type_index_.find(feature.feature_type);
        if (type_it != feature_type_index_.end())
        {
            auto& vec = type_it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), &feature), vec.end());
        }
    }

    return DAS_S_OK;
}

DasResult PluginManager::GetObjectByFeature(
    Das::PluginInterface::DasPluginFeature feature,
    const DasGuid&                         iid,
    void**                                 pp_out_object)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!pp_out_object)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_object = nullptr;

    auto type_it = feature_type_index_.find(feature);
    if (type_it == feature_type_index_.end() || type_it->second.empty())
    {
        DAS_CORE_LOG_WARN("Feature not found: {}", GetFeatureName(feature));
        return DAS_E_NOT_FOUND;
    }

    auto& feature_info = *type_it->second.front();
    if (!feature_info.interface_ptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // QueryInterface 获取请求的接口
    auto result =
        feature_info.interface_ptr->QueryInterface(iid, pp_out_object);
    return result;
}

std::span<FeatureInfo* const> PluginManager::GetFeaturesByType(
    Das::PluginInterface::DasPluginFeature type) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = feature_type_index_.find(type);
    if (it == feature_type_index_.end())
    {
        return {};
    }

    return it->second;
}

DasResult PluginManager::GetPluginFeatures(
    const std::filesystem::path& path,
    std::vector<FeatureInfo>&    out_features) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    auto path_it = path_to_guid_.find(path_str);
    if (path_it == path_to_guid_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    auto it = loaded_plugins_.find(path_it->second);
    if (it == loaded_plugins_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    out_features = it->second.features;
    return DAS_S_OK;
}

bool PluginManager::IsPluginLoaded(const std::filesystem::path& path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return path_to_guid_.contains(NormalizePath(path).string());
}

size_t PluginManager::GetLoadedPluginCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_plugins_.size();
}

DasGuid PluginManager::GetIidForFeature(
    Das::PluginInterface::DasPluginFeature feature) const
{
    using namespace Das::PluginInterface;

    switch (feature)
    {
    case DAS_PLUGIN_FEATURE_CAPTURE_FACTORY:
        return DasIidOf<IDasCaptureFactory>();
    case DAS_PLUGIN_FEATURE_ERROR_LENS:
        return DasIidOf<IDasErrorLens>();
    case DAS_PLUGIN_FEATURE_TASK:
        return DasIidOf<IDasTask>();
    case DAS_PLUGIN_FEATURE_INPUT_FACTORY:
        return DasIidOf<IDasInputFactory>();
    case DAS_PLUGIN_FEATURE_COMPONENT_FACTORY:
        return DasIidOf<IDasComponentFactory>();
    default:
        return DasGuid{}; // 空 GUID
    }
}

std::string PluginManager::GetFeatureName(
    Das::PluginInterface::DasPluginFeature feature) const
{
    using namespace Das::PluginInterface;

    switch (feature)
    {
    case DAS_PLUGIN_FEATURE_CAPTURE_FACTORY:
        return "CAPTURE_FACTORY";
    case DAS_PLUGIN_FEATURE_ERROR_LENS:
        return "ERROR_LENS";
    case DAS_PLUGIN_FEATURE_TASK:
        return "TASK";
    case DAS_PLUGIN_FEATURE_INPUT_FACTORY:
        return "INPUT_FACTORY";
    case DAS_PLUGIN_FEATURE_COMPONENT_FACTORY:
        return "COMPONENT_FACTORY";
    default:
        return "UNKNOWN";
    }
}

std::filesystem::path PluginManager::NormalizePath(
    const std::filesystem::path& path) const
{
    auto result = std::filesystem::weakly_canonical(path);
    return result;
}

LoadedPlugin* PluginManager::FindPluginByGuid(const DasGuid& guid)
{
    auto it = loaded_plugins_.find(guid);
    if (it == loaded_plugins_.end())
    {
        return nullptr;
    }
    return &it->second;
}

PluginPackageDesc* PluginManager::FindPluginPackageByGuid(const DasGuid& guid)
{
    auto* plugin = FindPluginByGuid(guid);
    if (plugin && plugin->desc)
    {
        return plugin->desc.get();
    }
    return nullptr;
}

ComponentFactoryManager& PluginManager::GetComponentFactoryManager()
{
    return component_factory_mgr_;
}

void PluginManager::RegisterTestFeature(
    Das::PluginInterface::DasPluginFeature type,
    const DasGuid&                         plugin_guid,
    IDasBase*                              interface_ptr)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Allocate a stable FeatureInfo that persists until Shutdown
    static std::vector<std::unique_ptr<FeatureInfo>> test_features;
    auto fi = std::make_unique<FeatureInfo>();
    fi->feature_type = type;
    fi->iid = DasGuid{};
    fi->interface_ptr = interface_ptr;
    fi->plugin_guid = plugin_guid;
    fi->plugin_name = "test_plugin";
    fi->session_id = 0;

    auto* raw = fi.get();
    test_features.push_back(std::move(fi));

    feature_type_index_[type].push_back(raw);
}

DasResult PluginManager::LoadPluginViaIpc(
    const std::filesystem::path&       manifest_path,
    const DasGuid&                     plugin_guid,
    std::shared_ptr<PluginPackageDesc> desc)
{
    // 查找或创建 HostLauncher
    DasPtr<DAS::Core::IPC::IHostLauncher> launcher;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        host_it = host_launchers_.find(plugin_guid);
        if (host_it != host_launchers_.end())
        {
            launcher = host_it->second;
        }
    }

    if (!launcher)
    {
        // 首次加载时创建并启动 Host 进程
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        auto result = ipc_context_.get().CreateHostLauncher(&raw_launcher);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "LoadPluginViaIpc: CreateHostLauncher failed: error={}",
                static_cast<int>(result));
            return result;
        }
        launcher = DasPtr<DAS::Core::IPC::IHostLauncher>(raw_launcher);

        uint16_t session_id = 0;
        result = launcher->Start(host_exe_path_, session_id, 30000);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "LoadPluginViaIpc: HostLauncher::Start failed: error={}",
                static_cast<int>(result));
            return result;
        }

        // 注册进程退出回调 + 心跳超时回调
        // 将 IHostLauncher* 转换为 HostLauncher* 以访问扩展接口
        auto* concrete_launcher =
            static_cast<DAS::Core::IPC::HostLauncher*>(launcher.Get());
        if (concrete_launcher)
        {
            concrete_launcher->SetAssociatedGuid(plugin_guid);

            concrete_launcher->SetOnProcessExit(
                [this, plugin_guid](uint16_t /*sid*/, int exit_code)
                { OnHostProcessExit(plugin_guid, exit_code); });

            concrete_launcher->SetOnHeartbeatTimeout(
                [this](DasGuid guid)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    CleanupPluginByGuid(guid);
                });
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            host_launchers_[plugin_guid] = launcher;
        }
    }

    // 异步加载插件 -> sync_wait 同步等待
    DasPtr<IDasAsyncLoadPluginOperation> op;
    auto result = ipc_context_.get().LoadPluginAsync(
        launcher.Get(),
        manifest_path.string().c_str(),
        op.Put());
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "LoadPluginViaIpc: LoadPluginAsync failed: error={}",
            static_cast<int>(result));
        return result;
    }

    // sync_wait 阻塞等待（HTTP 线程安全，走 IO 线程 pending_calls_）
    auto sender = DAS::Core::IPC::async_op(ipc_context_.get(), std::move(op));
    auto wait_result =
        DAS::Core::IPC::wait(ipc_context_.get(), std::move(sender));
    if (!wait_result)
    {
        DAS_CORE_LOG_ERROR(
            "LoadPluginViaIpc: sync_wait timed out for path={}",
            manifest_path.string());
        return DAS_E_IPC_REMOTE_ERROR;
    }

    auto [ipc_result, raw_proxy] = *wait_result;
    if (DAS::IsFailed(ipc_result) || !raw_proxy)
    {
        DAS_CORE_LOG_ERROR(
            "LoadPluginViaIpc: IPC load failed: error={}",
            static_cast<int>(ipc_result));
        return ipc_result;
    }

    // 获取 IDasPluginPackage proxy
    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    auto qi_result = raw_proxy->QueryInterface(
        DAS_IID_PLUGIN_PACKAGE,
        reinterpret_cast<void**>(package.Put()));
    raw_proxy->Release();

    if (DAS::IsFailed(qi_result))
    {
        DAS_CORE_LOG_ERROR(
            "LoadPluginViaIpc: plugin does not implement IDasPluginPackage: "
            "path={}",
            manifest_path.string());
        return DAS_E_NO_INTERFACE;
    }

    // 构造 LoadedPlugin 并更新索引
    LoadedPlugin plugin;
    plugin.plugin_path = manifest_path;
    plugin.package = std::move(package);
    plugin.desc = desc;

    const auto guid = desc->guid;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 双检：IPC 加载期间可能有并发加载
        if (loaded_plugins_.contains(guid))
        {
            DAS_CORE_LOG_ERROR(
                "LoadPluginViaIpc: GUID conflict after IPC load: path={}",
                manifest_path.string());
            return DAS_E_DUPLICATE_ELEMENT;
        }

        loaded_plugins_[guid] = std::move(plugin);
        path_to_guid_[manifest_path.string()] = guid;
    }

    DAS_CORE_LOG_INFO("Loaded plugin via IPC: {}", manifest_path.string());

    return DAS_S_OK;
}

DasResult PluginManager::UnloadPluginIpc(
    const DasGuid& guid,
    LoadedPlugin&  plugin)
{
    // 通过 IPC proxy 调用 CanUnloadNow
    if (plugin.package)
    {
        bool can_unload = false;
        auto unload_result = plugin.package->CanUnloadNow(&can_unload);
        if (unload_result != DAS_S_OK || !can_unload)
        {
            DAS_CORE_LOG_WARN("UnloadPluginIpc: plugin cannot be unloaded now");
            return DAS_E_FAIL;
        }
    }

    // 从索引中移除（必须在 Stop 之前，Stop 可能触发断连回调）
    {
        std::lock_guard<std::mutex> lock(mutex_);
        component_factory_mgr_.OnPluginUnloading(guid);
        path_to_guid_.erase(plugin.plugin_path.string());
        loaded_plugins_.erase(guid);
    }

    // 发送 GOODBYE 让 Host 优雅退出
    // HostLauncher::Stop() 内部发送 GOODBYE + 等待进程退出
    auto host_it = host_launchers_.find(guid);
    if (host_it != host_launchers_.end())
    {
        auto launcher = host_it->second;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            host_launchers_.erase(host_it);
        }
        launcher->Stop();
    }

    DAS_CORE_LOG_INFO("Unloaded plugin via IPC");
    return DAS_S_OK;
}

void PluginManager::CleanupPluginByGuid(DasGuid plugin_guid)
{
    // 调用方已持有 mutex_

    // 清理 loaded_plugins_ 索引
    auto plugin_it = loaded_plugins_.find(plugin_guid);
    if (plugin_it != loaded_plugins_.end())
    {
        component_factory_mgr_.OnPluginUnloading(plugin_guid);
        path_to_guid_.erase(plugin_it->second.plugin_path.string());
        loaded_plugins_.erase(plugin_it);
    }

    // 清理 HostLauncher
    host_launchers_.erase(plugin_guid);

    DAS_CORE_LOG_INFO("Cleaned up plugin index by GUID");
}

void PluginManager::OnHostProcessExit(DasGuid plugin_guid, int exit_code)
{
    // 此回调在 io_context 线程上执行
    DAS_CORE_LOG_WARN(
        "Host process exited unexpectedly: exit_code={}",
        exit_code);

    std::lock_guard<std::mutex> lock(mutex_);
    CleanupPluginByGuid(plugin_guid);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
