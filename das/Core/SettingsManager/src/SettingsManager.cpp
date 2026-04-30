#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/Config.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Utils/DasJsonCore.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

DAS_CORE_SETTINGS_MANAGER_NS_BEGIN

namespace
{
    /// Recursive helper: navigate one segment then recurse.
    std::optional<yyjson::writer::detail::const_value_ref>
    ResolveDotPathRecurse(
        std::string_view                     remaining_path,
        const yyjson::writer::detail::value& current)
    {
        auto obj_opt = current.as_object();
        if (!obj_opt)
        {
            return std::nullopt;
        }
        const auto& obj = obj_opt.value();
        auto        dot = remaining_path.find('.');
        auto        key = remaining_path.substr(0, dot);
        if (!obj.contains(key))
        {
            return std::nullopt;
        }
        if (dot == std::string_view::npos)
        {
            return obj[key];
        }
        auto sub_val = obj[key];
        auto sub_obj = sub_val.as_object();
        if (!sub_obj)
        {
            return std::nullopt;
        }
        // Serialize sub-object to avoid the const_object_ref assignment
        // limitation (libc++ optional requires value type to be assignable).
        auto json_str = sub_obj->write();
        auto parsed = Das::Utils::ParseYyjsonFromString(
            std::string_view(json_str.data(), json_str.size()));
        if (!parsed)
        {
            return std::nullopt;
        }
        return ResolveDotPathRecurse(remaining_path.substr(dot + 1), *parsed);
    }

    /// Navigate a dot-separated path in a yyjson value tree.
    /// Returns nullopt if any intermediate key is missing.
    std::optional<yyjson::writer::detail::const_value_ref> ResolveDotPath(
        const yyjson::writer::detail::value& root,
        const std::string&                   path)
    {
        return ResolveDotPathRecurse(path, root);
    }

    /// Ensure a dot-separated path exists in a mutable yyjson value tree.
    /// Creates any missing intermediate objects.
    /// Returns the value at the final key (or creates it if missing).
    yyjson::writer::detail::value_ref EnsureDotPath(
        yyjson::writer::detail::value& root,
        const std::string&             path)
    {
        using ObjOpt = std::optional<yyjson::writer::detail::object_ref>;
        ObjOpt cur = root.as_object();
        assert(cur.has_value());

        size_t start = 0;
        size_t end = path.find('.');
        while (end != std::string::npos)
        {
            auto& cur_obj = cur.value();
            auto  key = path.substr(start, end - start);
            auto  sub_val = cur_obj[std::string_view(key)];
            if (!sub_val.is_object())
            {
                sub_val = Das::Utils::MakeYyjsonObject();
            }
            cur = sub_val.as_object();
            assert(cur.has_value());
            start = end + 1;
            end = path.find('.', start);
        }
        auto& cur_obj = cur.value();
        auto  key = path.substr(start);
        return cur_obj[std::string_view(key)];
    }
} // namespace

SettingsKeyCell* SettingsManager::GetOrCreateCell(const std::string& key)
{
    // Double-checked locking: first try shared_lock read
    {
        std::shared_lock lock(cells_mutex_);
        auto             it = key_cells_.find(key);
        if (it != key_cells_.end())
        {
            return &it->second;
        }
    }
    // Not found: acquire unique_lock and create
    {
        std::unique_lock lock(cells_mutex_);
        auto [it, inserted] = key_cells_.emplace(key, SettingsKeyCell{});
        return &it->second;
    }
}

SettingsManager::SettingsManager(const std::filesystem::path& base_dir)
    : base_dir_{base_dir}
{
    try
    {
        std::filesystem::create_directories(base_dir_);
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }

    const auto ui_path = base_dir_ / "ui.json";
    if (std::filesystem::exists(ui_path))
    {
        auto*                               cell = GetOrCreateCell("global/ui");
        std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
        auto                                content = ReadJsonFile(ui_path);
        auto parsed = Das::Utils::ParseYyjsonFromString(content);
        if (parsed)
        {
            cell->snapshot = std::move(*parsed);
        }
    }
}

