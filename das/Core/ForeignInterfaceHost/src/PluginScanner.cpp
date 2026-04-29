#include <das/Core/ForeignInterfaceHost/PluginScanner.h>

#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/DasJsonCore.h>

#include <fstream>
#include <iterator>

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
    auto            dir_iter = std::filesystem::directory_iterator(
        plugin_dir,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    if (ec)
    {
        DAS_CORE_LOG_WARN(
            "Failed to iterate plugin directory {}: {}",
            plugin_dir.string(),
            ec.message());
        return result;
    }

    for (const auto& entry : dir_iter)
    {
        if (entry.is_directory())
        {
            auto dirname = entry.path().filename().string();

            // Skip temporary/transient states from concurrent installs
            if (dirname.ends_with(".installing")
                || dirname.ends_with(".willBeDelete")
                || dirname.starts_with(".tmp_install_"))
            {
                continue;
            }

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
                std::ifstream ifs(manifest_path);
                std::string   content(
                    (std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
                auto parsed = Das::Utils::ParseYyjsonFromString(
                    content,
                    yyjson::ReadFlag::AllowComments
                        | yyjson::ReadFlag::AllowTrailingCommas);
                if (!parsed)
                {
                    DAS_CORE_LOG_WARN(
                        "Failed to parse manifest {}",
                        manifest_path.string());
                    continue;
                }
                PluginPackageDesc desc;
                ParsePluginPackageDescFromJson(*parsed, desc);
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
        else
        {
            // Flat-file mode: manifest .json at plugin_dir root
            auto path = entry.path();
            if (path.extension() != ".json")
            {
                continue;
            }

            auto stem = path.stem().string();

            // Check for flat-file deletion marker
            auto marker = plugin_dir / (stem + ".willBeDelete");
            if (std::filesystem::exists(marker))
            {
                continue;
            }

            try
            {
                std::ifstream ifs(path);
                std::string   content(
                    (std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
                auto parsed = Das::Utils::ParseYyjsonFromString(
                    content,
                    yyjson::ReadFlag::AllowComments
                        | yyjson::ReadFlag::AllowTrailingCommas);
                if (!parsed)
                {
                    continue;
                }
                PluginPackageDesc desc;
                ParsePluginPackageDescFromJson(*parsed, desc);

                // Verify companion plugin binary exists
                auto plugin_file =
                    plugin_dir
                    / (desc.name + "." + desc.plugin_filename_extension);
                if (!std::filesystem::exists(plugin_file))
                {
                    continue;
                }

                result.push_back(std::move(desc));
            }
            catch (const std::exception&)
            {
                // Not a valid manifest — skip silently
            }
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
        if (entry.is_directory())
        {
            // Directory-mode: look for .willBeDelete inside subdirectory
            for (const auto& sub : std::filesystem::directory_iterator(
                     entry.path(),
                     std::filesystem::directory_options::skip_permission_denied,
                     ec))
            {
                if (sub.path().extension() == ".willBeDelete")
                {
                    auto plugin_name = sub.path().stem().string();
                    auto parent_name = entry.path().filename().string();
                    if (plugin_name != parent_name)
                    {
                        continue;
                    }

                    DAS_CORE_LOG_INFO(
                        "Cleaning up marked plugin: {}",
                        plugin_name);

                    try
                    {
                        auto plugin_path = plugin_dir / plugin_name;
                        std::filesystem::remove_all(plugin_path);
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
        else
        {
            // Flat-file mode: .willBeDelete marker at plugin_dir root
            if (entry.path().extension() != ".willBeDelete")
            {
                continue;
            }

            auto plugin_name = entry.path().stem().string();
            DAS_CORE_LOG_INFO(
                "Cleaning up marked flat-file plugin: {}",
                plugin_name);

            try
            {
                // Delete manifest and plugin binary by scanning companion
                // files matching the plugin name
                auto manifest = plugin_dir / (plugin_name + ".json");
                if (std::filesystem::exists(manifest))
                {
                    std::ifstream ifs(manifest);
                    std::string   content(
                        (std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
                    auto parsed = Das::Utils::ParseYyjsonFromString(
                        content,
                        yyjson::ReadFlag::AllowComments
                            | yyjson::ReadFlag::AllowTrailingCommas);
                    if (parsed)
                    {
                        PluginPackageDesc desc;
                        ParsePluginPackageDescFromJson(*parsed, desc);

                        auto plugin_file = plugin_dir
                                           / (desc.name + "."
                                              + desc.plugin_filename_extension);
                        std::filesystem::remove(plugin_file, ec);
                    }
                }

                std::filesystem::remove(manifest, ec);
                std::filesystem::remove(entry.path(), ec);
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to clean up flat-file plugin {}: {}",
                    plugin_name,
                    e.what());
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
            // Determine plugin mode: directory or flat-file
            auto                  plugin_subdir = plugin_dir / desc.name;
            std::filesystem::path marker_path;
            if (std::filesystem::exists(plugin_subdir)
                && std::filesystem::is_directory(plugin_subdir))
            {
                marker_path = plugin_subdir / (desc.name + ".willBeDelete");
            }
            else
            {
                marker_path = plugin_dir / (desc.name + ".willBeDelete");
            }

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

yyjson::writer::detail::value PluginPackageDescToJson(
    const PluginPackageDesc& desc)
{
    yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
    j[std::string_view("name")] = desc.name;
    j[std::string_view("description")] = desc.description;
    j[std::string_view("author")] = desc.author;
    j[std::string_view("version")] = desc.version;
    j[std::string_view("guid")] = desc.guid;
    j[std::string_view("supportedSystem")] = desc.supported_system;
    j[std::string_view("language")] = static_cast<std::int64_t>(desc.language);
    j[std::string_view("pluginFilenameExtension")] =
        desc.plugin_filename_extension;

    if (desc.opt_resource_path.has_value())
    {
        j[std::string_view("resourcePath")] = desc.opt_resource_path.value();
    }

    return j;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
