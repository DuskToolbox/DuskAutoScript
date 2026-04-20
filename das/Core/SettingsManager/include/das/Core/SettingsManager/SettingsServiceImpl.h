#ifndef DAS_CORE_SETTINGS_MANAGER_SETTINGSSERVICEIMPL_H
#define DAS_CORE_SETTINGS_MANAGER_SETTINGSSERVICEIMPL_H

#include <atomic>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/IDasSettingsService.h>

DAS_CORE_SETTINGS_MANAGER_NS_BEGIN

class SettingsServiceImpl final : public IDasSettingsService
{
public:
    explicit SettingsServiceImpl(SettingsManager& mgr);
    ~SettingsServiceImpl() override = default;

    // IDasBase
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out) override;

    // IDasSettingsService
    std::string GetGlobalSettings() override;
    DasResult   UpdateGlobalSettings(const std::string& json_str) override;
    std::string GetProfileList() override;
    DasResult   CreateProfile(const std::string& profile_id) override;
    DasResult   DeleteProfile(const std::string& profile_id) override;
    std::string GetProfile(const std::string& profile_id) override;
    DasResult   UpdateProfile(
        const std::string& profile_id,
        const std::string& json_str) override;
    std::string GetPluginSettings(
        const std::string& profile_id,
        const std::string& guid) override;
    DasResult UpdatePluginSettings(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& json_str) override;
    nlohmann::json GetPluginSettingsJson(
        const std::string& profile_id,
        const std::string& guid) override;
    nlohmann::json GetPluginSettingsFieldJson(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name) override;
    DasResult UpdatePluginSettingsFieldJson(
        const std::string&    profile_id,
        const std::string&    guid,
        const std::string&    field_name,
        const nlohmann::json& value) override;

private:
    std::atomic<uint32_t> ref_count_{0};
    SettingsManager&      mgr_;
};

DAS_CORE_SETTINGS_MANAGER_NS_END

#endif // DAS_CORE_SETTINGS_MANAGER_SETTINGSSERVICEIMPL_H
