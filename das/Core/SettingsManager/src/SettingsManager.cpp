#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/Config.h>
#include <das/Core/SettingsManager/SettingsManager.h>

#include <filesystem>
#include <fstream>
#include <utility>

DAS_CORE_SETTINGS_MANAGER_NS_BEGIN

namespace
{
    nlohmann::json* ResolveDotPath(
        nlohmann::json&    root,
        const std::string& path)
    {
        nlohmann::json* current = &root;
        size_t          start = 0;
        size_t          end = path.find('.');
        while (end != std::string::npos)
        {
            auto key = path.substr(start, end - start);
            if (!current->contains(key))
            {
                return nullptr;
            }
            current = &(*current)[key];
            start = end + 1;
            end = path.find('.', start);
        }
        auto key = path.substr(start);
        if (!current->contains(key))
        {
            return nullptr;
        }
        return &(*current)[key];
    }

    nlohmann::json* EnsureDotPath(nlohmann::json& root, const std::string& path)
    {
        nlohmann::json* current = &root;
        size_t          start = 0;
        size_t          end = path.find('.');
        while (end != std::string::npos)
        {
            auto key = path.substr(start, end - start);
            if (!current->contains(key))
            {
                (*current)[key] = nlohmann::json::object();
            }
            else if (!(*current)[key].is_object())
            {
                (*current)[key] = nlohmann::json::object();
            }
            current = &(*current)[key];
            start = end + 1;
            end = path.find('.', start);
        }
        auto key = path.substr(start);
        return &(*current)[key];
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
        try
        {
            cell->snapshot = nlohmann::json::parse(content);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
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
        auto json = nlohmann::json::parse(ifs);
        return json.dump();
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
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
            std::ofstream ofs{tmp_path};
            if (!ofs.is_open())
            {
                return DAS_E_INVALID_FILE;
            }
            auto json = nlohmann::json::parse(json_str);
            ofs << json.dump(4);
        }

        std::filesystem::rename(tmp_path, path);
        return DAS_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
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
    const std::filesystem::path& path,
    const nlohmann::json&        data)
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
            std::ofstream ofs{tmp_path};
            if (!ofs.is_open())
            {
                return DAS_E_INVALID_FILE;
            }
            ofs << data.dump(4);
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
            return cell->snapshot.dump();
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    // Double-check after upgrade (another thread may have filled it)
    if (!cell->snapshot.is_null())
    {
        return cell->snapshot.dump();
    }

    auto content = ReadJsonFile(base_dir_ / "ui.json");
    try
    {
        cell->snapshot = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }
    return content;
}

DasResult SettingsManager::UpdateGlobalSettings(const std::string& json_str)
{
    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(json_str);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }

    auto* cell = GetOrCreateCell("global/ui");

    // Per-key mutex covers the entire write cycle:
    // snapshot update + WriteJsonFile are both under this mutex
    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = parsed;
    return WriteJsonFile(base_dir_ / "ui.json", parsed);
}

std::string SettingsManager::GetProfileList()
{
    // No lock needed for read-only directory traversal
    nlohmann::json profiles = nlohmann::json::array();

    try
    {
        if (!std::filesystem::exists(base_dir_))
        {
            return profiles.dump();
        }

        for (const auto& entry : std::filesystem::directory_iterator(base_dir_))
        {
            if (entry.is_directory())
            {
                profiles.push_back(
                    {{"profileId", entry.path().filename().string()}});
            }
        }
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }

    return profiles.dump();
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

    // Fast path: shared_lock for concurrent read access
    {
        std::shared_lock lock(cell->mutex);
        if (!cell->snapshot.is_null())
        {
            return cell->snapshot.dump();
        }
    }

    // Cache miss: acquire unique_lock for file read + snapshot fill
    std::unique_lock<std::shared_mutex> lock(cell->mutex);
    if (!cell->snapshot.is_null())
    {
        return cell->snapshot.dump();
    }

    auto content = ReadJsonFile(GetProfileUiPath(profile_id));
    try
    {
        cell->snapshot = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }
    return content;
}

DasResult SettingsManager::UpdateProfile(
    const std::string& profile_id,
    const std::string& json_str)
{
    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(json_str);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }

    auto  key = profile_id + "/ui";
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = parsed;
    return WriteJsonFile(GetProfileUiPath(profile_id), parsed);
}

