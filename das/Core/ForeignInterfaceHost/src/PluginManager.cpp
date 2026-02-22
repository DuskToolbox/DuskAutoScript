#include <das/Core/ForeignInterfaceHost/PluginManager.h>

#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/Core/Logger/Logger.h>

#include <algorithm>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

PluginManager& PluginManager::GetInstance()
{
    static PluginManager instance;
    return instance;
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

    // 卸载所有插件
    for (auto& [path, plugin] : loaded_plugins_)
    {
        // 先注销对象
        for (auto& feature : plugin.features)
        {
            Core::IPC::RemoteObjectRegistry::GetInstance().UnregisterObject(
                feature.object_id);
        }
    }

    feature_map_.clear();
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

DasResult PluginManager::LoadPlugin(
    const std::filesystem::path&              path,
    Das::PluginInterface::IDasPluginPackage** pp_out_package)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!runtime_)
    {
        DAS_CORE_LOG_ERROR("No runtime set for loading plugins");
        return DAS_E_OBJECT_NOT_INIT;
    }

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    if (loaded_plugins_.contains(path_str))
    {
        DAS_CORE_LOG_WARN("Plugin already loaded: {}", path_str);
        if (pp_out_package && loaded_plugins_[path_str].package)
        {
            *pp_out_package = loaded_plugins_[path_str].package.Get();
            (*pp_out_package)->AddRef();
        }
        return DAS_S_FALSE;
    }

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

    // 枚举插件的 Features 并创建接口
    uint64_t index = 0;
    while (true)
    {
        Das::PluginInterface::DasPluginFeature feature{};
        auto enum_result = plugin.package->EnumFeature(index, &feature);
        if (enum_result != DAS_S_OK)
        {
            break; // 枚举结束
        }

        FeatureInfo feature_info;
        feature_info.feature_name = GetFeatureName(feature);
        feature_info.iid = GetIidForFeature(feature);
        feature_info.session_id = session_id_;
        feature_info.plugin_name = normalized_path.stem().string();

        // 创建 Feature 接口
        void* p_interface = nullptr;
        auto  create_result =
            plugin.package->CreateFeatureInterface(index, &p_interface);
        if (create_result == DAS_S_OK && p_interface)
        {
            feature_info.interface_ptr =
                DasPtr<IDasBase>(static_cast<IDasBase*>(p_interface));
        }

        plugin.features.push_back(std::move(feature_info));
        ++index;
    }

    loaded_plugins_[path_str] = std::move(plugin);

    DAS_CORE_LOG_INFO(
        "Loaded plugin: {} with {} features",
        path_str,
        loaded_plugins_[path_str].features.size());

    if (pp_out_package && loaded_plugins_[path_str].package)
    {
        *pp_out_package = loaded_plugins_[path_str].package.Get();
        (*pp_out_package)->AddRef();
    }

    return DAS_S_OK;
}

DasResult PluginManager::UnloadPlugin(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    auto it = loaded_plugins_.find(path_str);
    if (it == loaded_plugins_.end())
    {
        DAS_CORE_LOG_WARN("Plugin not loaded: {}", path_str);
        return DAS_E_NOT_FOUND;
    }

    bool can_unload = false;
    if (it->second.package)
    {
        auto unload_result = it->second.package->CanUnloadNow(&can_unload);
        if (unload_result != DAS_S_OK || !can_unload)
        {
            DAS_CORE_LOG_WARN("Plugin cannot be unloaded now: {}", path_str);
            return DAS_E_FAIL;
        }
    }

    // 注销对象
    for (auto& feature : it->second.features)
    {
        if (!Core::IPC::IsNullObjectId(feature.object_id))
        {
            Core::IPC::RemoteObjectRegistry::GetInstance().UnregisterObject(
                feature.object_id);
        }
        // 从 feature_map_ 中移除
        feature_map_.erase(feature.feature_name);
    }

    loaded_plugins_.erase(it);

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

    auto it = loaded_plugins_.find(path_str);
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

    for (const auto& [path, plugin] : loaded_plugins_)
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

    auto it = loaded_plugins_.find(path_str);
    if (it == loaded_plugins_.end())
    {
        DAS_CORE_LOG_WARN("Plugin not loaded: {}", path_str);
        return DAS_E_NOT_FOUND;
    }

    auto&    registry = Core::IPC::RemoteObjectRegistry::GetInstance();
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
            feature.feature_name,
            1);

        if (register_result == DAS_S_OK)
        {
            feature.object_id = obj_id;
            feature_map_[feature.feature_name] = &feature;

            DAS_CORE_LOG_INFO("Registered feature {}", feature.feature_name);
        }
        else
        {
            DAS_CORE_LOG_WARN(
                "Failed to register feature {}: error={}",
                feature.feature_name,
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

    auto it = loaded_plugins_.find(path_str);
    if (it == loaded_plugins_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    auto& registry = Core::IPC::RemoteObjectRegistry::GetInstance();

    for (auto& feature : it->second.features)
    {
        if (!Core::IPC::IsNullObjectId(feature.object_id))
        {
            registry.UnregisterObject(feature.object_id);
            feature.object_id = Core::IPC::ObjectId{};
        }
        feature_map_.erase(feature.feature_name);
    }

    return DAS_S_OK;
}

DasResult PluginManager::GetObjectByFeature(
    const std::string& feature_name,
    const DasGuid&     iid,
    void**             pp_out_object)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!pp_out_object)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_object = nullptr;

    auto it = feature_map_.find(feature_name);
    if (it == feature_map_.end() || !it->second)
    {
        DAS_CORE_LOG_WARN("Feature not found: {}", feature_name);
        return DAS_E_NOT_FOUND;
    }

    auto& feature = *it->second;
    if (!feature.interface_ptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // QueryInterface 获取请求的接口
    auto result = feature.interface_ptr->QueryInterface(iid, pp_out_object);
    return result;
}

DasResult PluginManager::GetObjectByFeature(
    Das::PluginInterface::DasPluginFeature feature,
    const DasGuid&                         iid,
    void**                                 pp_out_object)
{
    return GetObjectByFeature(GetFeatureName(feature), iid, pp_out_object);
}

void PluginManager::GetAllFeatures(std::vector<std::string>& out_features) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    out_features.clear();
    out_features.reserve(feature_map_.size());

    for (const auto& [name, info] : feature_map_)
    {
        out_features.push_back(name);
    }
}

DasResult PluginManager::GetPluginFeatures(
    const std::filesystem::path& path,
    std::vector<FeatureInfo>&    out_features) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto normalized_path = NormalizePath(path);
    auto path_str = normalized_path.string();

    auto it = loaded_plugins_.find(path_str);
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
    return loaded_plugins_.contains(NormalizePath(path).string());
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

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
