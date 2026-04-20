#ifndef DAS_SETTINGS_SERVICE_H
#define DAS_SETTINGS_SERVICE_H

#include <das/IDasBase.h>
#include <nlohmann/json.hpp>
#include <string>

DAS_DEFINE_GUID(
    DAS_IID_SETTINGS_SERVICE,
    IDasSettingsService,
    0xA1B2C3D4,
    0xE5F6,
    0x4A7B,
    0x8C,
    0x9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D)

DAS_SWIG_EXPORT_ATTRIBUTE(IDasSettingsService)
DAS_INTERFACE IDasSettingsService : public IDasBase
{
    // Global Settings (settings/ui.json)
    DAS_METHOD_(nlohmann::json) GetGlobalSettings() = 0;
    DAS_METHOD UpdateGlobalSettings(const nlohmann::json& data) = 0;

    // Profile management
    DAS_METHOD_(nlohmann::json) GetProfileList() = 0;
    DAS_METHOD CreateProfile(const std::string& profile_id) = 0;
    DAS_METHOD DeleteProfile(const std::string& profile_id) = 0;
    DAS_METHOD_(nlohmann::json) GetProfile(const std::string& profile_id) = 0;
    DAS_METHOD UpdateProfile(
        const std::string&    profile_id,
        const nlohmann::json& data) = 0;

    // Plugin settings
    DAS_METHOD_(nlohmann::json)
    GetPluginSettings(const std::string& profile_id, const std::string& guid) =
        0;
    DAS_METHOD UpdatePluginSettings(
        const std::string&    profile_id,
        const std::string&    guid,
        const nlohmann::json& data) = 0;

    // Plugin settings field-level access
    DAS_METHOD_(nlohmann::json)
    GetPluginSettingsField(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name) = 0;
    DAS_METHOD UpdatePluginSettingsField(
        const std::string&    profile_id,
        const std::string&    guid,
        const std::string&    field_name,
        const nlohmann::json& value) = 0;
};

#endif // DAS_SETTINGS_SERVICE_H
