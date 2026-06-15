#include <das/Core/ForeignInterfaceHost/PluginManager.h>

#include <cpp_yyjson.hpp>
#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    std::filesystem::path ResolveManifestPath(
        const std::filesystem::path& normalized_path)
    {
        if (std::filesystem::is_directory(normalized_path))
        {
            return FindManifest(normalized_path);
        }

        if (normalized_path.extension() == ".json")
        {
            return normalized_path;
        }

        return {};
    }

    std::filesystem::path ResolveManifestIdentityPath(
        const std::filesystem::path& normalized_path)
    {
        auto manifest_path = ResolveManifestPath(normalized_path);
        if (!manifest_path.empty())
        {
            return std::filesystem::weakly_canonical(manifest_path);
        }

        return normalized_path;
    }

    bool IsFolderModeManifest(
        const std::filesystem::path& normalized_path,
        const std::filesystem::path& manifest_path)
    {
        if (manifest_path.empty())
        {
            return false;
        }

        if (std::filesystem::is_directory(normalized_path))
        {
            return true;
        }

        if (manifest_path.filename() == "manifest.json")
        {
            return true;
        }

        return manifest_path.filename().u8string()
               == manifest_path.parent_path().filename().u8string() + u8".json";
    }

    std::filesystem::path ResolveNodeModulesRoot(
        const std::filesystem::path& normalized_path,
        const std::filesystem::path& manifest_path)
    {
        if (manifest_path.empty())
        {
            return {};
        }

        const auto package_root = manifest_path.parent_path();
        if (IsFolderModeManifest(normalized_path, manifest_path))
        {
            return package_root.parent_path() / "node_modules";
        }

        return package_root / "node_modules";
    }

    std::vector<FeatureInfo*> CollectFeaturePointers(
        LoadedPlugin&                          plugin,
        Das::PluginInterface::DasPluginFeature feature_type)
    {
        std::vector<FeatureInfo*> result;
        for (auto& feature : plugin.features)
        {
            if (feature.feature_type == feature_type && feature.interface_ptr)
            {
                result.push_back(&feature);
            }
        }
        return result;
    }
} // namespace

PluginManager::PluginManager(
    Das::Core::SettingsManager::SettingsManager& settings_manager,
    Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
    : settings_manager_(settings_manager), ipc_context_{std::move(ipc_context)}
{
    remote_plugin_host_factory_ = [this]()
    { return std::make_unique<IpcRemotePluginHost>(ipc_context_); };
}

PluginManager::~PluginManager()
{
    // CR-02 析构路径 drain：必须先 drain 在途回调，再释放其它成员。
    // Shutdown 调 ipc_context_->ResetHostLifecycleCallbacks()（经 IIpcContext
    // 虚方法 dispatch 到 IpcContext::ResetHostLifecycleCallbacks，遍历真实
    // launchers_ 清空 on_process_exit_slot_ / on_heartbeat_timeout_slot_），
    // 使心跳线程后续 Invoke 空转（callback_ 已置空），不再回调已析构的 this。
    // 若用户已显式调过 Shutdown，二次调用幂等（索引已空，drain 空转安全）。
    Shutdown();
    ClearActiveErrorLensManager(&error_lens_mgr_);
}

DasResult PluginManager::Initialize(uint16_t session_id)
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
    SetActiveErrorLensManager(&error_lens_mgr_);

    DAS_CORE_LOG_INFO(
        "PluginManager initialized with session_id={}",
        session_id_);
    return DAS_S_OK;
}

DasResult PluginManager::Shutdown()
{
    // CR-03 / INV-03: Shutdown 采用方案 A 两阶段 —— 阶段 1 在 mutex_ 锁内
    // 清空所有索引（这些操作不阻塞、不碰 callback_mutex_，锁内安全）；阶段 2
    // 在释放 mutex_ 后调用 ipc_context_->ResetHostLifecycleCallbacks()
    // （经 IIpcContext 虚方法 dispatch 到
    // IpcContext::ResetHostLifecycleCallbacks， 遍历真实 launchers_ 清空
    // on_process_exit_slot_ / on_heartbeat_timeout_slot_）。锁序保持
    // callback_mutex_ -> mutex_ 正向， 消除旧版持 mutex_ 调 ClearCallbacks/Stop
    // 的 AB-BA 死锁向量。
    {
        std::lock_guard<std::mutex> lock(mutex_);

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
            error_lens_mgr_.OnPluginUnloading(guid);
            component_factory_mgr_.OnPluginUnloading(guid);
            task_component_factory_mgr_.OnPluginUnloading(guid);
        }

        feature_type_index_.clear();
        path_to_guid_.clear();
        loaded_plugins_.clear();
        session_id_ = 0;
        ClearActiveErrorLensManager(&error_lens_mgr_);
    } // 释放 mutex_ —— 阶段 2 锁外调 ResetHostLifecycleCallbacks（INV-03）

    // 阶段 2：锁外 ResetHostLifecycleCallbacks（经 ipc_context_ 接口转发到
    // 真实 launchers_，纯 RAII 析构 drain，非两段式，语义像 ~ofstream 的
    // flush）。 不再依赖已删除的 host_launchers_
    // 死字段。ResetHostLifecycleCallbacks 内部调 GuardedCallback::Clear 持
    // callback_mutex_ drain 在途回调（CR-02 drain 屏障），让心跳线程后续 Invoke
    // 空转碰不到悬空 [this]。
    ipc_context_.get().ResetHostLifecycleCallbacks();

    DAS_CORE_LOG_INFO("PluginManager shutdown complete");
    return DAS_S_OK;
}

