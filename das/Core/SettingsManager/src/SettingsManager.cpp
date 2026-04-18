#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/Config.h>
#include <das/Core/SettingsManager/SettingsManager.h>

#include <filesystem>
#include <fstream>

DAS_CORE_SETTINGS_MANAGER_NS_BEGIN

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

        // Atomic write: write to temp file, then rename
        auto tmp_path = path;
        tmp_path += ".tmp";
        {
            std::ofstream ofs{tmp_path};
            if (!ofs.is_open())
            {
                return DAS_E_INVALID_FILE;
            }
            // Parse and re-dump with formatting
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

        // Create empty ui.json
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

        // Invalidate caches
        profile_cache_.erase(profile_id + "/ui");
        // Remove all plugin settings for this profile
        auto prefix = profile_id + "/";
        for (auto it = plugin_settings_cache_.begin();
             it != plugin_settings_cache_.end();)
        {
            if (it->first.substr(0, prefix.size()) == prefix)
            {
                it = plugin_settings_cache_.erase(it);
            }
            else
            {
                ++it;
            }
        }

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

std::string SettingsManager::GetPluginSettings(
    const std::string& profile_id,
    const std::string& guid)
{
    std::shared_lock lock{mutex_};

    auto cache_key = profile_id + "/" + guid;
    auto it = plugin_settings_cache_.find(cache_key);
    if (it != plugin_settings_cache_.end())
    {
        return it->second.dump();
    }

    lock.unlock();
    auto content = ReadJsonFile(GetPluginSettingsPath(profile_id, guid));
    std::unique_lock write_lock{mutex_};
    try
    {
        plugin_settings_cache_[cache_key] = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }
    return content;
}

DasResult SettingsManager::UpdatePluginSettings(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& json_str)
{
    try
    {
        auto parsed = nlohmann::json::parse(json_str);

        std::unique_lock lock{mutex_};
        auto             cache_key = profile_id + "/" + guid;
        plugin_settings_cache_[cache_key] = std::move(parsed);
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

DAS_CORE_SETTINGS_MANAGER_NS_END
