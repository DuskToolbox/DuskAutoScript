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
        try
        {
            std::ifstream ifs{ui_path};
            if (ifs.is_open())
            {
                global_settings_cache_ = nlohmann::json::parse(ifs);
            }
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

nlohmann::json& SettingsManager::EnsureProfileCached(
    const std::string& profile_id)
{
    auto cache_key = profile_id + "/ui";
    auto it = profile_cache_.find(cache_key);
    if (it != profile_cache_.end())
    {
        return it->second;
    }

    auto content = ReadJsonFile(GetProfileUiPath(profile_id));
    try
    {
        profile_cache_[cache_key] = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        profile_cache_[cache_key] = nlohmann::json::object();
    }
    return profile_cache_[cache_key];
}

// --- String-based methods (legacy) ---

std::string SettingsManager::GetGlobalSettings()
{
    std::shared_lock lock{mutex_};

    if (global_settings_cache_.is_null())
    {
        lock.unlock();
        auto             content = ReadJsonFile(base_dir_ / "ui.json");
        std::unique_lock write_lock{mutex_};
        try
        {
            global_settings_cache_ = nlohmann::json::parse(content);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
        }
        return content;
    }

    return global_settings_cache_.dump();
}

DasResult SettingsManager::UpdateGlobalSettings(const std::string& json_str)
{
    try
    {
        auto parsed = nlohmann::json::parse(json_str);

        std::unique_lock lock{mutex_};
        global_settings_cache_ = std::move(parsed);
        return WriteJsonFile(base_dir_ / "ui.json", json_str);
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

std::string SettingsManager::GetProfileList()
{
    std::shared_lock lock{mutex_};

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
    std::unique_lock lock{mutex_};

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
    std::unique_lock lock{mutex_};

    try
    {
        auto profile_dir = GetProfileDir(profile_id);
        if (!std::filesystem::exists(profile_dir))
        {
            return DAS_S_FALSE;
        }

        std::filesystem::remove_all(profile_dir);

        profile_cache_.erase(profile_id + "/ui");

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
    std::shared_lock lock{mutex_};

    auto cache_key = profile_id + "/ui";
    auto it = profile_cache_.find(cache_key);
    if (it != profile_cache_.end())
    {
        return it->second.dump();
    }

    lock.unlock();
    auto             content = ReadJsonFile(GetProfileUiPath(profile_id));
    std::unique_lock write_lock{mutex_};
    try
    {
        profile_cache_[cache_key] = nlohmann::json::parse(content);
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
    try
    {
        auto parsed = nlohmann::json::parse(json_str);

        std::unique_lock lock{mutex_};
        auto             cache_key = profile_id + "/ui";
        profile_cache_[cache_key] = std::move(parsed);
        return WriteJsonFile(GetProfileUiPath(profile_id), json_str);
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

// --- Plugin settings (split file: settings/${pid}/${pluginGuid}.json) ---

std::string SettingsManager::GetPluginSettings(
    const std::string& profile_id,
    const std::string& guid)
{
    std::shared_lock lock{mutex_};
    return ReadJsonFile(GetPluginSettingsPath(profile_id, guid));
}

DasResult SettingsManager::UpdatePluginSettings(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& json_str)
{
    try
    {
        // Validate JSON before writing
        (void)nlohmann::json::parse(json_str);

        std::unique_lock lock{mutex_};
        return WriteJsonFile(GetPluginSettingsPath(profile_id, guid), json_str);
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
    std::shared_lock lock{mutex_};
    auto content = ReadJsonFile(GetPluginSettingsPath(profile_id, guid));
    try
    {
        return nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return nlohmann::json::object();
    }
}

std::pair<nlohmann::json, DasResult>
SettingsManager::GetPluginSettingsWithStatus(
    const std::string& profile_id,
    const std::string& guid)
{
    auto path = GetPluginSettingsPath(profile_id, guid);

    // Check if file exists
    if (!std::filesystem::exists(path))
    {
        DAS_CORE_LOG_WARN(
            "Plugin settings file missing: {}, returning empty defaults",
            path.string());
        auto defaults = nlohmann::json::object();
        // Persist empty defaults so future reads succeed
        WriteJsonFile(path, defaults);
        return {defaults, DAS_S_FALSE};
    }

    // Read raw file content directly (not via ReadJsonFile which silently
    // returns "{}" for corrupt JSON)
    {
        std::shared_lock lock{mutex_};
        try
        {
            std::ifstream ifs{path};
            if (!ifs.is_open())
            {
                lock.unlock();
                auto defaults = nlohmann::json::object();
                WriteJsonFile(path, defaults);
                return {defaults, DAS_S_FALSE};
            }
            auto parsed = nlohmann::json::parse(ifs);
            if (parsed.is_object())
            {
                return {parsed, DAS_S_OK};
            }
            // Not an object - treat as corrupt
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
        }
    }

    // File is corrupt - rebuild with empty defaults
    DAS_CORE_LOG_WARN(
        "Plugin settings file corrupt: {}, restoring empty defaults",
        path.string());
    auto defaults = nlohmann::json::object();
    WriteJsonFile(path, defaults);
    return {defaults, DAS_S_FALSE};
}

DasResult SettingsManager::UpdatePluginSettingsJson(
    const std::string&    profile_id,
    const std::string&    guid,
    const nlohmann::json& data)
{
    std::unique_lock lock{mutex_};
    return WriteJsonFile(GetPluginSettingsPath(profile_id, guid), data);
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
    std::unique_lock lock{mutex_};

    auto           path = GetPluginSettingsPath(profile_id, guid);
    auto           content = ReadJsonFile(path);
    nlohmann::json settings;
    try
    {
        settings = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        settings = nlohmann::json::object();
    }

    try
    {
        auto* target = EnsureDotPath(settings, field_name);
        *target = value;
        return WriteJsonFile(path, settings);
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

    std::unique_lock lock{mutex_};

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

    return DAS_S_FALSE;
}

// --- JSON-based methods (zero-copy) ---

nlohmann::json SettingsManager::GetGlobalSettingsJson()
{
    std::shared_lock lock{mutex_};

    if (global_settings_cache_.is_null())
    {
        lock.unlock();
        auto             content = ReadJsonFile(base_dir_ / "ui.json");
        std::unique_lock write_lock{mutex_};
        try
        {
            global_settings_cache_ = nlohmann::json::parse(content);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
        }
        return global_settings_cache_;
    }

    return global_settings_cache_;
}

DasResult SettingsManager::UpdateGlobalSettingsJson(const nlohmann::json& data)
{
    try
    {
        std::unique_lock lock{mutex_};
        global_settings_cache_ = data;
        return WriteJsonFile(base_dir_ / "ui.json", data);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

nlohmann::json SettingsManager::GetProfileListJson()
{
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
    std::shared_lock lock{mutex_};

    auto cache_key = profile_id + "/ui";
    auto it = profile_cache_.find(cache_key);
    if (it != profile_cache_.end())
    {
        return it->second;
    }

    lock.unlock();
    auto             content = ReadJsonFile(GetProfileUiPath(profile_id));
    std::unique_lock write_lock{mutex_};
    try
    {
        profile_cache_[cache_key] = nlohmann::json::parse(content);
        return profile_cache_[cache_key];
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
    try
    {
        std::unique_lock lock{mutex_};
        auto             cache_key = profile_id + "/ui";
        profile_cache_[cache_key] = data;
        return WriteJsonFile(GetProfileUiPath(profile_id), data);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

// --- Scheduler state (settings/${pid}/scheduler.json) ---

nlohmann::json SettingsManager::GetSchedulerIndexJson(
    const std::string& profile_id)
{
    std::shared_lock lock{mutex_};
    auto             content = ReadJsonFile(GetSchedulerIndexPath(profile_id));
    try
    {
        auto parsed = nlohmann::json::parse(content);
        if (!parsed.is_object() || !parsed.contains("nextTaskId")
            || !parsed.contains("taskOrder"))
        {
            return {{"nextTaskId", 0}, {"taskOrder", nlohmann::json::array()}};
        }
        return parsed;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return {{"nextTaskId", 0}, {"taskOrder", nlohmann::json::array()}};
    }
}

DasResult SettingsManager::UpdateSchedulerIndexJson(
    const std::string&    profile_id,
    const nlohmann::json& scheduler_json)
{
    try
    {
        std::unique_lock lock{mutex_};
        return WriteJsonFile(GetSchedulerIndexPath(profile_id), scheduler_json);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

// --- Task instance (settings/${pid}/taskId${taskId}.json) ---

nlohmann::json SettingsManager::GetTaskInstanceJson(
    const std::string& profile_id,
    int64_t            task_id)
{
    std::shared_lock lock{mutex_};
    auto content = ReadJsonFile(GetTaskInstancePath(profile_id, task_id));
    try
    {
        return nlohmann::json::parse(content);
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
    try
    {
        std::unique_lock lock{mutex_};
        return WriteJsonFile(
            GetTaskInstancePath(profile_id, task_id),
            task_json);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult SettingsManager::DeleteTaskInstanceJson(
    const std::string& profile_id,
    int64_t            task_id)
{
    std::unique_lock lock{mutex_};

    try
    {
        auto path = GetTaskInstancePath(profile_id, task_id);
        if (!std::filesystem::exists(path))
        {
            return DAS_S_FALSE;
        }
        std::filesystem::remove(path);
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

DAS_CORE_SETTINGS_MANAGER_NS_END
