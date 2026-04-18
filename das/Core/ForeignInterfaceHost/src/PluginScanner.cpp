#include <das/Core/ForeignInterfaceHost/PluginScanner.h>

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>

#include <fstream>
#include <nlohmann/json.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

std::filesystem::path FindManifest(
    const std::filesystem::path& plugin_dir_entry)
{
    auto dirname = plugin_dir_entry.filename().string();

    auto primary = plugin_dir_entry / (dirname + ".json");
    if (std::filesystem::exists(primary))
    {
        return primary;
    }

    auto fallback = plugin_dir_entry / "manifest.json";
    if (std::filesystem::exists(fallback))
    {
        return fallback;
    }

    return {};
}

std::vector<PluginPackageDesc> ScanPlugins(
    const std::filesystem::path& plugin_dir)
{
    std::vector<PluginPackageDesc> result;

    if (!std::filesystem::exists(plugin_dir))
    {
        return result;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             plugin_dir,
             std::filesystem::directory_options::skip_permission_denied,
             ec))
    {
        if (!entry.is_directory())
        {
            continue;
        }

        auto dirname = entry.path().filename().string();

        auto marker = entry.path() / (dirname + ".willBeDelete");
        if (std::filesystem::exists(marker))
        {
            continue;
        }

        auto manifest_path = FindManifest(entry.path());
        if (manifest_path.empty())
        {
            continue;
        }

        try
        {
            std::ifstream     ifs(manifest_path);
            auto              json_data = nlohmann::json::parse(ifs);
            PluginPackageDesc desc;
            from_json(json_data, desc);
            result.push_back(std::move(desc));
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_WARN(
                "Failed to parse manifest {}: {}",
                manifest_path.string(),
                e.what());
        }
    }

    return result;
}

void CleanupMarkedPlugins(const std::filesystem::path& plugin_dir)
{
    if (!std::filesystem::exists(plugin_dir))
    {
        return;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             plugin_dir,
             std::filesystem::directory_options::skip_permission_denied,
             ec))
    {
        if (!entry.is_directory())
        {
            continue;
        }

        for (const auto& sub : std::filesystem::directory_iterator(
                 entry.path(),
                 std::filesystem::directory_options::skip_permission_denied,
                 ec))
        {
            if (sub.path().extension() == ".willBeDelete")
            {
                auto plugin_name = sub.path().stem().string();
                DAS_CORE_LOG_INFO("Cleaning up marked plugin: {}", plugin_name);

                try
                {
                    auto plugin_path = plugin_dir / plugin_name;
                    std::filesystem::remove_all(plugin_path);
                    std::filesystem::remove(sub.path());
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_WARN(
                        "Failed to clean up plugin {}: {}",
                        plugin_name,
                        e.what());
                }
            }
        }
    }
}

DasResult MarkForDeletion(
    const std::filesystem::path& plugin_dir,
    const DasGuid&               guid)
{
    auto plugins = ScanPlugins(plugin_dir);

    for (const auto& desc : plugins)
    {
        if (desc.guid == guid)
        {
            auto marker_path =
                plugin_dir / desc.name / (desc.name + ".willBeDelete");

            std::ofstream ofs(marker_path);
            if (!ofs)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to create deletion marker: {}",
                    marker_path.string());
                return DAS_E_FAIL;
            }

            return DAS_S_OK;
        }
    }

    return DAS_E_NOT_FOUND;
}

nlohmann::json PluginPackageDescToJson(const PluginPackageDesc& desc)
{
    nlohmann::json j;
    j["name"] = desc.name;
    j["description"] = desc.description;
    j["author"] = desc.author;
    j["version"] = desc.version;
    j["guid"] = desc.guid;
    j["supportedSystem"] = desc.supported_system;
    j["language"] = desc.language;
    j["pluginFilenameExtension"] = desc.plugin_filename_extension;

    if (desc.opt_resource_path.has_value())
    {
        j["resourcePath"] = desc.opt_resource_path.value();
    }

    return j;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
