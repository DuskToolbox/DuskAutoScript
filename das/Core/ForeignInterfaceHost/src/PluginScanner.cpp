#include <das/Core/ForeignInterfaceHost/PluginScanner.h>

#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasConfig.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>

#include <fstream>
#include <iterator>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

// ─── LocalFileProvider ────────────────────────────
// Local filesystem implementation, visible only within this TU.

class LocalFileProvider
{
    std::filesystem::path base_dir_;

public:
    explicit LocalFileProvider(std::filesystem::path base_dir)
        : base_dir_(std::move(base_dir))
    {
    }

    std::vector<FileEntry> ListDirectory(
        const std::string& relative_path_u8,
        bool               recursive)
    {
        std::filesystem::path target = base_dir_;
        if (!relative_path_u8.empty())
        {
            target = base_dir_
                     / std::filesystem::path(
                         std::u8string_view(
                             reinterpret_cast<const char8_t*>(
                                 relative_path_u8.data()),
                             relative_path_u8.size()));
        }

        std::vector<FileEntry> entries;
        std::error_code        ec;

        auto opts = std::filesystem::directory_options::skip_permission_denied;

        if (recursive)
        {
            for (const auto& e : std::filesystem::recursive_directory_iterator(
                     target,
                     opts,
                     ec))
            {
                if (ec)
                {
                    break;
                }
                auto u8name = e.path().filename().u8string();
                auto u8abs = e.path().u8string();
                entries.push_back({
                    .name = std::string{DAS::Utils::U8AsString(u8name)},
                    .absolute_path = std::string{DAS::Utils::U8AsString(u8abs)},
                    .is_directory = e.is_directory(),
                });
            }
        }
        else
        {
            for (const auto& e :
                 std::filesystem::directory_iterator(target, opts, ec))
            {
                if (ec)
                {
                    break;
                }
                auto u8name = e.path().filename().u8string();
                auto u8abs = e.path().u8string();
                entries.push_back({
                    .name = std::string{DAS::Utils::U8AsString(u8name)},
                    .absolute_path = std::string{DAS::Utils::U8AsString(u8abs)},
                    .is_directory = e.is_directory(),
                });
            }
        }

        return entries;
    }

    std::string ReadFile(const std::string& relative_path_u8)
    {
        auto target =
            base_dir_
            / std::filesystem::path(
                std::u8string_view(
                    reinterpret_cast<const char8_t*>(relative_path_u8.data()),
                    relative_path_u8.size()));

        std::ifstream ifs(target, std::ios::binary);
        if (!ifs)
        {
            return {};
        }
        return std::string(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
    }

    std::string GetBasePath() const
    {
        auto u8 = base_dir_.u8string();
        return std::string{DAS::Utils::U8AsString(u8)};
    }
};

// ─── Thin wrapper: ScanPlugins delegates to template ───

std::vector<ScanResult> ScanPlugins(const std::filesystem::path& plugin_dir)
{
    if (!std::filesystem::exists(plugin_dir))
    {
        return {};
    }

    LocalFileProvider provider(plugin_dir);
    auto              scanned = ScanPluginsWith(provider);

    // Post-filter: flat-file mode requires companion plugin binary to exist.
    std::vector<ScanResult> result;
    for (auto& sr : scanned)
    {
        auto plugin_subdir = plugin_dir / sr.desc.name;
        if (std::filesystem::exists(plugin_subdir)
            && std::filesystem::is_directory(plugin_subdir))
        {
            // Directory mode — no extra check needed
            result.push_back(std::move(sr));
        }
        else
        {
            // Flat-file mode — verify companion plugin binary exists
            auto plugin_file =
                plugin_dir
                / (sr.desc.name + "." + sr.desc.plugin_filename_extension);
            if (std::filesystem::exists(plugin_file))
            {
                result.push_back(std::move(sr));
            }
        }
    }

    return result;
}

std::filesystem::path FindManifest(
    const std::filesystem::path& plugin_dir_entry)
{
    auto dirname = std::string{
        DAS::Utils::U8AsString(plugin_dir_entry.filename().u8string())};

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
                    auto plugin_name = std::string{
                        DAS::Utils::U8AsString(sub.path().stem().u8string())};
                    auto parent_name = std::string{DAS::Utils::U8AsString(
                        entry.path().filename().u8string())};
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

            auto plugin_name = std::string{
                DAS::Utils::U8AsString(entry.path().stem().u8string())};
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

    for (const auto& sr : plugins)
    {
        if (sr.desc.guid == guid)
        {
            // Determine plugin mode: directory or flat-file
            auto                  plugin_subdir = plugin_dir / sr.desc.name;
            std::filesystem::path marker_path;
            if (std::filesystem::exists(plugin_subdir)
                && std::filesystem::is_directory(plugin_subdir))
            {
                marker_path = plugin_subdir / (sr.desc.name + ".willBeDelete");
            }
            else
            {
                marker_path = plugin_dir / (sr.desc.name + ".willBeDelete");
            }

            std::ofstream ofs(marker_path);
            if (!ofs)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to create deletion marker: {}",
                    DAS::Utils::U8AsString(marker_path.u8string()));
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

// 将 PluginSettingDesc 转换为 NativeUI 动态表单消费的配置项结构
// {name, description, valueType, inputType, defaultValue, options, required}。
//
// valueType 沿用 DasType 枚举值（0=INT,1=UINT,2=FLOAT,4=STRING,8=BOOL,
// 16=JSON_OBJECT,32=JSON_ARRAY），与前端 profiles_store 的 valueType 完全一致。
// inputType：enum_values 非空 → 1（单选），否则 0（自由文本输入）；
//   多选(2)/长单行(3) 暂无 manifest 字段表达，保留为后续扩展点。
// options 直接取自 enum_values，无枚举时输出空数组。
yyjson::value NativeConfigItemFromSetting(const PluginSettingDesc& setting)
{
    auto j = Das::Utils::MakeYyjsonObject();
    auto obj = j.as_object();
    if (!obj)
    {
        return j;
    }

    (*obj)[std::string_view("name")] = std::make_pair(
        std::string_view(setting.name),
        yyjson::copy_string);

    (*obj)[std::string_view("description")] = std::make_pair(
        std::string_view(setting.description.value_or("")),
        yyjson::copy_string);

    (*obj)[std::string_view("valueType")] =
        static_cast<std::int64_t>(setting.type);

    const bool has_enum =
        setting.enum_values.has_value() && !setting.enum_values->empty();
    (*obj)[std::string_view("inputType")] =
        static_cast<std::int64_t>(has_enum ? 1 : 0);

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
                (*obj)[std::string_view("defaultValue")] = std::make_pair(
                    std::string_view(val),
                    yyjson::copy_string);
            }
        },
        setting.default_value);