std::string SettingsManager::ReadJsonFile(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
    {
        return "{}";
    }

    try
    {
        std::ifstream ifs{path};
        if (!ifs.is_open())
        {
            return "{}";
        }
        std::stringstream ss;
        ss << ifs.rdbuf();
        auto content = ss.str();
        // Validate by parsing; if OK, re-serialize
        auto parsed = Das::Utils::ParseYyjsonFromString(content);
        if (parsed)
        {
            return *Das::Utils::SerializeYyjsonValue(*parsed, false);
        }
        return "{}";
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return "{}";
    }
}

DasResult SettingsManager::WriteJsonFile(
    const std::filesystem::path& path,
    const std::string&           json_str)
{
    try
    {
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        auto tmp_path = path;
        tmp_path += ".tmp";
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(json_str);
            if (!parsed)
            {
                return DAS_E_INVALID_JSON;
            }
            auto serialized = Das::Utils::SerializeYyjsonValue(*parsed, true);
            if (!serialized)
            {
                return DAS_E_INVALID_JSON;
            }

            std::ofstream ofs{tmp_path};
            if (!ofs.is_open())
            {
                return DAS_E_INVALID_FILE;
            }
            ofs << *serialized;
        }

        std::filesystem::rename(tmp_path, path);
        return DAS_S_OK;
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_FILE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult SettingsManager::WriteJsonFile(
    const std::filesystem::path&         path,
    const yyjson::writer::detail::value& data)
{
    try
    {
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        auto tmp_path = path;
        tmp_path += ".tmp";
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(data, true);
            if (!serialized)
            {
                return DAS_E_INVALID_JSON;
            }

            std::ofstream ofs{tmp_path};
            if (!ofs.is_open())
            {
                return DAS_E_INVALID_FILE;
            }
            ofs << *serialized;
        }

        std::filesystem::rename(tmp_path, path);
        return DAS_S_OK;
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_FILE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

// --- String-based methods (legacy) ---

std::string SettingsManager::GetGlobalSettings()
{
    auto* cell = GetOrCreateCell("global/ui");

    // Fast path: shared_lock for concurrent read access
    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            auto serialized =
                Das::Utils::SerializeYyjsonValue(cell->snapshot, false);
            return serialized ? *serialized : std::string{};
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    // Double-check after upgrade (another thread may have filled it)
    if (!cell->snapshot.is_null())
    {
        auto serialized =
            Das::Utils::SerializeYyjsonValue(cell->snapshot, false);
        return serialized ? *serialized : std::string{};
    }

    auto content = ReadJsonFile(base_dir_ / "ui.json");
    auto parsed = Das::Utils::ParseYyjsonFromString(content);
    if (parsed)
    {
        cell->snapshot = std::move(*parsed);
    }
    return content;
}

DasResult SettingsManager::UpdateGlobalSettings(const std::string& json_str)
{
    auto parsed = Das::Utils::ParseYyjsonFromString(json_str);
    if (!parsed)
    {
        return DAS_E_INVALID_JSON;
    }

    auto* cell = GetOrCreateCell("global/ui");

    // Per-key mutex covers the entire write cycle:
    // snapshot update + WriteJsonFile are both under this mutex
    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = *parsed;
    return WriteJsonFile(base_dir_ / "ui.json", *parsed);
}

std::string SettingsManager::GetProfileList()
{
    // No lock needed for read-only directory traversal
    auto profiles = Das::Utils::MakeYyjsonArray();

    try
    {
        if (!std::filesystem::exists(base_dir_))
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(profiles, false);
            return serialized ? *serialized : std::string{};
        }

        for (const auto& entry : std::filesystem::directory_iterator(base_dir_))
        {
            if (entry.is_directory())
            {
                auto filename = entry.path().filename().string();
                auto profile_str = "{\"profileId\":\"" + filename + "\"}";
                auto parsed = Das::Utils::ParseYyjsonFromString(profile_str);
                if (parsed)
                {
                    profiles.as_array()->emplace_back(std::move(*parsed));
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }

    auto serialized = Das::Utils::SerializeYyjsonValue(profiles, false);
    return serialized ? *serialized : std::string{};
}

DasResult SettingsManager::CreateProfile(const std::string& profile_id)
{
    try
    {
        auto profile_dir = GetProfileDir(profile_id);
        if (std::filesystem::exists(profile_dir))
        {
            return DAS_S_FALSE;
        }

        std::filesystem::create_directories(profile_dir);

        auto          ui_path = GetProfileUiPath(profile_id);
        std::ofstream ofs{ui_path};
        if (!ofs.is_open())
        {
            return DAS_E_INVALID_FILE;
        }
        ofs << "{}";

        return DAS_S_OK;
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_PATH;
    }
}

DasResult SettingsManager::DeleteProfile(const std::string& profile_id)
{
    try
    {
        auto profile_dir = GetProfileDir(profile_id);
        if (!std::filesystem::exists(profile_dir))
        {
            return DAS_S_FALSE;
        }

        std::filesystem::remove_all(profile_dir);
        return DAS_S_OK;
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_PATH;
    }
}

std::string SettingsManager::GetProfile(const std::string& profile_id)
{
    auto  key = profile_id + "/ui";
    auto* cell = GetOrCreateCell(key);

    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            auto serialized =
                Das::Utils::SerializeYyjsonValue(cell->snapshot, false);
            return serialized ? *serialized : std::string{};
        }
    }

    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    if (!cell->snapshot.is_null())
    {
        auto serialized =
            Das::Utils::SerializeYyjsonValue(cell->snapshot, false);
        return serialized ? *serialized : std::string{};
    }

    auto content = ReadJsonFile(GetProfileUiPath(profile_id));
    auto parsed = Das::Utils::ParseYyjsonFromString(content);
    if (parsed)
    {
        cell->snapshot = std::move(*parsed);
    }
    return content;
}

DasResult SettingsManager::UpdateProfile(
    const std::string& profile_id,
    const std::string& json_str)
{
    auto parsed = Das::Utils::ParseYyjsonFromString(json_str);
    if (!parsed)
    {
        return DAS_E_INVALID_JSON;
    }

    auto  key = profile_id + "/ui";
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = *parsed;
    return WriteJsonFile(GetProfileUiPath(profile_id), *parsed);
}

// --- Plugin settings (split file: settings/${pid}/${pluginGuid}.json) ---

std::string SettingsManager::GetPluginSettings(
    const std::string& profile_id,
    const std::string& guid)
{
    auto json = GetPluginSettingsJson(profile_id, guid);
    auto serialized = Das::Utils::SerializeYyjsonValue(json, false);
    return serialized ? *serialized : std::string{};
}

DasResult SettingsManager::UpdatePluginSettings(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& json_str)
{
    auto parsed = Das::Utils::ParseYyjsonFromString(json_str);
    if (!parsed)
    {
        return DAS_E_INVALID_JSON;
    }

    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = *parsed;
    return WriteJsonFile(GetPluginSettingsPath(profile_id, guid), *parsed);
}

yyjson::writer::detail::value SettingsManager::GetPluginSettingsJson(
    const std::string& profile_id,
    const std::string& guid)
{
    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);

    // Fast path: shared_lock for concurrent read access
    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            return cell->snapshot;
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    if (!cell->snapshot.is_null())
    {
        return cell->snapshot;
    }

    auto content = ReadJsonFile(GetPluginSettingsPath(profile_id, guid));
    auto parsed = Das::Utils::ParseYyjsonFromString(content);
    if (parsed)
    {
        cell->snapshot = std::move(*parsed);
    }
    else
    {
        cell->snapshot = Das::Utils::MakeYyjsonObject();
    }
    return cell->snapshot;
}

std::pair<yyjson::writer::detail::value, DasResult>
SettingsManager::GetPluginSettingsWithStatus(
    const std::string& profile_id,
    const std::string& guid)
{
    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);
    auto  path = GetPluginSettingsPath(profile_id, guid);

    // Per-key mutex covers the entire check-read-write cycle
    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);

    // Check if file exists
    if (!std::filesystem::exists(path))
    {
        DAS_CORE_LOG_WARN(
            "Plugin settings file missing: {}, returning empty defaults",
            path.string());
        auto defaults = Das::Utils::MakeYyjsonObject();
        // Persist empty defaults so future reads succeed
        WriteJsonFile(path, defaults);
        cell->snapshot = defaults;
        return {defaults, DAS_S_FALSE};
    }

    // Read raw file content directly
    try
    {
        std::ifstream ifs{path};
        if (!ifs.is_open())
        {
            auto defaults = Das::Utils::MakeYyjsonObject();
            WriteJsonFile(path, defaults);
            cell->snapshot = defaults;
            return {defaults, DAS_S_FALSE};
        }
        std::stringstream ss;
        ss << ifs.rdbuf();
        auto parsed = Das::Utils::ParseYyjsonFromString(ss.str());
        if (parsed && parsed->is_object())
        {
            cell->snapshot = std::move(*parsed);
            return {cell->snapshot, DAS_S_OK};
        }
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }

    // File is corrupt - rebuild with empty defaults
    DAS_CORE_LOG_WARN(
        "Plugin settings file corrupt: {}, restoring empty defaults",
        path.string());
    auto defaults = Das::Utils::MakeYyjsonObject();
    WriteJsonFile(path, defaults);
    cell->snapshot = defaults;
    return {defaults, DAS_S_FALSE};
}

