#include <das/Core/ForeignInterfaceHost/PluginScanner.h>

#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasConfig.h>
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
                const auto& const_val = *parsed;
                auto        obj = const_val.as_object();
                if (!obj)
                {
                    DAS_CORE_LOG_WARN(
                        "Failed to parse manifest {}: not an object",
                        manifest_path.string());
                    continue;
                }
                PluginPackageDesc desc;
                ParsePluginPackageDescFromJson(*obj, desc);
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
                const auto& const_val = *parsed;
                auto        obj = const_val.as_object();
                if (!obj)
                {
                    continue;
                }
                PluginPackageDesc desc;
                ParsePluginPackageDescFromJson(*obj, desc);

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
                        const auto&       const_val = *parsed;
                        auto              obj = const_val.as_object();
                        if (obj)
                        {
                            ParsePluginPackageDescFromJson(*obj, desc);
                        }

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

yyjson::value PluginPackageDescToJson(const PluginPackageDesc& desc)
{
    auto j = Das::Utils::MakeYyjsonObject();
    auto obj = j.as_object();
    if (!obj)
    {
        return j;
    }
    (*obj)[std::string_view("name")] =
        std::make_pair(std::string_view(desc.name), yyjson::copy_string);
    (*obj)[std::string_view("description")] =
        std::make_pair(std::string_view(desc.description), yyjson::copy_string);
    (*obj)[std::string_view("author")] =
        std::make_pair(std::string_view(desc.author), yyjson::copy_string);
    (*obj)[std::string_view("version")] =
        std::make_pair(std::string_view(desc.version), yyjson::copy_string);
    (*obj)[std::string_view("guid")] =
        Das::Core::ForeignInterfaceHost::DasGuidToStdString(desc.guid);
    (*obj)[std::string_view("supportedSystem")] = std::make_pair(
        std::string_view(desc.supported_system),
        yyjson::copy_string);
    (*obj)[std::string_view("language")] =
        static_cast<std::int64_t>(desc.language);
    (*obj)[std::string_view("pluginFilenameExtension")] = std::make_pair(
        std::string_view(desc.plugin_filename_extension),
        yyjson::copy_string);

    if (desc.opt_resource_path.has_value())
    {
        (*obj)[std::string_view("resourcePath")] = std::make_pair(
            std::string_view(desc.opt_resource_path.value()),
            yyjson::copy_string);
    }

    return j;
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

yyjson::value PluginSettingDescToJson(const PluginSettingDesc& setting)
{
    auto j = Das::Utils::MakeYyjsonObject();
    auto obj = j.as_object();
    if (!obj)
    {
        return j;
    }
    (*obj)[std::string_view("name")] =
        std::make_pair(std::string_view(setting.name), yyjson::copy_string);
    (*obj)[std::string_view("type")] = static_cast<std::int64_t>(setting.type);
    (*obj)[std::string_view("required")] = setting.required;

    if (setting.description.has_value())
    {
        (*obj)[std::string_view("description")] = std::make_pair(
            std::string_view(setting.description.value()),
            yyjson::copy_string);
    }

    if (setting.deprecation_message.has_value())
    {
        (*obj)[std::string_view("deprecationMessage")] = std::make_pair(
            std::string_view(setting.deprecation_message.value()),
            yyjson::copy_string);
    }

    if (setting.enum_values.has_value())
    {
        auto arr = Das::Utils::MakeYyjsonArray();
        auto arr_ref = arr.as_array();
        if (arr_ref)
        {
            for (const auto& v : setting.enum_values.value())
            {
                arr_ref->emplace_back(std::string(v));
            }
        }
        (*obj)[std::string_view("enumValues")] = std::move(arr);
    }

    if (setting.enum_descriptions.has_value())
    {
        auto arr = Das::Utils::MakeYyjsonArray();
        auto arr_ref = arr.as_array();
        if (arr_ref)
        {
            for (const auto& v : setting.enum_descriptions.value())
            {
                arr_ref->emplace_back(std::string(v));
            }
        }
        (*obj)[std::string_view("enumDescriptions")] = std::move(arr);
    }

    std::visit(
        [&obj](const auto& val)
        {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                (*obj)[std::string_view("defaultValue")] =
                    yyjson::value(nullptr);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                (*obj)[std::string_view("defaultValue")] = val;
            }
            else if constexpr (std::is_same_v<T, std::int64_t>)
            {
                (*obj)[std::string_view("defaultValue")] = val;
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                (*obj)[std::string_view("defaultValue")] =
                    static_cast<double>(val);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                (*obj)[std::string_view("defaultValue")] =
                    std::make_pair(std::string_view(val), yyjson::copy_string);
            }
        },
        setting.default_value);

    return j;
}

