#include <das/Core/ForeignInterfaceHost/PluginResourceIndex.h>

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/DasJsonCore.h>

#include <fstream>
#include <iterator>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

PluginResourceIndex& PluginResourceIndex::GetInstance()
{
    static PluginResourceIndex instance;
    return instance;
}

void PluginResourceIndex::ConfigurePluginResourceScanRoot(
    std::filesystem::path scan_root)
{
    std::unique_lock lock(mutex_);

    if (scan_root_configured_ && scan_root_ == scan_root)
    {
        DAS_CORE_LOG_INFO(
            "PluginResourceIndex: scan root unchanged: {}",
            scan_root.string());
        return;
    }

    scan_root_ = std::move(scan_root);
    scan_root_configured_ = true;
    cache_stale_ = true;

    DAS_CORE_LOG_INFO(
        "PluginResourceIndex: scan root configured: {}",
        scan_root_.string());
}

DasResult PluginResourceIndex::ResolvePluginResourceEntryByGuid(
    const DasGuid&              lookup_guid,
    const PluginResourceEntry** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    {
        std::shared_lock lock(mutex_);
        if (!cache_stale_)
        {
            auto it = entries_.find(lookup_guid);
            if (it != entries_.end())
            {
                *pp_out = &(it->second);
                return DAS_S_OK;
            }
        }
        if (!scan_root_configured_)
        {
            DAS_CORE_LOG_WARN(
                "PluginResourceIndex: scan root not configured, "
                "cannot resolve GUID");
            return DAS_E_NOT_FOUND;
        }
    }

    auto result = ScanAndPublish();
    if (IsFailed(result))
    {
        return result;
    }

    std::shared_lock lock(mutex_);
    auto             it = entries_.find(lookup_guid);
    if (it != entries_.end())
    {
        *pp_out = &(it->second);
        return DAS_S_OK;
    }

    DAS_CORE_LOG_WARN("PluginResourceIndex: GUID not found after scan");
    return DAS_E_NOT_FOUND;
}

DasResult PluginResourceIndex::ResolveResourceFullPath(
    const std::filesystem::path& resource_root,
    const std::string&           relative_path,
    std::filesystem::path&       full_path)
{
    if (relative_path.empty())
    {
        DAS_CORE_LOG_WARN("PluginResourceIndex: relative path is empty");
        return DAS_E_INVALID_PATH;
    }

    std::filesystem::path rp(relative_path);

    if (rp.is_absolute())
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: relative path must not be absolute: {}",
            relative_path);
        return DAS_E_INVALID_PATH;
    }

    if (relative_path.find("..") != std::string::npos)
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: relative path must not contain '..': {}",
            relative_path);
        return DAS_E_INVALID_PATH;
    }

    std::error_code ec;
    auto canonical_root = std::filesystem::weakly_canonical(resource_root, ec);
    if (ec)
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: cannot canonicalize resource_root: {}",
            ec.message());
        return DAS_E_INVALID_PATH;
    }

    auto resolved = canonical_root / rp;
    auto canonical_resolved = std::filesystem::weakly_canonical(resolved, ec);
    if (ec)
    {
        ec.clear();
        canonical_resolved = resolved;
    }

    auto relative = canonical_resolved.lexically_relative(canonical_root);
    auto rel_str = relative.string();
    if (rel_str.empty())
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: relative path resolves outside resource root: "
            "{}",
            relative_path);
        return DAS_E_INVALID_PATH;
    }

    if (rel_str.size() >= 2 && rel_str.substr(0, 2) == "..")
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: relative path escapes resource root: {}",
            relative_path);
        return DAS_E_INVALID_PATH;
    }

    full_path = std::move(canonical_resolved);
    return DAS_S_OK;
}

void PluginResourceIndex::InvalidateCache()
{
    std::unique_lock lock(mutex_);
    cache_stale_ = true;
    DAS_CORE_LOG_INFO("PluginResourceIndex: cache invalidated");
}