    auto opts_arr = Das::Utils::MakeYyjsonArray();
    auto opts_ref = opts_arr.as_array();
    if (opts_ref && has_enum)
    {
        for (const auto& v : setting.enum_values.value())
        {
            opts_ref->emplace_back(std::string(v));
        }
    }
    (*obj)[std::string_view("options")] = std::move(opts_arr);

    (*obj)[std::string_view("required")] = setting.required;

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

    // items[]：NativeUI 契约 —— 插件项列表，每项带 defaultConfigSchema。
    // 数据源全部来自现有 PluginPackageDesc，无需扩展 manifest 或 IDL：
    //   - 插件包级配置（settings_groups / settings_desc）→ 一个 type="plugin" 项
    //   - 每个 task → 一个 type="tool" 项，defaultConfigSchema 由 task.descriptors 合成
    // 仅在有可输出内容时才追加 items 字段，保持与现有 settings/tasks 条件输出风格一致。
    const bool has_plugin_level_settings =
        !desc.settings_groups.empty() || !desc.settings_desc.empty();
    const bool has_task_items = !desc.task_descriptors.empty();
    if (has_plugin_level_settings || has_task_items)
    {
        auto items_arr = Das::Utils::MakeYyjsonArray();
        auto items_ref = items_arr.as_array();
        if (items_ref)
        {
            // 插件包级配置 → type="plugin" 的 PluginItem
            if (has_plugin_level_settings)
            {
                auto plugin_item = Das::Utils::MakeYyjsonObject();
                auto plugin_ref = plugin_item.as_object();
                if (plugin_ref)
                {
                    (*plugin_ref)[std::string_view("id")] =
                        DasGuidToStdString(desc.guid);
                    (*plugin_ref)[std::string_view("name")] = std::make_pair(
                        std::string_view(desc.name),
                        yyjson::copy_string);
                    (*plugin_ref)[std::string_view("description")] =
                        std::make_pair(
                            std::string_view(desc.description),
                            yyjson::copy_string);
                    (*plugin_ref)[std::string_view("type")] = std::make_pair(
                        std::string_view("plugin"),
                        yyjson::copy_string);

                    auto schema = Das::Utils::MakeYyjsonObject();
                    auto schema_ref = schema.as_object();
                    auto groups_arr = Das::Utils::MakeYyjsonArray();
                    auto groups_ref = groups_arr.as_array();
                    if (groups_ref)
                    {
                        // 每个 settings_group → 一个 group
                        for (const auto& [guid, group] : desc.settings_groups)
                        {
                            auto g_obj = Das::Utils::MakeYyjsonObject();
                            auto g_ref = g_obj.as_object();
                            if (!g_ref)
                            {
                                continue;
                            }
                            (*g_ref)[std::string_view("groupId")] =
                                DasGuidToStdString(guid);
                            (*g_ref)[std::string_view("groupName")] =
                                std::make_pair(
                                    std::string_view(group.name),
                                    yyjson::copy_string);

                            auto g_items = Das::Utils::MakeYyjsonArray();
                            auto g_items_ref = g_items.as_array();
                            if (g_items_ref)
                            {
                                for (const auto& d : group.descriptors)
                                {
                                    g_items_ref->emplace_back(
                                        Details::NativeConfigItemFromSetting(
                                            d));
                                }
                            }
                            (*g_ref)[std::string_view("items")] =
                                std::move(g_items);

                            groups_ref->emplace_back(std::move(g_obj));
                        }

                        // 顶层 settings_desc（无分组）归入 "general" 分组
                        if (!desc.settings_desc.empty())
                        {
                            auto g_obj = Das::Utils::MakeYyjsonObject();
                            auto g_ref = g_obj.as_object();
                            if (g_ref)
                            {
                                (*g_ref)[std::string_view("groupId")] =
                                    std::make_pair(
                                        std::string_view("general"),
                                        yyjson::copy_string);
                                (*g_ref)[std::string_view("groupName")] =
                                    std::make_pair(
                                        std::string_view("General"),
                                        yyjson::copy_string);

                                auto g_items = Das::Utils::MakeYyjsonArray();
                                auto g_items_ref = g_items.as_array();
                                if (g_items_ref)
                                {
                                    for (const auto& d : desc.settings_desc)
                                    {
                                        g_items_ref->emplace_back(
                                            Details::
                                                NativeConfigItemFromSetting(
                                                    d));
                                    }
                                }
                                (*g_ref)[std::string_view("items")] =
                                    std::move(g_items);

                                groups_ref->emplace_back(std::move(g_obj));
                            }
                        }
                    }

                    if (schema_ref)
                    {
                        (*schema_ref)[std::string_view("groups")] =
                            std::move(groups_arr);
                    }
                    (*plugin_ref)[std::string_view("defaultConfigSchema")] =
                        std::move(schema);

                    items_ref->emplace_back(std::move(plugin_item));
                }
            }

            // 每个 task → type="tool" 的 PluginItem
            for (const auto& [task_guid, task] : desc.task_descriptors)
            {
                auto task_item = Das::Utils::MakeYyjsonObject();
                auto task_ref = task_item.as_object();
                if (!task_ref)
                {
                    continue;
                }

                (*task_ref)[std::string_view("id")] =
                    DasGuidToStdString(task_guid);
                (*task_ref)[std::string_view("name")] = std::make_pair(
                    std::string_view(task.name),
                    yyjson::copy_string);
                (*task_ref)[std::string_view("description")] = std::make_pair(
                    std::string_view(task.description),
                    yyjson::copy_string);
                (*task_ref)[std::string_view("type")] = std::make_pair(
                    std::string_view("tool"),
                    yyjson::copy_string);

                // defaultConfigSchema 由 task.descriptors 合成
                if (!task.descriptors.empty())
                {
                    auto schema = Das::Utils::MakeYyjsonObject();
                    auto schema_ref = schema.as_object();
                    auto groups_arr = Das::Utils::MakeYyjsonArray();
                    auto groups_ref = groups_arr.as_array();
                    if (groups_ref)
                    {
                        auto g_obj = Das::Utils::MakeYyjsonObject();
                        auto g_ref = g_obj.as_object();
                        if (g_ref)
                        {
                            (*g_ref)[std::string_view("groupId")] =
                                DasGuidToStdString(task_guid);
                            (*g_ref)[std::string_view("groupName")] =
                                std::make_pair(
                                    std::string_view(task.name),
                                    yyjson::copy_string);

                            auto g_items = Das::Utils::MakeYyjsonArray();
                            auto g_items_ref = g_items.as_array();
                            if (g_items_ref)
                            {
                                for (const auto& d : task.descriptors)
                                {
                                    g_items_ref->emplace_back(
                                        Details::NativeConfigItemFromSetting(
                                            d));
                                }
                            }
                            (*g_ref)[std::string_view("items")] =
                                std::move(g_items);

                            groups_ref->emplace_back(std::move(g_obj));
                        }
                    }

                    if (schema_ref)
                    {
                        (*schema_ref)[std::string_view("groups")] =
                            std::move(groups_arr);
                    }
                    (*task_ref)[std::string_view("defaultConfigSchema")] =
                        std::move(schema);
                }

                items_ref->emplace_back(std::move(task_item));
            }
        }
        (*obj)[std::string_view("items")] = std::move(items_arr);
    }

    return j;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