DAS_NS_ANONYMOUS_DETAILS_END

yyjson::value PluginPackageDescDetailToJson(const PluginPackageDesc& desc)
{
    auto j = PluginPackageDescToJson(desc);
    auto obj = j.as_object();
    if (!obj)
    {
        return j;
    }

    (*obj)[std::string_view("loadMode")] =
        static_cast<std::int64_t>(desc.load_mode);

    if (!desc.settings_desc.empty())
    {
        auto arr = Das::Utils::MakeYyjsonArray();
        auto arr_ref = arr.as_array();
        if (arr_ref)
        {
            for (const auto& s : desc.settings_desc)
            {
                arr_ref->emplace_back(Details::PluginSettingDescToJson(s));
            }
        }
        (*obj)[std::string_view("settingsDesc")] = std::move(arr);
    }

    if (!desc.settings_groups.empty())
    {
        auto settings_obj = Das::Utils::MakeYyjsonObject();
        auto settings_ref = settings_obj.as_object();
        if (settings_ref)
        {
            for (const auto& [guid, group] : desc.settings_groups)
            {
                auto group_obj = Das::Utils::MakeYyjsonObject();
                auto group_ref = group_obj.as_object();
                if (!group_ref)
                {
                    continue;
                }
                (*group_ref)[std::string_view("name")] = std::make_pair(
                    std::string_view(group.name),
                    yyjson::copy_string);
                (*group_ref)[std::string_view("description")] = std::make_pair(
                    std::string_view(group.description),
                    yyjson::copy_string);

                if (!group.descriptors.empty())
                {
                    auto d_arr = Das::Utils::MakeYyjsonArray();
                    auto d_arr_ref = d_arr.as_array();
                    if (d_arr_ref)
                    {
                        for (const auto& d : group.descriptors)
                        {
                            d_arr_ref->emplace_back(
                                Details::PluginSettingDescToJson(d));
                        }
                    }
                    (*group_ref)[std::string_view("descriptors")] =
                        std::move(d_arr);
                }

                auto guid_str = DasGuidToStdString(guid);
                (*settings_ref)[std::string_view(guid_str)] =
                    std::move(group_obj);
            }
        }
        (*obj)[std::string_view("settings")] = std::move(settings_obj);
    }

    if (!desc.task_descriptors.empty())
    {
        auto tasks_obj = Das::Utils::MakeYyjsonObject();
        auto tasks_ref = tasks_obj.as_object();
        if (tasks_ref)
        {
            for (const auto& [task_guid, task] : desc.task_descriptors)
            {
                auto task_obj = Das::Utils::MakeYyjsonObject();
                auto task_ref = task_obj.as_object();
                if (!task_ref)
                {
                    continue;
                }
                (*task_ref)[std::string_view("pluginGuid")] =
                    DasGuidToStdString(task.plugin_guid);
                (*task_ref)[std::string_view("name")] = std::make_pair(
                    std::string_view(task.name),
                    yyjson::copy_string);
                (*task_ref)[std::string_view("description")] = std::make_pair(
                    std::string_view(task.description),
                    yyjson::copy_string);

                if (task.game_name.has_value())
                {
                    (*task_ref)[std::string_view("gameName")] = std::make_pair(
                        std::string_view(task.game_name.value()),
                        yyjson::copy_string);
                }

                if (!task.descriptors.empty())
                {
                    auto d_arr = Das::Utils::MakeYyjsonArray();
                    auto d_arr_ref = d_arr.as_array();
                    if (d_arr_ref)
                    {
                        for (const auto& d : task.descriptors)
                        {
                            d_arr_ref->emplace_back(
                                Details::PluginSettingDescToJson(d));
                        }
                    }
                    (*task_ref)[std::string_view("descriptors")] =
                        std::move(d_arr);
                }

                if (task.authoring.has_value())
                {
                    auto auth_obj = Das::Utils::MakeYyjsonObject();
                    auto auth_ref = auth_obj.as_object();
                    if (auth_ref)
                    {
                        (*auth_ref)[std::string_view("factoryGuid")] =
                            DasGuidToStdString(task.authoring->factory_guid);
                        if (!task.authoring->supported_kinds.empty())
                        {
                            auto k_arr = Das::Utils::MakeYyjsonArray();
                            auto k_arr_ref = k_arr.as_array();
                            if (k_arr_ref)
                            {
                                for (const auto& kind :
                                     task.authoring->supported_kinds)
                                {
                                    k_arr_ref->emplace_back(std::string(kind));
                                }
                            }
                            (*auth_ref)[std::string_view("supportedKinds")] =
                                std::move(k_arr);
                        }
                    }
                    (*task_ref)[std::string_view("authoring")] =
                        std::move(auth_obj);
                }

                if (task.execution_component.has_value())
                {
                    auto exec_obj = Das::Utils::MakeYyjsonObject();
                    auto exec_ref = exec_obj.as_object();
                    if (exec_ref)
                    {
                        (*exec_ref)[std::string_view("componentGuid")] =
                            DasGuidToStdString(
                                task.execution_component->component_guid);
                    }
                    (*task_ref)[std::string_view("executionComponent")] =
                        std::move(exec_obj);
                }

                auto guid_str = DasGuidToStdString(task_guid);
                (*tasks_ref)[std::string_view(guid_str)] = std::move(task_obj);
            }
        }
        (*obj)[std::string_view("tasks")] = std::move(tasks_obj);
    }

    if (desc.task_components.has_value())
    {
        auto tc_obj = Das::Utils::MakeYyjsonObject();
        auto tc_ref = tc_obj.as_object();
        if (tc_ref)
        {
            if (desc.task_components->factories.has_value()
                && !desc.task_components->factories->empty())
            {
                auto f_arr = Das::Utils::MakeYyjsonArray();
                auto f_arr_ref = f_arr.as_array();
                if (f_arr_ref)
                {
                    for (const auto& f :
                         desc.task_components->factories.value())
                    {
                        f_arr_ref->emplace_back(std::string(f));
                    }
                }
                (*tc_ref)[std::string_view("factories")] = std::move(f_arr);
            }

            if (desc.task_components->components.has_value()
                && !desc.task_components->components->empty())
            {
                auto comp_obj = Das::Utils::MakeYyjsonObject();
                auto comp_ref = comp_obj.as_object();
                if (comp_ref)
                {
                    for (const auto& [key, entry] :
                         desc.task_components->components.value())
                    {
                        auto entry_obj = Das::Utils::MakeYyjsonObject();
                        auto entry_ref = entry_obj.as_object();
                        if (entry_ref)
                        {
                            if (entry.factory_guid.has_value())
                            {
                                (*entry_ref)[std::string_view("factoryGuid")] =
                                    std::make_pair(
                                        std::string_view(
                                            entry.factory_guid.value()),
                                        yyjson::copy_string);
                            }
                            if (entry.definition.has_value())
                            {
                                (*entry_ref)[std::string_view("definition")] =
                                    entry.definition.value();
                            }
                        }
                        (*comp_ref)[std::string_view(key)] =
                            std::move(entry_obj);
                    }
                }
                (*tc_ref)[std::string_view("components")] = std::move(comp_obj);
            }
        }
        (*obj)[std::string_view("taskComponents")] = std::move(tc_obj);
    }

    return j;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