void PluginManager::SetRemotePluginHostFactoryForTest(
    std::function<std::unique_ptr<IRemotePluginHost>()> factory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    remote_plugin_host_factory_ = std::move(factory);
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
    DasOutPtr<Das::PluginInterface::IDasPluginPackage> out_package(
        pp_out_package);

    // 阶段1: 锁内快速检查 + manifest 解析
    std::filesystem::path              normalized_path;
    std::filesystem::path              manifest_path;
    std::filesystem::path              identity_path;
    std::filesystem::path              runtime_path;
    std::string                        path_str;
    std::shared_ptr<PluginPackageDesc> desc;
    DasResult                          manifest_parse_result = DAS_S_OK;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        normalized_path = NormalizePath(path);
        manifest_path = ResolveManifestPath(normalized_path);
        identity_path = ResolveManifestIdentityPath(normalized_path);
        runtime_path = !manifest_path.empty() ? identity_path : normalized_path;
        path_str =
            std::string{DAS::Utils::U8AsString(identity_path.u8string())};

        // Canonical manifest path deduplication. Directory input is an alias
        // to the manifest it resolves to.
        if (path_to_guid_.contains(path_str))
        {
            DAS_CORE_LOG_WARN("Plugin already loaded: {}", path_str);
            auto it = loaded_plugins_.find(path_to_guid_[path_str]);
            if (pp_out_package && it != loaded_plugins_.end()
                && it->second.package)
            {
                out_package.Set(it->second.package.Get());
                out_package.Keep();
            }
            return DAS_S_FALSE;
        }

        // 解析 manifest JSON 获取 PluginPackageDesc（含 guid）
        desc = std::make_shared<PluginPackageDesc>();

        if (!manifest_path.empty() && std::filesystem::exists(manifest_path))
        {
            std::ifstream ifs(manifest_path);
            if (ifs.is_open())
            {
                try
                {
                    std::string content(
                        (std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
                    auto parsed = Das::Utils::ParseYyjsonFromString(
                        content,
                        yyjson::ReadFlag::AllowComments
                            | yyjson::ReadFlag::AllowTrailingCommas);
                    if (parsed)
                    {
                        const auto& const_val = *parsed;
                        auto        obj = const_val.as_object();
                        if (obj)
                        {
                            ParsePluginPackageDescFromJson(*obj, *desc);
                        }
                    }
                    else
                    {
                        DAS_CORE_LOG_WARN(
                            "Failed to parse manifest for {}",
                            path_str);
                        manifest_parse_result = DAS_E_INVALID_JSON;
                    }
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_WARN(
                        "Failed to parse manifest for {}: {}",
                        path_str,
                        e.what());
                    manifest_parse_result = DAS_E_INVALID_JSON;
                }
            }
        }
    }

    if (DAS::IsFailed(manifest_parse_result))
    {
        return manifest_parse_result;
    }

    // 阶段2: 锁外执行加载，所有 runtime 都返回统一 load result
    RuntimeLoadRequest request{};
    request.manifest_path = manifest_path;
    request.runtime_path = runtime_path;
    request.plugin_guid = desc->guid;
    request.language = desc->language;
    request.load_mode = desc->load_mode;
    request.main_process_owner_session_id = session_id_;
    if (request.language == ForeignInterfaceLanguage::Node)
    {
        request.node_modules_root =
            ResolveNodeModulesRoot(normalized_path, manifest_path);
    }

    // IPC 模式下注入生命周期回调，使 Host 进程崩溃或心跳超时时
    // PluginManager 能自动清理插件索引。
    // guid 按值捕获（从 desc->guid 复制），保证 lambda 生命周期安全。
    if (request.load_mode == LoadMode::Ipc)
    {
        const auto guid = desc->guid;
        request.on_process_exit =
            [this, guid](uint16_t /*session_id*/, int exit_code)
        { OnHostProcessExit(guid, exit_code); };
        request.on_heartbeat_timeout = [this](DasGuid callback_guid)
        { OnHeartbeatTimeout(callback_guid); };
    }

    std::unique_ptr<IRuntimeProvider> scoped_provider;
    IRuntimeProvider*                 provider = nullptr;

    {
        RuntimeProviderFactoryDesc provider_desc{};
        provider_desc.language = desc->language;
        provider_desc.load_mode = desc->load_mode;
        provider_desc.native_host_exe_path = host_exe_path_;
        if (remote_plugin_host_factory_)
        {
            provider_desc.remote_plugin_host = remote_plugin_host_factory_();
        }

        auto selected_provider =
            CreateRuntimeProvider(std::move(provider_desc));
        if (!selected_provider)
        {
            if (selected_provider.error() == DAS_E_NO_IMPLEMENTATION)
            {
                DAS_CORE_LOG_ERROR(
                    "No runtime provider for plugin '{}'",
                    path_str);
            }
            return selected_provider.error();
        }
        scoped_provider = std::move(selected_provider.value());
        provider = scoped_provider.get();
    }

    if (!provider)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    auto load_result = provider->LoadPlugin(request);
    if (!load_result)
    {
        DAS_CORE_LOG_ERROR(
            "Failed to load plugin {}: error={}",
            path_str,
            static_cast<int>(load_result.error()));
        return load_result.error();
    }
    if (!load_result->object)
    {
        return DAS_E_INVALID_POINTER;
    }

    LoadedPlugin plugin;
    plugin.plugin_path = identity_path;

    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    auto qi_result = load_result->object->QueryInterface(
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
        feature_info.feature_index = index;
        feature_info.feature_type = feature;
        feature_info.iid = GetIidForFeature(feature);
        feature_info.session_id = load_result->owner_session_id;
        feature_info.plugin_name = std::string{
            DAS::Utils::U8AsString(identity_path.stem().u8string())};
        feature_info.plugin_guid = desc->guid;

        DasPtr<IDasBase> p_interface = nullptr;
        auto             create_result =
            plugin.package->CreateFeatureInterface(index, p_interface.Put());
        if (create_result == DAS_S_OK)
        {
            feature_info.interface_ptr = p_interface;
        }

        plugin.features.push_back(std::move(feature_info));
        ++index;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (loaded_plugins_.contains(desc->guid))
        {
            DAS_CORE_LOG_ERROR(
                "Plugin GUID conflict: path={}, existing plugin with same GUID",
                path_str);
            return DAS_E_DUPLICATE_ELEMENT;
        }

        const auto guid = desc->guid;
        // 记录 IPC owner_session_id（RuntimeLoadResult.owner_session_id 已在
        // IpcRemotePluginHost.cpp:112/208 设置），用于运行时卸载
        // (UnloadPluginIpc 经 session_id 调 ipc_context_
        // ->UnregisterHostLauncherBySession)。
        plugin.session_id = load_result->owner_session_id;
        loaded_plugins_[guid] = std::move(plugin);
        path_to_guid_[path_str] = guid;

        DAS_CORE_LOG_INFO(
            "Loaded plugin: {} with {} features",
            path_str,
            loaded_plugins_[guid].features.size());

        auto plugin_factories = CollectFeaturePointers(
            loaded_plugins_[guid],
            Das::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
        if (!plugin_factories.empty())
        {
            component_factory_mgr_.OnPluginLoaded(guid, plugin_factories);
        }

        auto error_lens_features = CollectFeaturePointers(
            loaded_plugins_[guid],
            Das::PluginInterface::DAS_PLUGIN_FEATURE_ERROR_LENS);
        const auto error_lens_registration_result =
            error_lens_mgr_.OnPluginLoaded(guid, error_lens_features);
        if (DAS::IsFailed(error_lens_registration_result))
        {
            error_lens_mgr_.OnPluginUnloading(guid);
            component_factory_mgr_.OnPluginUnloading(guid);
            path_to_guid_.erase(path_str);
            loaded_plugins_.erase(guid);
            return error_lens_registration_result;
        }

        auto task_component_factories = CollectFeaturePointers(
            loaded_plugins_[guid],
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY);
        const auto task_component_registration_result =
            task_component_factory_mgr_.OnPluginLoaded(
                guid,
                task_component_factories,
                loaded_plugins_[guid].desc->task_components);
        if (DAS::IsFailed(task_component_registration_result))
        {
            error_lens_mgr_.OnPluginUnloading(guid);
            task_component_factory_mgr_.OnPluginUnloading(guid);
            component_factory_mgr_.OnPluginUnloading(guid);
            path_to_guid_.erase(path_str);
            loaded_plugins_.erase(guid);
            return task_component_registration_result;
        }

        if (pp_out_package && loaded_plugins_[guid].package)
        {
            out_package.Set(loaded_plugins_[guid].package.Get());
        }
    }

    out_package.Keep();
    return DAS_S_OK;
}

DasResult PluginManager::UnloadPlugin(const std::filesystem::path& path)
{
    std::unique_lock<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = std::string{DAS::Utils::U8AsString(
        ResolveManifestIdentityPath(normalized_path).u8string())};

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

    // 检查是否为 IPC 加载的插件（LoadedPlugin.session_id != 0 表示 IPC 加载）
    if (plug_it->second.session_id != 0)
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
    error_lens_mgr_.OnPluginUnloading(guid);
    component_factory_mgr_.OnPluginUnloading(guid);
    task_component_factory_mgr_.OnPluginUnloading(guid);

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
    DAS_UTILS_CHECK_POINTER(pp_out_package)

    DasOutPtr<Das::PluginInterface::IDasPluginPackage> result(pp_out_package);

    std::lock_guard<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = std::string{DAS::Utils::U8AsString(
        ResolveManifestIdentityPath(normalized_path).u8string())};

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

    result.Set(it->second.package.Get());
    result.Keep();
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
    auto path_str = std::string{DAS::Utils::U8AsString(
        ResolveManifestIdentityPath(normalized_path).u8string())};

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

        feature_type_index_[feature.feature_type].push_back(&feature);

        if (register_result == DAS_S_OK)
        {
            feature.object_id = obj_id;

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
    auto path_str = std::string{DAS::Utils::U8AsString(
        ResolveManifestIdentityPath(normalized_path).u8string())};

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

    if (feature == Das::PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY
        && iid == DasIidOf<Das::PluginInterface::IDasInputFactory>())
    {
        Das::PluginInterface::IDasInputFactory* p_factory = nullptr;
        const auto result = feature_info.interface_ptr->QueryInterface(
            iid,
            reinterpret_cast<void**>(&p_factory));
        if (DAS::IsFailed(result) || !p_factory)
        {
            return result;
        }

        auto factory =
            DasPtr<Das::PluginInterface::IDasInputFactory>::Attach(p_factory);
        auto decorated_factory = Das::Core::Debug::MaybeDecorateInputFactory(
            std::move(factory),
            feature_info.plugin_name.c_str());
        if (!decorated_factory)
        {
            return DAS_E_INVALID_POINTER;
        }

        decorated_factory->AddRef();
        *pp_out_object = decorated_factory.Get();
        return result;
    }

    // QueryInterface 获取请求的接口
    return feature_info.interface_ptr->QueryInterface(iid, pp_out_object);
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

DasResult PluginManager::CreateFeatureInterface(
    const DasGuid& plugin_guid,
    uint64_t       feature_index,
    const DasGuid& iid,
    void**         pp_out_object)
{
    if (!pp_out_object)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_object = nullptr;

    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = loaded_plugins_.find(plugin_guid);
        if (it == loaded_plugins_.end() || !it->second.package)
        {
            return DAS_E_NOT_FOUND;
        }
        package = it->second.package;
    }

    DasPtr<IDasBase> feature_base;
    auto             create_result =
        package->CreateFeatureInterface(feature_index, feature_base.Put());
    if (DAS::IsFailed(create_result))
    {
        return create_result;
    }

    return feature_base->QueryInterface(iid, pp_out_object);
}

DasResult PluginManager::GetPluginFeatures(
    const std::filesystem::path& path,
    std::vector<FeatureInfo>&    out_features) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = std::string{DAS::Utils::U8AsString(
        ResolveManifestIdentityPath(normalized_path).u8string())};

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
    return path_to_guid_.contains(
        std::string{DAS::Utils::U8AsString(
            ResolveManifestIdentityPath(NormalizePath(path)).u8string())});
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
    case DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY:
        return DasIidOf<IDasTaskAuthoringSessionFactory>();
    case DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY:
        return DasIidOf<IDasTaskComponentFactory>();
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
    case DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY:
        return "TASK_AUTHORING_FACTORY";
    case DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY:
        return "TASK_COMPONENT_FACTORY";
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

TaskComponentFactoryManager& PluginManager::GetTaskComponentFactoryManager()
{
    return task_component_factory_mgr_;
}

ErrorLensManager& PluginManager::GetErrorLensManager()
{
    return error_lens_mgr_;
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
    fi->feature_index = 0;
    fi->feature_type = type;
    fi->iid = GetIidForFeature(type);
    fi->interface_ptr = interface_ptr;
    fi->plugin_guid = plugin_guid;
    fi->plugin_name = "test_plugin";
    fi->session_id = 0;

    auto* raw = fi.get();
    test_features.push_back(std::move(fi));

    feature_type_index_[type].push_back(raw);
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
            DAS_CORE_LOG_WARN("IPC plugin cannot be unloaded now");
            return DAS_E_FAIL;
        }
    }

    // 从索引中移除（必须在 Stop 之前，Stop 可能触发断连回调）
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_lens_mgr_.OnPluginUnloading(guid);
        component_factory_mgr_.OnPluginUnloading(guid);
        task_component_factory_mgr_.OnPluginUnloading(guid);
        path_to_guid_.erase(
            std::string{DAS::Utils::U8AsString(plugin.plugin_path.u8string())});
        loaded_plugins_.erase(guid);
    }

    // 经 ipc_context_ 接口按 session 主动 Stop Host 进程 + 清 ConnectionManager
    // 索引。 HostLauncher 所有权唯一归 IpcContext::launchers_ +
    // ConnectionManager::hosts_ （PluginManager 不持有，host_launchers_
    // 死字段已删）。本调用在 mutex_ 锁外 （上面内层 lock_guard
    // 作用域已结束），满足 INV-03。UnregisterHostLauncherBySession 在
    // IpcContext override 内组合 launcher->Stop()（含 GOODBYE + 等待进程退出 +
    // ClearCallbacks 全清三 slot，保留运行时卸载"主动 Stop Host 进程"语义）+
    // runloop_.GetConnectionManager().UnregisterHostLauncher（清
    // hosts_/connections_ 索引）。
    return ipc_context_.get().UnregisterHostLauncherBySession(
        plugin.session_id);
}

