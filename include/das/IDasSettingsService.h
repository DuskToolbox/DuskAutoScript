#ifndef DAS_SETTINGS_SERVICE_H
#define DAS_SETTINGS_SERVICE_H

#include <das/IDasBase.h>

typedef void (*SettingsNotifyFunc)(const char* json, void* user_data);

namespace Das::ExportInterface
{
    DAS_INTERFACE IDasJson;
} // namespace Das::ExportInterface

struct IDasReadOnlyString;

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
    DAS_METHOD GetGlobalSettings(Das::ExportInterface::IDasJson * *pp_out) = 0;
    DAS_METHOD UpdateGlobalSettings(Das::ExportInterface::IDasJson * p_data) =
        0;

    // Profile management
    DAS_METHOD GetProfileList(Das::ExportInterface::IDasJson * *pp_out) = 0;
    DAS_METHOD CreateProfile(
        IDasReadOnlyString * p_profile_id,
        IDasReadOnlyString * p_name) = 0;
    DAS_METHOD DeleteProfile(IDasReadOnlyString * p_profile_id) = 0;
    DAS_METHOD RenameProfile(
        IDasReadOnlyString * p_profile_id,
        IDasReadOnlyString * p_name) = 0;
    DAS_METHOD GetProfile(
        IDasReadOnlyString * p_profile_id,
        Das::ExportInterface::IDasJson * *pp_out) = 0;
    DAS_METHOD UpdateProfile(
        IDasReadOnlyString * p_profile_id,
        Das::ExportInterface::IDasJson * p_data) = 0;

    // Plugin settings (routed by plugin GUID)
    DAS_METHOD GetPluginSettings(
        IDasReadOnlyString * p_profile_id,
        const DasGuid*                   p_plugin_guid,
        Das::ExportInterface::IDasJson** pp_out) = 0;
    DAS_METHOD UpdatePluginSettings(
        IDasReadOnlyString * p_profile_id,
        const DasGuid*                  p_plugin_guid,
        Das::ExportInterface::IDasJson* p_data) = 0;

    // Plugin settings field-level access
    DAS_METHOD GetPluginSettingsField(
        IDasReadOnlyString * p_profile_id,
        const DasGuid*                   p_plugin_guid,
        IDasReadOnlyString*              p_field_name,
        Das::ExportInterface::IDasJson** pp_out) = 0;
    DAS_METHOD UpdatePluginSettingsField(
        IDasReadOnlyString * p_profile_id,
        const DasGuid*                  p_plugin_guid,
        IDasReadOnlyString*             p_field_name,
        Das::ExportInterface::IDasJson* p_value) = 0;
    DAS_METHOD SetSettingsNotifyCallback(
        SettingsNotifyFunc func,
        void*              user_data) = 0;
};

#endif // DAS_SETTINGS_SERVICE_H