// --- Plugin settings (split file: settings/${pid}/${pluginGuid}.json) ---

std::string SettingsManager::GetPluginSettings(
    const std::string& profile_id,
    const std::string& guid)
{
    auto json = GetPluginSettingsJson(profile_id, guid);
    return json.dump();
}

DasResult SettingsManager::UpdatePluginSettings(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& json_str)
{
    try
    {
        // Validate JSON before writing
        auto parsed = nlohmann::json::parse(json_str);

        auto  key = "profile/" + profile_id + "/plugin/" + guid;
        auto* cell = GetOrCreateCell(key);

        std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
        cell->snapshot = parsed;
        return WriteJsonFile(GetPluginSettingsPath(profile_id, guid), parsed);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

nlohmann::json SettingsManager::GetPluginSettingsJson(
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
    try
    {
        cell->snapshot = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        cell->snapshot = nlohmann::json::object();
    }
    return cell->snapshot;
}

std::pair<nlohmann::json, DasResult>
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
        auto defaults = nlohmann::json::object();
        // Persist empty defaults so future reads succeed
        WriteJsonFile(path, defaults);
        cell->snapshot = defaults;
        return {defaults, DAS_S_FALSE};
    }

    // Read raw file content directly (not via ReadJsonFile which silently
    // returns "{}" for corrupt JSON)
    try
    {
        std::ifstream ifs{path};
        if (!ifs.is_open())
        {
            auto defaults = nlohmann::json::object();
            WriteJsonFile(path, defaults);
            cell->snapshot = defaults;
            return {defaults, DAS_S_FALSE};
        }
        auto parsed = nlohmann::json::parse(ifs);
        if (parsed.is_object())
        {
            cell->snapshot = parsed;
            return {parsed, DAS_S_OK};
        }
        // Not an object - treat as corrupt
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }

    // File is corrupt - rebuild with empty defaults
    DAS_CORE_LOG_WARN(
        "Plugin settings file corrupt: {}, restoring empty defaults",
        path.string());
    auto defaults = nlohmann::json::object();
    WriteJsonFile(path, defaults);
    cell->snapshot = defaults;
    return {defaults, DAS_S_FALSE};
}

DasResult SettingsManager::UpdatePluginSettingsJson(
    const std::string&    profile_id,
    const std::string&    guid,
    const nlohmann::json& data)
{
    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = data;
    auto result = WriteJsonFile(GetPluginSettingsPath(profile_id, guid), data);

    if (DAS::IsOk(result))
    {
        nlohmann::json event;
        event["type"] = "settings_changed";
        event["profile"] = profile_id;
        event["guid"] = guid;
        event["field"] = ""; // full replacement
        event["value"] = data;
        auto event_str = event.dump();
        if (settings_notify_)
        {
            settings_notify_(event_str.c_str());
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

    auto* field = ResolveDotPath(settings, field_name);
    if (field == nullptr)
    {
        return {};
    }

    try
    {
        return field->dump();
    }
    catch (const nlohmann::json::exception& ex)
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
    nlohmann::json field_value;
    try
    {
        field_value = nlohmann::json::parse(field_json_value);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }

    return UpdatePluginSettingsFieldJson(
        profile_id,
        guid,
        field_name,
        field_value);
}

nlohmann::json SettingsManager::GetPluginSettingsFieldJson(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& field_name)
{
    auto settings = GetPluginSettingsJson(profile_id, guid);
    if (settings.is_null() || !settings.is_object())
    {
        return nullptr;
    }

    auto* field = ResolveDotPath(settings, field_name);
    if (field == nullptr)
    {
        return nullptr;
    }
    return *field;
}

DasResult SettingsManager::UpdatePluginSettingsFieldJson(
    const std::string&    profile_id,
    const std::string&    guid,
    const std::string&    field_name,
    const nlohmann::json& value)
{
    auto  key = "profile/" + profile_id + "/plugin/" + guid;
    auto* cell = GetOrCreateCell(key);

    // Per-key mutex protects the entire RMW cycle:
    // read current -> modify -> WriteJsonFile -> update snapshot
    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);

    auto           path = GetPluginSettingsPath(profile_id, guid);
    nlohmann::json current = cell->snapshot;
    if (current.is_null())
    {
        auto content = ReadJsonFile(path);
        try
        {
            current = nlohmann::json::parse(content);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
        }
    }

    if (!current.is_object())
    {
        current = nlohmann::json::object();
    }

    try
    {
        auto* target = EnsureDotPath(current, field_name);
        *target = value;
        auto result = WriteJsonFile(path, current);
        if (DAS::IsOk(result))
        {
            cell->snapshot = std::move(current);

            // Notify WebSocket clients of settings change
            nlohmann::json event;
            event["type"] = "settings_changed";
            event["profile"] = profile_id;
            event["guid"] = guid;
            event["field"] = field_name;
            event["value"] = value;
            auto event_str = event.dump();
            if (settings_notify_)
            {
                settings_notify_(event_str.c_str());
            }
        }
        return result;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
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
                auto existing = nlohmann::json::parse(ifs);
                if (!existing.is_object())
                {
                    needs_rebuild = true;
                }
            }
        }
        catch (const nlohmann::json::exception&)
        {
            needs_rebuild = true;
        }
    }

    if (!needs_rebuild)
    {
        return DAS_S_OK;
    }

    // Rebuild from defaults
    nlohmann::json rebuilt = nlohmann::json::object();
    for (size_t i = 0; i < field_names.size(); ++i)
    {
        try
        {
            rebuilt[field_names[i]] = nlohmann::json::parse(default_values[i]);
        }
        catch (const nlohmann::json::exception&)
        {
            rebuilt[field_names[i]] = default_values[i];
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

nlohmann::json SettingsManager::GetGlobalSettingsJson()
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
    try
    {
        cell->snapshot = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }
    return cell->snapshot;
}

DasResult SettingsManager::UpdateGlobalSettingsJson(const nlohmann::json& data)
{
    auto* cell = GetOrCreateCell("global/ui");

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = data;
    return WriteJsonFile(base_dir_ / "ui.json", data);
}

nlohmann::json SettingsManager::GetProfileListJson()
{
    // No lock needed for read-only directory traversal
    nlohmann::json profiles = nlohmann::json::array();

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
                profiles.push_back(
                    {{"profileId", entry.path().filename().string()}});
            }
        }
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }

    return profiles;
}