void PluginManager::CleanupPluginByGuid(DasGuid plugin_guid)
{
    // 调用方已持有 mutex_

    // 清理 loaded_plugins_ 索引
    auto plugin_it = loaded_plugins_.find(plugin_guid);
    if (plugin_it != loaded_plugins_.end())
    {
        error_lens_mgr_.OnPluginUnloading(plugin_guid);
        component_factory_mgr_.OnPluginUnloading(plugin_guid);
        task_component_factory_mgr_.OnPluginUnloading(plugin_guid);
        path_to_guid_.erase(
            std::string{DAS::Utils::U8AsString(
                plugin_it->second.plugin_path.u8string())});
        loaded_plugins_.erase(plugin_it);
    }

    // HostLauncher 清理由 IpcContext/ConnectionManager 按 session 自行管理，
    // 本方法不再持有 launcher 索引（host_launchers_ 死字段已删）。满足
    // INV-02（只清 loaded_plugins_ / path_to_guid_，不调
    // ClearCallbacks/Stop）。

    DAS_CORE_LOG_INFO("Cleaned up plugin index by GUID");
}

void PluginManager::OnHostProcessExit(DasGuid plugin_guid, int exit_code)
{
    // 此回调在 io_context 线程上执行（exit-watcher 协程 co_spawn 在 io_context
    // 上），锁序：callback_mutex_（HostLauncher exit-watcher slot）-> mutex_
    // （INV-03）。与 OnHeartbeatTimeout 路径锁序对称。
    DAS_CORE_LOG_WARN(
        "Host process exited unexpectedly: exit_code={}",
        exit_code);

    std::lock_guard<std::mutex> lock(mutex_);
    CleanupPluginByGuid(plugin_guid);
}

void PluginManager::OnHeartbeatTimeout(DasGuid plugin_guid)
{
    // 此回调在 ConnectionManager::heartbeat_thread_ 上执行（非 io_context
    // 线程）， 锁序：callback_mutex_（HostLauncher）-> mutex_（INV-03）。
    // heartbeat_thread_ 由 ConnectionManager 持有并在析构时 join。
    DAS_CORE_LOG_WARN("Heartbeat timeout for plugin, cleaning up index");

    std::lock_guard<std::mutex> lock(mutex_);
    CleanupPluginByGuid(plugin_guid);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
