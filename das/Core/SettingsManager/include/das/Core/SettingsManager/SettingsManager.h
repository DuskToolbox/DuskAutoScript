#ifndef DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H
#define DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H

#include <das/Core/SettingsManager/Config.h>
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
    std::string GetGlobalSettings();
    DasResult   UpdateGlobalSettings(const std::string& json_str);

    // Profile management (settings/${pid}/)
    std::string GetProfileList();
    DasResult   CreateProfile(const std::string& profile_id);
    DasResult   DeleteProfile(const std::string& profile_id);

    // Profile data (settings/${pid}/ui.json)
    std::string GetProfile(const std::string& profile_id);
    DasResult   UpdateProfile(
        const std::string& profile_id,
        const std::string& json_str);

    // Plugin settings (settings/${pid}/${guid}.json)
    std::string GetPluginSettings(
        const std::string& profile_id,
        const std::string& guid);
    DasResult UpdatePluginSettings(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& json_str);

private:
    std::filesystem::path GetProfileDir(const std::string& profile_id) const;
    std::filesystem::path GetProfileUiPath(const std::string& profile_id) const;
    std::filesystem::path GetPluginSettingsPath(
        const std::string& profile_id,
        const std::string& guid) const;

    // File I/O helpers
    static std::string ReadJsonFile(const std::filesystem::path& path);
    static DasResult   WriteJsonFile(
        const std::filesystem::path& path,
        const std::string&           json_str);

    std::filesystem::path                           base_dir_;
    nlohmann::json                                  global_settings_cache_;
    std::unordered_map<std::string, nlohmann::json> profile_cache_;
    std::unordered_map<std::string, nlohmann::json> plugin_settings_cache_;
    mutable std::shared_mutex                       mutex_;
};

DAS_CORE_SETTINGS_MANAGER_NS_END

#endif // DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H
