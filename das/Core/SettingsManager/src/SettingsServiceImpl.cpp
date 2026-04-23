#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/SettingsServiceImpl.h>
#include <das/DasApi.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <new>

// DasCore-internal: zero-copy IDasJsonImpl creation from nlohmann::json
namespace Das::Core::Utils
{
    DasResult CreateDasJsonFromNlohmann(
        const nlohmann::json&            json,
        Das::ExportInterface::IDasJson** pp_out);
}

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

// ── Helper: extract std::string from IDasReadOnlyString ──

using Das::Utils::ToString;

namespace
{
    std::string GuidToString(const DasGuid& guid)
    {
        constexpr auto fmt_str =
            "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}";
        return DAS_FMT_NS::format(
            fmt_str,
            guid.data1,
            guid.data2,
            guid.data3,
            guid.data4[0],
            guid.data4[1],
            guid.data4[2],
            guid.data4[3],
            guid.data4[4],
            guid.data4[5],
            guid.data4[6],
            guid.data4[7]);
    }

    DasResult JsonToIDasJson(
        const nlohmann::json&            json,
        Das::ExportInterface::IDasJson** pp_out)
    {
        using Das::Core::Utils::CreateDasJsonFromNlohmann;
        return CreateDasJsonFromNlohmann(json, pp_out);
    }

    DasResult IDasJsonToNlohmann(
        Das::ExportInterface::IDasJson* p_data,
        nlohmann::json&                 out)
    {
        DAS_UTILS_CHECK_POINTER(p_data)
        DAS::DasPtr<IDasReadOnlyString> p_str;
        auto result = p_data->ToString(-1, p_str.Put());
        if (DAS::IsFailed(result))
        {
            return result;
        }
        const char* c_str = nullptr;
        auto        get_result = p_str->GetUtf8(&c_str);
        if (DAS::IsFailed(get_result))
        {
            return get_result;
        }
        try
        {
            out = nlohmann::json::parse(c_str);
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_INVALID_JSON;
        }
    }
} // namespace

// ── Global Settings ──

DasResult SettingsServiceImpl::GetGlobalSettings(
    Das::ExportInterface::IDasJson** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    auto json = mgr_.GetGlobalSettingsJson();
    return JsonToIDasJson(json, pp_out);
}

DasResult SettingsServiceImpl::UpdateGlobalSettings(
    Das::ExportInterface::IDasJson* p_data)
{
    nlohmann::json data;
    auto           result = IDasJsonToNlohmann(p_data, data);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    return mgr_.UpdateGlobalSettingsJson(data);
}

// ── Profile management ──

DasResult SettingsServiceImpl::GetProfileList(
    Das::ExportInterface::IDasJson** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    auto json = mgr_.GetProfileListJson();
    return JsonToIDasJson(json, pp_out);
}

DasResult SettingsServiceImpl::CreateProfile(IDasReadOnlyString* p_profile_id)
{
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    return mgr_.CreateProfile(ToString(p_profile_id));
}

DasResult SettingsServiceImpl::DeleteProfile(IDasReadOnlyString* p_profile_id)
{
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    return mgr_.DeleteProfile(ToString(p_profile_id));
}

DasResult SettingsServiceImpl::GetProfile(
    IDasReadOnlyString*              p_profile_id,
    Das::ExportInterface::IDasJson** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    auto json = mgr_.GetProfileJson(ToString(p_profile_id));
    return JsonToIDasJson(json, pp_out);
}

DasResult SettingsServiceImpl::UpdateProfile(
    IDasReadOnlyString*             p_profile_id,
    Das::ExportInterface::IDasJson* p_data)
{
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    nlohmann::json data;
    auto           result = IDasJsonToNlohmann(p_data, data);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    return mgr_.UpdateProfileJson(ToString(p_profile_id), data);
}

// ── Plugin settings ──

DasResult SettingsServiceImpl::GetPluginSettings(
    IDasReadOnlyString*              p_profile_id,
    const DasGuid*                   p_plugin_guid,
    Das::ExportInterface::IDasJson** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    DAS_UTILS_CHECK_POINTER(p_plugin_guid)
    auto [json, status] = mgr_.GetPluginSettingsWithStatus(
        ToString(p_profile_id),
        GuidToString(*p_plugin_guid));
    auto json_result = JsonToIDasJson(json, pp_out);
    if (DAS::IsFailed(json_result))
    {
        return json_result;
    }
    return status;
}

DasResult SettingsServiceImpl::UpdatePluginSettings(
    IDasReadOnlyString*             p_profile_id,
    const DasGuid*                  p_plugin_guid,
    Das::ExportInterface::IDasJson* p_data)
{
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    DAS_UTILS_CHECK_POINTER(p_plugin_guid)
    nlohmann::json data;
    auto           result = IDasJsonToNlohmann(p_data, data);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    return mgr_.UpdatePluginSettingsJson(
        ToString(p_profile_id),
        GuidToString(*p_plugin_guid),
        data);
}

// ── Plugin settings field-level access ──

DasResult SettingsServiceImpl::GetPluginSettingsField(
    IDasReadOnlyString*              p_profile_id,
    const DasGuid*                   p_plugin_guid,
    IDasReadOnlyString*              p_field_name,
    Das::ExportInterface::IDasJson** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    DAS_UTILS_CHECK_POINTER(p_plugin_guid)
    DAS_UTILS_CHECK_POINTER(p_field_name)
    auto json = mgr_.GetPluginSettingsFieldJson(
        ToString(p_profile_id),
        GuidToString(*p_plugin_guid),
        ToString(p_field_name));
    return JsonToIDasJson(json, pp_out);
}

DasResult SettingsServiceImpl::UpdatePluginSettingsField(
    IDasReadOnlyString*             p_profile_id,
    const DasGuid*                  p_plugin_guid,
    IDasReadOnlyString*             p_field_name,
    Das::ExportInterface::IDasJson* p_value)
{
    DAS_UTILS_CHECK_POINTER(p_profile_id)
    DAS_UTILS_CHECK_POINTER(p_plugin_guid)
    DAS_UTILS_CHECK_POINTER(p_field_name)
    nlohmann::json data;
    auto           result = IDasJsonToNlohmann(p_value, data);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    return mgr_.UpdatePluginSettingsFieldJson(
        ToString(p_profile_id),
        GuidToString(*p_plugin_guid),
        ToString(p_field_name),
        data);
}

DAS_CORE_SETTINGS_MANAGER_NS_END