DasResult SettingsManager::UpdatePluginSettingsJson(
    const std::string&                   profile_id,
    const std::string&                   guid,
    const yyjson::writer::detail::value& data)
{
    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = data;
    auto result = WriteJsonFile(GetPluginSettingsPath(profile_id, guid), data);

    if (DAS::IsOk(result))
    {
        auto event = Das::Utils::MakeYyjsonObject();
        auto obj = *event.as_object();
        obj[std::string_view("type")] = std::string_view("settings_changed");
        obj[std::string_view("profile")] = std::string_view(profile_id);
        obj[std::string_view("guid")] = std::string_view(guid);
        obj[std::string_view("field")] = std::string_view("");
        obj[std::string_view("value")] = data;
        auto serialized = Das::Utils::SerializeYyjsonValue(event, false);
        if (serialized && settings_notify_)
        {
            settings_notify_(serialized->c_str());
        }
    }
    return result;
}

std::string SettingsManager::GetPluginSettingsField(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& field_name)
{
    auto settings = GetPluginSettingsJson(profile_id, guid);
    if (settings.is_null() || !settings.is_object())
    {
        return {};
    }

    auto field = ResolveDotPath(settings, field_name);
    if (!field)
    {
        return {};
    }

    try
    {
        auto result = field->write(yyjson::WriteFlag::NoFlag);
        return std::string(result.data(), result.size());
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return {};
    }
}

