#ifndef DAS_GATEWAY_PROFILEMANAGER_H
#define DAS_GATEWAY_PROFILEMANAGER_H

#include <das/ExportInterface/IDasSettings.h>
#include <das/Gateway/Config.h>
#include <das/Utils/CommonUtils.hpp>
#include <unordered_map>

DAS_GATEWAY_NS_BEGIN

class IDasProfileImpl : public IDasProfile
{
public:
    // IDasBase
    DAS_UTILS_IDASBASE_AUTO_IMPL(IDasProfileImpl)
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasProfile
    DasResult GetJsonSettingProperty(
        DasProfileProperty profile_property,
        IDasJsonSetting**  pp_out_json) override;
    DasResult GetStringProperty(
        DasProfileProperty   profile_property,
        IDasReadOnlyString** pp_out_property) override;
    // IDasProfileImpl
    DasResult SetJsonSettingProperty(
        DasProfileProperty profile_property,
        IDasJsonSetting*   p_property);
    void SetName(IDasReadOnlyString* p_name);
    void SetId(IDasReadOnlyString* p_id);
    void OnDeleted();

private:
    DasPtr<IDasReadOnlyString> p_name_;
    DasPtr<IDasReadOnlyString> p_id_;
    DasPtr<IDasJsonSetting>    p_settings_;
    DasPtr<IDasJsonSetting>    p_scheduler_state_;
};

class ProfileManager
{
public:
    ProfileManager();

    DasResult GetAllIDasProfile(
        size_t         buffer_size,
        IDasProfile*** ppp_out_profile);
    DasResult CreateIDasProfile(
        IDasReadOnlyString* p_profile_id,
        IDasReadOnlyString* p_profile_name,
        IDasReadOnlyString* p_profile_json);
    DasResult DeleteIDasProfile(IDasReadOnlyString* p_profile_id);
    DasResult FindIDasProfile(
        IDasReadOnlyString* p_profile_id,
        IDasProfile**       pp_out_profile);

private:
    std::unordered_map<std::string, DasPtr<IDasProfileImpl>> profiles_;
};

extern std::optional<ProfileManager> g_profileManager;

/**
 * @brief 直接抛异常，外面自己接
 */
DAS_GATEWAY_API void InitializeProfileManager();

DAS_GATEWAY_NS_END

#endif // DAS_GATEWAY_PROFILEMANAGER_H
