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
    ~SettingsServiceImpl() = default;

    // IDasBase
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out) override;

    // IDasSettingsService
    DAS_IMPL GetGlobalSettings(
        Das::ExportInterface::IDasJson** pp_out) override;
    DAS_IMPL UpdateGlobalSettings(
        Das::ExportInterface::IDasJson* p_data) override;

    DAS_IMPL GetProfileList(Das::ExportInterface::IDasJson** pp_out) override;
    DAS_IMPL CreateProfile(
        IDasReadOnlyString* p_profile_id,
        IDasReadOnlyString* p_name) override;
    DAS_IMPL DeleteProfile(IDasReadOnlyString* p_profile_id) override;
    DAS_IMPL RenameProfile(
        IDasReadOnlyString* p_profile_id,
        IDasReadOnlyString* p_name) override;
    DAS_IMPL GetProfile(
        IDasReadOnlyString*              p_profile_id,
        Das::ExportInterface::IDasJson** pp_out) override;
    DAS_IMPL UpdateProfile(
        IDasReadOnlyString*             p_profile_id,
        Das::ExportInterface::IDasJson* p_data) override;

    DAS_IMPL GetPluginSettings(
        IDasReadOnlyString*              p_profile_id,
        const DasGuid*                   p_plugin_guid,
        Das::ExportInterface::IDasJson** pp_out) override;
    DAS_IMPL UpdatePluginSettings(
        IDasReadOnlyString*             p_profile_id,
        const DasGuid*                  p_plugin_guid,
        Das::ExportInterface::IDasJson* p_data) override;

    DAS_IMPL GetPluginSettingsField(
        IDasReadOnlyString*              p_profile_id,
        const DasGuid*                   p_plugin_guid,
        IDasReadOnlyString*              p_field_name,
        Das::ExportInterface::IDasJson** pp_out) override;
    DAS_IMPL UpdatePluginSettingsField(
        IDasReadOnlyString*             p_profile_id,
        const DasGuid*                  p_plugin_guid,
        IDasReadOnlyString*             p_field_name,
        Das::ExportInterface::IDasJson* p_value) override;

    DAS_IMPL SetSettingsNotifyCallback(SettingsNotifyFunc func, void* user_data)
        override;

private:
    std::atomic<uint32_t> ref_count_{0};
    SettingsManager&      mgr_;
};

DAS_CORE_SETTINGS_MANAGER_NS_END

#endif // DAS_CORE_SETTINGS_MANAGER_SETTINGSSERVICEIMPL_H