nlohmann::json SettingsManager::GetProfileJson(const std::string& profile_id)
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
    try
    {
        cell->snapshot = nlohmann::json::parse(content);
        return cell->snapshot;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return nlohmann::json::object();
    }
}

DasResult SettingsManager::UpdateProfileJson(
    const std::string&    profile_id,
    const nlohmann::json& data)
{
    auto  key = profile_id + "/ui";
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = data;
    return WriteJsonFile(GetProfileUiPath(profile_id), data);
}

// --- Scheduler state (settings/${pid}/scheduler.json) ---

nlohmann::json SettingsManager::GetSchedulerIndexJson(
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
    try
    {
        auto parsed = nlohmann::json::parse(content);
        if (!parsed.is_object() || !parsed.contains("nextTaskId")
            || !parsed.contains("taskOrder"))
        {
            cell->snapshot = {
                {"nextTaskId", 0},
                {"taskOrder", nlohmann::json::array()}};
            return cell->snapshot;
        }
        cell->snapshot = parsed;
        return cell->snapshot;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        cell->snapshot = {
            {"nextTaskId", 0},
            {"taskOrder", nlohmann::json::array()}};
        return cell->snapshot;
    }
}

DasResult SettingsManager::UpdateSchedulerIndexJson(
    const std::string&    profile_id,
    const nlohmann::json& scheduler_json)
{
    auto  key = "profile/" + profile_id + "/scheduler";
    auto* cell = GetOrCreateCell(key);

    std::unique_lock<std::shared_mutex> cell_lock(cell->mutex);
    cell->snapshot = scheduler_json;
    return WriteJsonFile(GetSchedulerIndexPath(profile_id), scheduler_json);
}

// --- Task instance (settings/${pid}/taskId${taskId}.json) ---

nlohmann::json SettingsManager::GetTaskInstanceJson(
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
    try
    {
        cell->snapshot = nlohmann::json::parse(content);
        return cell->snapshot;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return nlohmann::json::object();
    }
}

DasResult SettingsManager::UpdateTaskInstanceJson(
    const std::string&    profile_id,
    int64_t               task_id,
    const nlohmann::json& task_json)
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
        cell->snapshot = nlohmann::json();
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