DasResult PluginResourceIndex::ScanAndPublish()
{
    std::filesystem::path scan_root;
    {
        std::shared_lock lock(mutex_);
        scan_root = scan_root_;
    }

    DAS_CORE_LOG_INFO(
        "PluginResourceIndex: starting full scan of {}",
        scan_root.string());

    if (!std::filesystem::exists(scan_root))
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: scan root does not exist: {}",
            scan_root.string());
        return DAS_E_NOT_FOUND;
    }

    std::unordered_map<DasGuid, PluginResourceEntry> temp_entries;

    std::error_code ec;
    auto            dir_iter = std::filesystem::directory_iterator(
        scan_root,
        std::filesystem::directory_options::skip_permission_denied,
        ec);

    if (ec)
    {
        DAS_CORE_LOG_ERROR(
            "PluginResourceIndex: failed to iterate scan root: {}",
            ec.message());
        return DAS_E_FAIL;
    }

    for (const auto& entry : dir_iter)
    {
        if (!entry.is_directory())
        {
            continue;
        }

        auto manifest_path = FindManifest(entry.path());
        if (manifest_path.empty())
        {
            continue;
        }

        PluginPackageDesc desc;
        try
        {
            std::ifstream ifs(manifest_path);
            std::string   content(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());
            auto parsed = Das::Utils::ParseYyjsonFromString(content);
            if (parsed)
            {
                const auto& const_val = *parsed;
                auto        obj = const_val.as_object();
                if (obj)
                {
                    ParsePluginPackageDescFromJson(*obj, desc);
                }
            }
            else
            {
                DAS_CORE_LOG_WARN(
                    "PluginResourceIndex: failed to parse manifest {}",
                    manifest_path.string());
                continue;
            }
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_WARN(
                "PluginResourceIndex: failed to parse manifest {}: {}",
                manifest_path.string(),
                e.what());
            continue;
        }

        auto package_root = manifest_path.parent_path();

        std::string resource_path_str;
        if (desc.opt_resource_path.has_value())
        {
            resource_path_str = desc.opt_resource_path.value();
        }
        else
        {
            resource_path_str = "resource";
        }

        std::filesystem::path resource_root;
        auto                  result = ValidateResourcePath(
            package_root,
            resource_path_str,
            resource_root);
        if (IsFailed(result))
        {
            DAS_CORE_LOG_WARN(
                "PluginResourceIndex: invalid resourcePath '{}' in plugin "
                "'{}': skipping",
                resource_path_str,
                desc.name);
            continue;
        }

        PluginResourceEntry resource_entry;
        resource_entry.lookup_guid = desc.guid;
        resource_entry.plugin_guid = desc.guid;
        resource_entry.manifest_path = manifest_path;
        resource_entry.package_root = package_root;
        resource_entry.resource_root = resource_root;
        resource_entry.plugin_name = desc.name;

        auto [it, inserted] =
            temp_entries.emplace(desc.guid, std::move(resource_entry));
        if (!inserted)
        {
            DAS_CORE_LOG_ERROR(
                "PluginResourceIndex: GUID conflict detected: GUID is "
                "claimed by both '{}' and '{}'. Scan aborted, no results "
                "published.",
                it->second.plugin_name,
                desc.name);
            return DAS_E_DUPLICATE_ELEMENT;
        }

        DAS_CORE_LOG_INFO(
            "PluginResourceIndex: indexed plugin '{}' with resource_root={}",
            desc.name,
            resource_root.string());
    }

    size_t published_count = 0;
    {
        std::unique_lock lock(mutex_);
        published_count = temp_entries.size();
        entries_ = std::move(temp_entries);
        cache_stale_ = false;
    }

    DAS_CORE_LOG_INFO(
        "PluginResourceIndex: scan complete, {} entries published",
        published_count);

    return DAS_S_OK;
}

DasResult PluginResourceIndex::ValidateResourcePath(
    const std::filesystem::path& package_root,
    const std::string&           resource_path,
    std::filesystem::path&       resource_root)
{
    if (resource_path.empty())
    {
        DAS_CORE_LOG_WARN("PluginResourceIndex: resourcePath is empty");
        return DAS_E_INVALID_PATH;
    }

    std::filesystem::path rp(resource_path);

    if (rp.is_absolute())
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: resourcePath must be relative, got: {}",
            resource_path);
        return DAS_E_INVALID_PATH;
    }

    std::string rp_str = rp.string();
    if (rp_str.find("..") != std::string::npos)
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: resourcePath must not contain '..': {}",
            resource_path);
        return DAS_E_INVALID_PATH;
    }

    std::error_code ec;
    auto canonical_root = std::filesystem::weakly_canonical(package_root, ec);
    if (ec)
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: cannot canonicalize package_root: {}",
            ec.message());
        return DAS_E_INVALID_PATH;
    }

    auto full_resource_path = canonical_root / rp;
    auto canonical_resource =
        std::filesystem::weakly_canonical(full_resource_path, ec);
    if (ec)
    {
        ec.clear();
        canonical_resource = full_resource_path;
    }

    auto relative = canonical_resource.lexically_relative(canonical_root);
    auto rel_str = relative.string();
    if (rel_str.empty())
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: resourcePath resolves outside package root: "
            "{}",
            resource_path);
        return DAS_E_INVALID_PATH;
    }

    if (rel_str.size() >= 2 && rel_str.substr(0, 2) == "..")
    {
        DAS_CORE_LOG_WARN(
            "PluginResourceIndex: resourcePath escapes package root: {}",
            resource_path);
        return DAS_E_INVALID_PATH;
    }

    resource_root = std::move(canonical_resource);
    return DAS_S_OK;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
