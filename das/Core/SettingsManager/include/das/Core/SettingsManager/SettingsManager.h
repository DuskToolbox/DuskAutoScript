#ifndef DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H
#define DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H

#include <das/Core/SettingsManager/Config.h>
#include <das/DasExport.h>
#include <das/DasTypes.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>

DAS_CORE_SETTINGS_MANAGER_NS_BEGIN

class SettingsManager
{
public:
    explicit SettingsManager(const std::filesystem::path& base_dir);
    ~SettingsManager() = default;

    // Global Settings (settings/ui.json)
    std::string    GetGlobalSettings();
    nlohmann::json GetGlobalSettingsJson();
    DasResult      UpdateGlobalSettings(const std::string& json_str);
    DasResult      UpdateGlobalSettingsJson(const nlohmann::json& data);

    // Profile management (settings/${pid}/)
    std::string    GetProfileList();
    nlohmann::json GetProfileListJson();
    DasResult      CreateProfile(const std::string& profile_id);
    DasResult      DeleteProfile(const std::string& profile_id);

    // Profile data (settings/${pid}/ui.json)
    std::string    GetProfile(const std::string& profile_id);
    nlohmann::json GetProfileJson(const std::string& profile_id);
    DasResult      UpdateProfile(
        const std::string& profile_id,
        const std::string& json_str);
    DasResult UpdateProfileJson(
        const std::string&    profile_id,
        const nlohmann::json& data);

    // Plugin settings (profile[guid] within settings/${pid}/ui.json)
    nlohmann::json GetPluginSettingsJson(
        const std::string& profile_id,
        const std::string& guid);
    std::string GetPluginSettings(
        const std::string& profile_id,
        const std::string& guid);
    DasResult UpdatePluginSettings(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& json_str);
    DasResult UpdatePluginSettingsJson(
        const std::string&    profile_id,
        const std::string&    guid,
        const nlohmann::json& data);

    // Plugin settings field-level access (JSON object, no serialization)
    nlohmann::json GetPluginSettingsFieldJson(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name);
    DasResult UpdatePluginSettingsFieldJson(
        const std::string&    profile_id,
        const std::string&    guid,
        const std::string&    field_name,
        const nlohmann::json& value);

    // Plugin settings field-level access (string-based, legacy)
    std::string GetPluginSettingsField(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name);
    DasResult UpdatePluginSettingsField(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name,
        const std::string& field_json_value);

    // Scheduler state (profile.scheduler within settings/${pid}/ui.json)
    nlohmann::json GetSchedulerStateJson(const std::string& profile_id);
    DasResult      UpdateSchedulerStateJson(
        const std::string&    profile_id,
        const nlohmann::json& scheduler_json);

private:
    std::filesystem::path GetProfileDir(const std::string& profile_id) const;
    std::filesystem::path GetProfileUiPath(const std::string& profile_id) const;

    // File I/O helpers
    static std::string ReadJsonFile(const std::filesystem::path& path);
    static DasResult   WriteJsonFile(
        const std::filesystem::path& path,
        const std::string&           json_str);
    static DasResult WriteJsonFile(
        const std::filesystem::path& path,
        const nlohmann::json&        data);

    /// Ensure the profile JSON is loaded and cached. Returns the cached
    /// profile JSON reference under write lock.
    nlohmann::json& EnsureProfileCached(const std::string& profile_id);

    std::filesystem::path                           base_dir_;
    nlohmann::json                                  global_settings_cache_;
    std::unordered_map<std::string, nlohmann::json> profile_cache_;
    mutable std::shared_mutex                       mutex_;
};

DAS_CORE_SETTINGS_MANAGER_NS_END

#endif // DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H