DasResult SettingsManager::UpdatePluginSettingsField(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& field_name,
    const std::string& field_json_value)
{
    auto field_value = Das::Utils::ParseYyjsonFromString(field_json_value);
    if (!field_value)
    {
        return DAS_E_INVALID_JSON;
    }

    return UpdatePluginSettingsFieldJson(
        profile_id,
        guid,
        field_name,
        *field_value);
}

yyjson::writer::detail::value SettingsManager::GetPluginSettingsFieldJson(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& field_name)
{
    auto settings = GetPluginSettingsJson(profile_id, guid);
    if (settings.is_null() || !settings.is_object())
    {
        return yyjson::writer::detail::value{};
    }

    auto field = ResolveDotPath(settings, field_name);
    if (!field)
    {
        return yyjson::writer::detail::value{};
    }

    // We need to create an owning copy of the field value.
    // Parse the serialized form to get a new yyjson::value.
    try
    {
        auto serialized = field->write(yyjson::WriteFlag::NoFlag);
        auto parsed = Das::Utils::ParseYyjsonFromString(
            std::string_view(serialized.data(), serialized.size()));
        return parsed ? std::move(*parsed) : yyjson::writer::detail::value{};
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return yyjson::writer::detail::value{};
    }
}

DasResult SettingsManager::UpdatePluginSettingsFieldJson(
    const std::string&                   profile_id,
    const std::string&                   guid,
    const std::string&                   field_name,
    const yyjson::writer::detail::value& value)
{
    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);

    // Per-key mutex protects the entire RMW cycle:
    // read current -> modify -> WriteJsonFile -> update snapshot
    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);

    auto path = GetPluginSettingsPath(profile_id, guid);
    auto current = cell->snapshot;
    if (current.is_null())
    {
        auto content = ReadJsonFile(path);
        auto parsed = Das::Utils::ParseYyjsonFromString(content);
        if (parsed)
        {
            current = std::move(*parsed);
        }
    }

    if (!current.is_object())
    {
        current = Das::Utils::MakeYyjsonObject();
    }

    try
    {
        auto target = EnsureDotPath(current, field_name);
        target = value;
        auto result = WriteJsonFile(path, current);
        if (DAS::IsOk(result))
        {
            cell->snapshot = std::move(current);

            // Notify WebSocket clients of settings change
            auto event = Das::Utils::MakeYyjsonObject();
            auto obj = *event.as_object();
            obj[std::string_view("type")] =
                std::string_view("settings_changed");
            obj[std::string_view("profile")] = std::string_view(profile_id);
            obj[std::string_view("guid")] = std::string_view(guid);
            obj[std::string_view("field")] = std::string_view(field_name);
            obj[std::string_view("value")] = value;
            auto serialized = Das::Utils::SerializeYyjsonValue(event, false);
            if (serialized && settings_notify_)
            {
                settings_notify_(serialized->c_str());
            }
        }
        return result;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult SettingsManager::RebuildPluginSettingsFromDefaults(
    const std::string&              profile_id,
    const std::string&              guid,
    const std::vector<std::string>& field_names,
    const std::vector<std::string>& default_values)
{
    if (field_names.size() != default_values.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);

    // Per-key mutex protects the entire rebuild cycle:
    // exists check + ifstream + parse/defaults + WriteJsonFile + snapshot
    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);

    auto path = GetPluginSettingsPath(profile_id, guid);

    // Try to read existing file
    bool needs_rebuild = false;
    if (!std::filesystem::exists(path))
    {
        needs_rebuild = true;
    }
    else
    {
        try
        {
            std::ifstream ifs{path};
            if (!ifs.is_open())
            {
                needs_rebuild = true;
            }
            else
            {
                std::stringstream ss;
                ss << ifs.rdbuf();
                auto parsed = Das::Utils::ParseYyjsonFromString(ss.str());
                if (!parsed || !parsed->is_object())
                {
                    needs_rebuild = true;
                }
            }
        }
        catch (const std::exception&)
        {
            needs_rebuild = true;
        }
    }

    if (!needs_rebuild)
    {
        return DAS_S_OK;
    }

    // Rebuild from defaults
    auto rebuilt = Das::Utils::MakeYyjsonObject();
    auto obj = *rebuilt.as_object();
    for (size_t i = 0; i < field_names.size(); ++i)
    {
        auto parsed_default =
            Das::Utils::ParseYyjsonFromString(default_values[i]);
        if (parsed_default)
        {
            obj[std::string_view(field_names[i])] = std::move(*parsed_default);
        }
        else
        {
            obj[std::string_view(field_names[i])] =
                std::string_view(default_values[i]);
        }
    }

    auto write_result = WriteJsonFile(path, rebuilt);
    if (IsFailed(write_result))
    {
        return write_result;
    }

    cell->snapshot = rebuilt;
    return DAS_S_FALSE;
}

