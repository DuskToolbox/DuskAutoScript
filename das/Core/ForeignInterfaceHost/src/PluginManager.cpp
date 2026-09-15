#include <das/Core/ForeignInterfaceHost/PluginManager.h>

#include <cpp_yyjson.hpp>
#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
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
#include <ranges>
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

        if (normalized_path.extension() == DAS_PATH_LITERAL(".json"))
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

        if (manifest_path.filename() == DAS_PATH_LITERAL("manifest.json"))
        {
            return true;
        }

        return Das::Utils::PathToUtf8(manifest_path.filename())
               == Das::Utils::PathToUtf8(manifest_path.parent_path().filename())
                      + ".json";
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
            return package_root.parent_path()
                   / DAS_PATH_LITERAL("node_modules");
        }

        return package_root / DAS_PATH_LITERAL("node_modules");
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
    Das::Core::SettingsManager::SettingsManager&          settings_manager,
    Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
    : settings_manager_(settings_manager), ipc_context_{std::move(ipc_context)}
{
    // host 本尊的 DOM 显式注册（显式 pin）：停机链确定性注销的记账锚点，
    // 跨进程注入经代理编组时的 auto-register 与之 dedup 共存
    task_component_factory_mgr_.SetIpcContext(ipc_context_);
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
    const std::scoped_lock lock(mutex_);

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
    // 插件卸载触碰线程亲和资源（GIL/解释器），须与加载同在 BusinessThread。
    // 非 BT 调用时派发到 BT 执行并等待；失败（停机窗口/超时）回退本线程
    // （Shutdown 幂等）。
    if (!DAS::Core::IPC::IsCurrentBusinessThread())
    {
        auto shutdown_sender =
            stdexec::schedule(
                DAS::Core::IPC::schedule_on_business_thread(ipc_context_.get()))
            | stdexec::then([this]() { return Shutdown(); });
        if (auto wait_result = DAS::Core::IPC::wait(
                ipc_context_.get(),
                std::move(shutdown_sender)))
        {
            return std::get<0>(*wait_result);
        }
        DAS_CORE_LOG_ERROR(
            "PluginManager::Shutdown: BusinessThread dispatch wait failed, "
            "falling back to caller thread");
    }

    // CR-03 / INV-03: Shutdown 采用方案 A 两阶段 —— 阶段 1 在 mutex_ 锁内
    // 清空所有索引（这些操作不阻塞、不碰 callback_mutex_，锁内安全）；阶段 2
    // 在释放 mutex_ 后调用 ipc_context_->ResetHostLifecycleCallbacks()
    // （经 IIpcContext 虚方法 dispatch 到
    // IpcContext::ResetHostLifecycleCallbacks， 遍历真实 launchers_ 清空
    // on_process_exit_slot_ / on_heartbeat_timeout_slot_）。锁序保持
    // callback_mutex_ -> mutex_ 正向， 消除旧版持 mutex_ 调 ClearCallbacks/Stop
    // 的 AB-BA 死锁向量。
    // shutdown_notify_guids: 释锁后通知 lifecycle listener 用的 guid 副本集合
    // （D-87-08 两阶段 collect 持锁 -> 释 mutex_ -> fire）。
    std::vector<DasGuid> shutdown_notify_guids;
    {
        const std::scoped_lock lock(mutex_);

        // 卸载所有插件；collect guid 副本用于释锁后通知 lifecycle listener。
        for (auto& [guid, plugin] : loaded_plugins_)
        {
            // 通知 ComponentFactoryManager 释放工厂引用
            error_lens_mgr_.OnPluginUnloading(guid);
            component_factory_mgr_.OnPluginUnloading(guid);
            task_component_factory_mgr_.OnPluginUnloading(guid);
            shutdown_notify_guids.push_back(guid);
        }

        feature_type_index_.clear();
        path_to_guid_.clear();
        loaded_plugins_.clear();
        session_id_ = 0;
        ClearActiveErrorLensManager(&error_lens_mgr_);
    } // 释放 mutex_ —— 阶段 2 锁外调 ResetHostLifecycleCallbacks（INV-03）

    // 锁外触发 lifecycle listener（Unloading per plugin）：上面 scoped_lock
    // 作用域已结束，满足 INV-03 与订阅者无 AB-BA 约束（D-87-08 全清语义下，
    // 订阅者对首个事件即全清 cache，后续事件空转，但仍按 per-plugin 通知
    // 保持语义对称）。
    for (const auto& guid : shutdown_notify_guids)
    {
        FireLifecycleListener(PluginLifecyclePhase::Unloading, guid);
    }

    // 阶段 2：锁外 ResetHostLifecycleCallbacks（经 ipc_context_ 接口转发到
    // 真实 launchers_，纯 RAII 析构 drain，非两段式，语义像 ~ofstream 的
    // flush）。 不再依赖已删除的 host_launchers_
    // 死字段。ResetHostLifecycleCallbacks 内部调 GuardedCallback::Clear 持
    // callback_mutex_ drain 在途回调（CR-02 drain 屏障），让心跳线程后续 Invoke
    // 空转碰不到悬空 [this]。
    ipc_context_.get().ResetHostLifecycleCallbacks();

    // host 本尊的 DOM 条目确定性注销：先于成员析构消亡，迟到 proxy 调用
    // 收 OBJECT_NOT_FOUND；TCFM 析构兜底注销幂等（registered 守卫）
    task_component_factory_mgr_.Shutdown();

    DAS_CORE_LOG_INFO("PluginManager shutdown complete");
    return DAS_S_OK;
}

