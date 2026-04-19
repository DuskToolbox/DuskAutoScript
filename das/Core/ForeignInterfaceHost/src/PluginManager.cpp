#include <das/Core/ForeignInterfaceHost/PluginManager.h>

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

    // 解析 manifest JSON 获取 PluginPackageDesc（含 guid）
    auto desc = std::make_shared<PluginPackageDesc>();
    // 尝试读取 <plugin_path>/manifest.json
    auto manifest_path = normalized_path / "manifest.json";
    if (std::filesystem::exists(manifest_path))
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

    // GUID 冲突检测
    if (loaded_plugins_.contains(desc->guid))
    {
        DAS_CORE_LOG_ERROR(
            "Plugin GUID conflict: path={}, existing plugin with same GUID",
            path_str);
        return DAS_E_DUPLICATE_ELEMENT;
    }

    plugin.desc = desc;

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
        feature_info.feature_type = feature;
        feature_info.iid = GetIidForFeature(feature);
        feature_info.session_id = session_id_;
        feature_info.plugin_name = normalized_path.stem().string();

        // 创建 Feature 接口
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

    // 先保存 guid，避免 move 后访问问题
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
            component_factory_mgr_.OnPluginLoaded(guid, plugin_factories);
        }
    }

    return DAS_S_OK;
}

DasResult PluginManager::UnloadPlugin(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(mutex_);

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

    // 先从 feature_type_index_ 中移除指针（在 erase loaded_plugins_ 之前）
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

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