// --- JSON-based methods (zero-copy) ---

yyjson::writer::detail::value SettingsManager::GetGlobalSettingsJson()
{
    auto* cell = GetOrCreateCell("global/ui");

    // Fast path: shared_lock for concurrent read access
    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            return cell->snapshot;
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    if (!cell->snapshot.is_null())
    {
        return cell->snapshot;
    }

    auto content = ReadJsonFile(base_dir_ / "ui.json");
    auto parsed = Das::Utils::ParseYyjsonFromString(content);
    if (parsed)
    {
        cell->snapshot = std::move(*parsed);
    }
    return cell->snapshot;
}

DasResult SettingsManager::UpdateGlobalSettingsJson(
    const yyjson::writer::detail::value& data)
{
    auto* cell = GetOrCreateCell("global/ui");

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = data;
    return WriteJsonFile(base_dir_ / "ui.json", data);
}

yyjson::writer::detail::value SettingsManager::GetProfileListJson()
{
    // No lock needed for read-only directory traversal
    auto profiles = Das::Utils::MakeYyjsonArray();

    try
    {
        if (!std::filesystem::exists(base_dir_))
        {
            return profiles;
        }

        for (const auto& entry : std::filesystem::directory_iterator(base_dir_))
        {
            if (entry.is_directory())
            {
                auto filename = entry.path().filename().string();
                auto profile_str = "{\"profileId\":\"" + filename + "\"}";
                auto parsed = Das::Utils::ParseYyjsonFromString(profile_str);
                if (parsed)
                {
                    profiles.as_array()->emplace_back(std::move(*parsed));
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }

    return profiles;
}

yyjson::writer::detail::value SettingsManager::GetProfileJson(
    const std::string& profile_id)
{
    auto  key = profile_id + "/ui";
    auto* cell = GetOrCreateCell(key);

    // Fast path: shared_lock for concurrent read access
    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            return cell->snapshot;
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    if (!cell->snapshot.is_null())
    {
        return cell->snapshot;
    }

    auto content = ReadJsonFile(GetProfileUiPath(profile_id));
    auto parsed = Das::Utils::ParseYyjsonFromString(content);
    if (parsed)
    {
        cell->snapshot = std::move(*parsed);
        return cell->snapshot;
    }

    cell->snapshot = Das::Utils::MakeYyjsonObject();
    return cell->snapshot;
}

DasResult SettingsManager::UpdateProfileJson(
    const std::string&                   profile_id,
    const yyjson::writer::detail::value& data)
{
    auto  key = profile_id + "/ui";
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = data;
    return WriteJsonFile(GetProfileUiPath(profile_id), data);
}

// --- Scheduler state (settings/${pid}/scheduler.json) ---

yyjson::writer::detail::value SettingsManager::GetSchedulerIndexJson(
    const std::string& profile_id)
{
    auto  key = "profile/" + profile_id + "/scheduler";
    auto* cell = GetOrCreateCell(key);

    // Fast path: shared_lock for concurrent read access
    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            return cell->snapshot;
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    if (!cell->snapshot.is_null())
    {
        return cell->snapshot;
    }

    auto content = ReadJsonFile(GetSchedulerIndexPath(profile_id));
    auto parsed = Das::Utils::ParseYyjsonFromString(content);
    auto init_scheduler = [&]() -> yyjson::writer::detail::value
    {
        auto sched = Das::Utils::MakeYyjsonObject();
        auto obj = *sched.as_object();
        obj[std::string_view("nextTaskId")] = static_cast<int64_t>(0);
        obj[std::string_view("taskOrder")] = Das::Utils::MakeYyjsonArray();
        return sched;
    };

    if (parsed && parsed->is_object())
    {
        auto obj_ref = parsed->as_object();
        if (obj_ref && obj_ref->contains(std::string_view("nextTaskId"))
            && obj_ref->contains(std::string_view("taskOrder")))
        {
            cell->snapshot = std::move(*parsed);
            return cell->snapshot;
        }
    }

    cell->snapshot = init_scheduler();
    return cell->snapshot;
}

DasResult SettingsManager::UpdateSchedulerIndexJson(
    const std::string&                   profile_id,
    const yyjson::writer::detail::value& scheduler_json)
{
    auto  key = "profile/" + profile_id + "/scheduler";
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = scheduler_json;
    return WriteJsonFile(GetSchedulerIndexPath(profile_id), scheduler_json);
}

// --- Task instance (settings/${pid}/taskId${taskId}.json) ---

yyjson::writer::detail::value SettingsManager::GetTaskInstanceJson(
    const std::string& profile_id,
    int64_t            task_id)
{
    auto  key = "profile/" + profile_id + "/task/" + std::to_string(task_id);
    auto* cell = GetOrCreateCell(key);

    // Fast path: shared_lock for concurrent read access
    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            return cell->snapshot;
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    if (!cell->snapshot.is_null())
    {
        return cell->snapshot;
    }

    auto content = ReadJsonFile(GetTaskInstancePath(profile_id, task_id));
    auto parsed = Das::Utils::ParseYyjsonFromString(content);
    if (parsed)
    {
        cell->snapshot = std::move(*parsed);
        return cell->snapshot;
    }

    cell->snapshot = Das::Utils::MakeYyjsonObject();
    return cell->snapshot;
}

DasResult SettingsManager::UpdateTaskInstanceJson(
    const std::string&                   profile_id,
    int64_t                              task_id,
    const yyjson::writer::detail::value& task_json)
{
    auto  key = "profile/" + profile_id + "/task/" + std::to_string(task_id);
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = task_json;
    return WriteJsonFile(GetTaskInstancePath(profile_id, task_id), task_json);
}

DasResult SettingsManager::DeleteTaskInstanceJson(
    const std::string& profile_id,
    int64_t            task_id)
{
    auto  key = "profile/" + profile_id + "/task/" + std::to_string(task_id);
    auto* cell = GetOrCreateCell(key);

    // Per-key mutex protects the delete operation + snapshot update
    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);

    try
    {
        auto path = GetTaskInstancePath(profile_id, task_id);
        if (!std::filesystem::exists(path))
        {
            return DAS_S_FALSE;
        }
        std::filesystem::remove(path);
        cell->snapshot = yyjson::writer::detail::value{};
        return DAS_S_OK;
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_FILE;
    }
}

// --- Path helpers ---

std::filesystem::path SettingsManager::GetProfileDir(
    const std::string& profile_id) const
{
    return base_dir_ / profile_id;
}

std::filesystem::path SettingsManager::GetProfileUiPath(
    const std::string& profile_id) const
{
    return base_dir_ / profile_id / "ui.json";
}

std::filesystem::path SettingsManager::GetPluginSettingsPath(
    const std::string& profile_id,
    const std::string& guid) const
{
    return base_dir_ / profile_id / (guid + ".json");
}

std::filesystem::path SettingsManager::GetSchedulerIndexPath(
    const std::string& profile_id) const
{
    return base_dir_ / profile_id / "scheduler.json";
}

std::filesystem::path SettingsManager::GetTaskInstancePath(
    const std::string& profile_id,
    int64_t            task_id) const
{
    return base_dir_ / profile_id
           / ("taskId" + std::to_string(task_id) + ".json");
}

void SettingsManager::SetSettingsNotifyCallback(
    SettingsNotifyFunc func,
    void*              user_data)
{
    settings_notify_ = {func, user_data};
}

DAS_CORE_SETTINGS_MANAGER_NS_END
