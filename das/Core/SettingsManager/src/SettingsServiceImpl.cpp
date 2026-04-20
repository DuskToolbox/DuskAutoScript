#include <das/Core/SettingsManager/SettingsServiceImpl.h>
#include <das/DasExport.h>
#include <new>

DAS_CORE_SETTINGS_MANAGER_NS_BEGIN

SettingsServiceImpl::SettingsServiceImpl(SettingsManager& mgr) : mgr_(mgr) {}

uint32_t DAS_STD_CALL SettingsServiceImpl::AddRef() { return ++ref_count_; }

uint32_t DAS_STD_CALL SettingsServiceImpl::Release()
{
    auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
    }
    return count;
}

DasResult DAS_STD_CALL
SettingsServiceImpl::QueryInterface(const DasGuid& iid, void** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (iid == DasIidOf<IDasBase>())
    {
        *pp_out = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }

    if (iid == DasIidOf<IDasSettingsService>())
    {
        *pp_out = static_cast<IDasSettingsService*>(this);
        AddRef();
        return DAS_S_OK;
    }

    *pp_out = nullptr;
    return DAS_E_NO_INTERFACE;
}

nlohmann::json SettingsServiceImpl::GetGlobalSettings()
{
    return mgr_.GetGlobalSettingsJson();
}

DasResult SettingsServiceImpl::UpdateGlobalSettings(const nlohmann::json& data)
{
    return mgr_.UpdateGlobalSettingsJson(data);
}

nlohmann::json SettingsServiceImpl::GetProfileList()
{
    return mgr_.GetProfileListJson();
}

DasResult SettingsServiceImpl::CreateProfile(const std::string& profile_id)
{
    return mgr_.CreateProfile(profile_id);
}

DasResult SettingsServiceImpl::DeleteProfile(const std::string& profile_id)
{
    return mgr_.DeleteProfile(profile_id);
}

nlohmann::json SettingsServiceImpl::GetProfile(const std::string& profile_id)
{
    return mgr_.GetProfileJson(profile_id);
}

DasResult SettingsServiceImpl::UpdateProfile(
    const std::string&    profile_id,
    const nlohmann::json& data)
{
    return mgr_.UpdateProfileJson(profile_id, data);
}

nlohmann::json SettingsServiceImpl::GetPluginSettings(
    const std::string& profile_id,
    const std::string& guid)
{
    return mgr_.GetPluginSettingsJson(profile_id, guid);
}

DasResult SettingsServiceImpl::UpdatePluginSettings(
    const std::string&    profile_id,
    const std::string&    guid,
    const nlohmann::json& data)
{
    return mgr_.UpdatePluginSettingsJson(profile_id, guid, data);
}

nlohmann::json SettingsServiceImpl::GetPluginSettingsField(
    const std::string& profile_id,
    const std::string& guid,
    const std::string& field_name)
{
    return mgr_.GetPluginSettingsFieldJson(profile_id, guid, field_name);
}

DasResult SettingsServiceImpl::UpdatePluginSettingsField(
    const std::string&    profile_id,
    const std::string&    guid,
    const std::string&    field_name,
    const nlohmann::json& value)
{
    return mgr_.UpdatePluginSettingsFieldJson(profile_id, guid, field_name, value);
}

DAS_CORE_SETTINGS_MANAGER_NS_END

DAS_C_API DasResult CreateDasSettingsService(
    Das::Core::SettingsManager::SettingsManager& mgr,
    IDasSettingsService**                        pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::SettingsManager::SettingsServiceImpl(mgr);
        impl->AddRef();
        *pp_out = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