void PluginManager::SetHostExePath(const std::string& path)
{
    host_exe_path_ = path;
}

void PluginManager::SetPluginLifecycleListener(
    IPluginLifecycleListener* listener)
{
    const std::scoped_lock lock(mutex_);
    lifecycle_listener_ = listener;
}

void PluginManager::FireLifecycleListener(
    PluginLifecyclePhase phase,
    const DasGuid&       guid)
{
    // 调用契约：调用方已释放 mutex_（4 个 fire 点的两阶段范式负责释锁），
    // 本方法不 acquire / release 任何锁。listener 未注册时空转。
    if (lifecycle_listener_)
    {
        lifecycle_listener_->OnPluginLifecycleEvent(phase, guid);
    }
}

void PluginManager::RegisterRuntimeProvider(
    ForeignInterfaceLanguage          language,
    LoadMode                          load_mode,
    std::shared_ptr<IRuntimeProvider> provider)
{
    const std::scoped_lock lock(mutex_);
    registered_providers_.push_back(
        RegisteredRuntimeProvider{language, load_mode, std::move(provider)});
}

std::shared_ptr<IRuntimeProvider> PluginManager::FindRegisteredRuntimeProvider(
    ForeignInterfaceLanguage language,
    LoadMode                 load_mode) const
{
    const std::scoped_lock lock(mutex_);
    // 逆序遍历：后注册者优先（便于测试覆盖内置）。
    for (const auto& registered_provider :
         std::views::reverse(registered_providers_))
    {
        if (registered_provider.language == language
            && registered_provider.load_mode == load_mode)
        {
            return registered_provider.provider;
        }
    }
    return nullptr;
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
        const std::scoped_lock lock(mutex_);

        normalized_path = NormalizePath(path);
        manifest_path = ResolveManifestPath(normalized_path);
        identity_path = ResolveManifestIdentityPath(normalized_path);
        runtime_path = !manifest_path.empty() ? identity_path : normalized_path;
        path_str = Das::Utils::PathToUtf8(identity_path);

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
                    const std::string content(
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

    // 阶段2: 锁外执行加载。path 转 UTF-8 const char*（借用，存活到 LoadPlugin
    // 返回）。
    const std::string manifest_path_u8 = Das::Utils::PathToUtf8(manifest_path);
    const std::string runtime_path_u8 = Das::Utils::PathToUtf8(runtime_path);
    std::string       node_modules_root_u8;
    if (desc->language == ForeignInterfaceLanguage::Node)
    {
        node_modules_root_u8 = Das::Utils::PathToUtf8(
            ResolveNodeModulesRoot(normalized_path, manifest_path));
    }

    RuntimeLoadRequest request{};
    request.manifest_path = manifest_path_u8.c_str();
    request.runtime_path = runtime_path_u8.c_str();
    request.plugin_guid = desc->guid;
    request.main_process_owner_session_id = session_id_;
    request.node_modules_root =
        node_modules_root_u8.empty() ? nullptr : node_modules_root_u8.c_str();

    // IPC 生命周期回调（构造内置 provider 时传入；Host 崩溃/心跳超时触发，
    // PluginManager 据此清理插件索引）。guid 按值捕获，lambda 生命周期安全。
    RuntimeLifecycleCallbacks callbacks{};
    if (desc->load_mode == LoadMode::Ipc)
    {
        const auto guid = desc->guid;
        callbacks.on_process_exit =
            [this, guid](uint16_t /*session_id*/, int exit_code)
        { OnHostProcessExit(guid, exit_code); };
        callbacks.on_heartbeat_timeout = [this](DasGuid callback_guid)
        { OnHeartbeatTimeout(callback_guid); };
    }

    // 选 provider：先查外部注册表（进程内注入扩展点），命中优先；
    // 否则走内置 CreateRuntimeProvider。
    std::unique_ptr<IRuntimeProvider> scoped_provider;     // 内置 provider 持有
    std::shared_ptr<IRuntimeProvider> registered_provider; // 注册 provider 持有
    registered_provider =
        FindRegisteredRuntimeProvider(desc->language, desc->load_mode);

    IRuntimeProvider* provider = nullptr;
    if (registered_provider)
    {
        provider = registered_provider.get();
    }
    else
    {
        RuntimeProviderFactoryDesc provider_desc{};
        provider_desc.language = desc->language;
        provider_desc.load_mode = desc->load_mode;
        provider_desc.native_host_exe_path =
            Das::Utils::PathFromUtf8(host_exe_path_);
        if (remote_plugin_host_factory_)
        {
            provider_desc.remote_plugin_host = remote_plugin_host_factory_();
        }

        auto selected_provider = CreateRuntimeProvider(
            std::move(provider_desc),
            std::move(callbacks));
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

    if (provider == nullptr)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    RuntimeLoadResult load_result{};
    const auto        load_hr = provider->LoadPlugin(request, &load_result);
    if (DAS::IsFailed(load_hr))
    {
        DAS_CORE_LOG_ERROR(
            "Failed to load plugin {}: error={}",
            path_str,
            static_cast<int>(load_hr));
        return load_hr;
    }
    if (load_result.object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    // 接管 object（已 AddRef），析构时 Release。
    const DasPtr<IDasBase> object_owner =
        DasPtr<IDasBase>::Attach(load_result.object);

    LoadedPlugin plugin;
    plugin.plugin_path = identity_path;

    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    auto qi_result = object_owner->QueryInterface(
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

    // 顺序契约（IDasPluginPackage）：首次 EnumFeature 之前注入 task
    // component host 本尊。跨进程加载时 package 为代理——注入经代理调用，
    // [in] 接口参数自动编组（DOM 注册 + 对端生成 host 代理），与工厂注入
    // 同型。失败仅告警并继续加载——实现者显式返回失败（如跨进程编组
    // 出错）时加载无需中断，host 消费方在创建组件时自行失败即可。
    const auto inject_host_result = plugin.package->SetTaskComponentHost(
        task_component_factory_mgr_.GetTaskComponentHost());
    if (DAS::IsFailed(inject_host_result))
    {
        DAS_CORE_LOG_WARN(
            "SetTaskComponentHost failed for plugin {}: result={}",
            path_str,
            static_cast<int>(inject_host_result));
    }

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
        feature_info.session_id = load_result.owner_session_id;
        feature_info.plugin_name = Das::Utils::PathToUtf8(identity_path.stem());
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

    // 释锁后回调（FireLifecycleListener）使用的 guid 副本：在下方锁内赋值，
    // 锁外读取 —— 满足 D-87-08「collect 持锁 -> 释 mutex_ -> fire」两阶段
    // 范式，避免 PluginManager::mutex_ 与订阅者内部锁形成 AB-BA。
    DasGuid loaded_guid_for_notify{};

    {
        const std::scoped_lock lock(mutex_);

        if (loaded_plugins_.contains(desc->guid))
        {
            DAS_CORE_LOG_ERROR(
                "Plugin GUID conflict: path={}, existing plugin with same GUID",
                path_str);
            return DAS_E_DUPLICATE_ELEMENT;
        }

        const auto guid = desc->guid;
        // 按值捕获一份供释锁后 FireLifecycleListener 使用（D-87-08：plugin
        // load 通知外部 listener。listener 触发必须晚于本锁作用域，避免
        // PluginManager::mutex_ 与订阅者内部锁形成 AB-BA）。
        loaded_guid_for_notify = guid;
        // 记录 IPC owner_session_id（RuntimeLoadResult.owner_session_id 已在
        // IpcRemotePluginHost.cpp:112/208 设置），用于运行时卸载
        // (UnloadPluginIpc 经 session_id 调 ipc_context_
        // ->UnregisterHostLauncherBySession)。
        plugin.session_id = load_result.owner_session_id;
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

        // 建立 feature 类型本地索引（GetObjectByFeature/GetFeaturesByType
        // 依赖）。此处插件已确定注册成功且 FeatureInfo 地址稳定，须晚于上方
        // 所有可能 loaded_plugins_.erase(guid) 的错误路径，否则 erase 会留下
        // 悬空 FeatureInfo*。
        for (auto& feature : loaded_plugins_[guid].features)
        {
            feature_type_index_[feature.feature_type].push_back(&feature);
        }

        if (pp_out_package && loaded_plugins_[guid].package)
        {
            out_package.Set(loaded_plugins_[guid].package.Get());
        }
    }

    // 锁外触发 lifecycle listener（Loaded）：上面 scoped_lock 作用域已结束，
    // 满足 INV-03 锁层级与订阅者无 AB-BA 约束（参考 Shutdown 两阶段范式）。
    FireLifecycleListener(PluginLifecyclePhase::Loaded, loaded_guid_for_notify);

    out_package.Keep();
    return DAS_S_OK;
}

DasResult PluginManager::UnloadPlugin(const std::filesystem::path& path)
{
    std::unique_lock<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str =
        Das::Utils::PathToUtf8(ResolveManifestIdentityPath(normalized_path));

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

    // 是否经 IPC host 加载（加载来源判断，非 session 语义）。IPC host session
    // 由 AllocateSessionId 分配，恒 ≥2（reserved_session_ids_={0,1,0xFFFF}
    // 在分配时 跳过）；进程内插件携带主进程 session（未 Initialize 为
    // 0、Initialize(1) 为 1）， 均 ≤1。故 session_id>1 等价于「经 IPC host
    // 加载」。注：此处借 session 编号 作加载来源代理，更彻底的做法是在
    // LoadedPlugin 记录加载来源标记。
    if (plug_it->second.session_id > 1)
    {
        // IPC 路径：统一索引清理（feature_type_index_ + sub-manager）必须在
        // move-out 前（plug_it 此刻有效，features 在原址）；之后再 move-out +
        // erase + 释锁，UnloadPluginIpc 只剩锁外停进程。
        CleanupPluginIndexes(guid, plug_it->second);
        LoadedPlugin ipc_plugin = std::move(plug_it->second);
        loaded_plugins_.erase(plug_it);
        path_to_guid_.erase(path_it);
        lock.unlock();
        return UnloadPluginIpc(guid, ipc_plugin);
    }

    // 进程内路径：统一索引清理（feature_type_index_ + sub-manager）再 erase。
    CleanupPluginIndexes(guid, plug_it->second);
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

    const std::scoped_lock lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str =
        Das::Utils::PathToUtf8(ResolveManifestIdentityPath(normalized_path));

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
    const std::scoped_lock lock(mutex_);

    std::vector<std::filesystem::path> paths;
    paths.reserve(loaded_plugins_.size());

    for (const auto& [guid, plugin] : loaded_plugins_)
    {
        paths.push_back(plugin.plugin_path);
    }

    return paths;
}

DasResult PluginManager::GetObjectByFeature(
    Das::PluginInterface::DasPluginFeature feature,
    const DasGuid&                         iid,
    void**                                 pp_out_object)
{
    const std::scoped_lock lock(mutex_);

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
        DasPtr<Das::PluginInterface::IDasInputFactory> factory;
        const auto result = feature_info.interface_ptr->QueryInterface(
            iid,
            reinterpret_cast<void**>(factory.Put()));
        if (DAS::IsFailed(result) || !factory)
        {
            return result;
        }

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
    const std::scoped_lock lock(mutex_);

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
        const std::scoped_lock lock(mutex_);
        auto                   it = loaded_plugins_.find(plugin_guid);
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
    const std::scoped_lock lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str =
        Das::Utils::PathToUtf8(ResolveManifestIdentityPath(normalized_path));

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
    const std::scoped_lock lock(mutex_);
    return path_to_guid_.contains(
        Das::Utils::PathToUtf8(
            ResolveManifestIdentityPath(NormalizePath(path))));
}

size_t PluginManager::GetLoadedPluginCount() const
{
    const std::scoped_lock lock(mutex_);
    return loaded_plugins_.size();
}

DasGuid PluginManager::GetIidForFeature(
    Das::PluginInterface::DasPluginFeature feature) const
{
    switch (feature)
    {
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_CAPTURE_FACTORY:
        return DasIidOf<Das::PluginInterface::IDasCaptureFactory>();
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_ERROR_LENS:
        return DasIidOf<Das::PluginInterface::IDasErrorLens>();
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK:
        return DasIidOf<Das::PluginInterface::IDasTask>();
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY:
        return DasIidOf<Das::PluginInterface::IDasInputFactory>();
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY:
        return DasIidOf<Das::PluginInterface::IDasComponentFactory>();
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY:
        return DasIidOf<
            Das::PluginInterface::IDasTaskAuthoringSessionFactory>();
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY:
        return DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>();
    default:
        return DasGuid{}; // 空 GUID
    }
}

std::string PluginManager::GetFeatureName(
    Das::PluginInterface::DasPluginFeature feature) const
{
    switch (feature)
    {
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_CAPTURE_FACTORY:
        return "CAPTURE_FACTORY";
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_ERROR_LENS:
        return "ERROR_LENS";
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK:
        return "TASK";
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY:
        return "INPUT_FACTORY";
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY:
        return "COMPONENT_FACTORY";
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY:
        return "TASK_AUTHORING_FACTORY";
    case Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY:
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

DasResult PluginManager::UnloadPluginIpc(
    const DasGuid& guid,
    LoadedPlugin&  plugin)
{
    // 索引清理（feature_type_index_ + sub-manager）+ loaded_plugins_ /
    // path_to_guid_ erase 已由唯一调用方 UnloadPlugin 在持锁阶段完成
    // （CleanupPluginIndexes + erase + move-out），并在调用前释 mutex_。本方法
    // 只负责锁外两件事（INV-03：停进程可能阻塞 / 触发重入回调，必须锁外）：
    //   1) fire lifecycle listener（Unloading）—— 订阅者先失效 authoring
    //   cache； 2) 经 ipc_context_ 按 session 主动 Stop Host 进程 + 清
    //   ConnectionManager。
    // plugin 由调用方 move-out 保活，session_id 在释锁后仍可用。fire 在 Stop
    // 之前， 让订阅者在 Host 进程 Stop 阻塞等待退出期间已完成 cache 全清。
    // UnregisterHostLauncherBySession 在 IpcContext override 内组合
    // launcher->Stop()（GOODBYE + 等待进程退出 + ClearCallbacks 全清三 slot）+
    // ConnectionManager::UnregisterHostLauncher（清 hosts_/connections_
    // 索引）。
    FireLifecycleListener(PluginLifecyclePhase::Unloading, guid);

    return ipc_context_.get().UnregisterHostLauncherBySession(
        plugin.session_id);
}

void PluginManager::CleanupPluginIndexes(
    DasGuid             plugin_guid,
    const LoadedPlugin& plugin)
{
    // 调用方已持有 mutex_。从 feature_type_index_ 移除本插件所有 feature 指针
    // （必须在 features 释放前），再通知 3 个内部 sub-manager
    // OnPluginUnloading。 统一三条卸载路径的索引清理，避免 feature_type_index_
    // 残留悬空 FeatureInfo* （曾致 IPC 卸载后 GetFeaturesByType
    // 解引用已释放内存的 UAF）。
    for (const auto& feature : plugin.features)
    {
        const auto type_it = feature_type_index_.find(feature.feature_type);
        if (type_it != feature_type_index_.end())
        {
            auto& vec = type_it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), &feature), vec.end());
        }
    }
    error_lens_mgr_.OnPluginUnloading(plugin_guid);
    component_factory_mgr_.OnPluginUnloading(plugin_guid);
    task_component_factory_mgr_.OnPluginUnloading(plugin_guid);
}

void PluginManager::CleanupPluginByGuid(DasGuid plugin_guid)
{
    // 调用方已持有 mutex_

    auto plugin_it = loaded_plugins_.find(plugin_guid);
    if (plugin_it != loaded_plugins_.end())
    {
        // 统一索引清理（feature_type_index_ + sub-manager）再 erase。
        CleanupPluginIndexes(plugin_guid, plugin_it->second);
        path_to_guid_.erase(
            Das::Utils::PathToUtf8(plugin_it->second.plugin_path));
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

    {
        const std::scoped_lock lock(mutex_);
        CleanupPluginByGuid(plugin_guid);
    }
    // 锁外触发 lifecycle listener（Unloading）：CleanupPluginByGuid 调用方
    // 持锁契约不变，fire 由本调用方在释锁后负责（D-87-08 两阶段范式）。
    FireLifecycleListener(PluginLifecyclePhase::Unloading, plugin_guid);
}

void PluginManager::OnHeartbeatTimeout(DasGuid plugin_guid)
{
    // 此回调在 ConnectionManager::heartbeat_thread_ 上执行（非 io_context
    // 线程）， 锁序：callback_mutex_（HostLauncher）-> mutex_（INV-03）。
    // heartbeat_thread_ 由 ConnectionManager 持有并在析构时 join。
    DAS_CORE_LOG_WARN("Heartbeat timeout for plugin, cleaning up index");

    {
        const std::scoped_lock lock(mutex_);
        CleanupPluginByGuid(plugin_guid);
    }
    // 锁外触发 lifecycle listener（Unloading）：与 OnHostProcessExit 对称。
    FireLifecycleListener(PluginLifecyclePhase::Unloading, plugin_guid);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
