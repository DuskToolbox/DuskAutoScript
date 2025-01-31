#include "das/ExportInterface/IDasSettings.h"
#include <das/Gateway/Config.h>
#include <das/Gateway/IDasSettingsImpl.h>
#include <das/Gateway/Logger.h>
#include <das/Gateway/ProfileManager.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/FileUtils.hpp>
#include <das/Utils/QueryInterface.hpp>
#include <das/Utils/StreamUtils.hpp>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <fstream>

DAS_GATEWAY_NS_BEGIN

namespace
{
    constexpr auto DAS_GATEWAY_SETTINGS_FILE = u8"settings.json";
    constexpr auto DAS_GATEWAY_SCHEDULER_STATE_FILE = u8"SchedulerState.json";
    constexpr auto DAS_GATEWAY_PROFILE_INFO_FILE = u8"info.json";

    auto GetDataDirectory() -> std::filesystem::path
    {
        auto            result = std::filesystem::current_path() / u8"data";
        std::error_code error;
        if (!Utils::CreateDirectoryRecursive(result, error))
        {
            const auto message = DAS_FMT_NS::format(
                "Failed to create directory {}. Error code = {}.",
                reinterpret_cast<const char*>(result.u8string().c_str()),
                error.value());
            SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
            DAS_GATEWAY_THROW_IF_FAILED(DAS_E_INVALID_FILE)
        }
        return result;
    }
}

DAS_DEFINE_VARIABLE(g_profileManager);

DasResult IDasProfileImpl::QueryInterface(const DasGuid& iid, void** pp_object)
{
    return Utils::QueryInterface<IDasProfile>(this, iid, pp_object);
}

DasResult IDasProfileImpl::GetJsonSettingProperty(
    DasProfileProperty profile_property,
    IDasJsonSetting**  pp_out_json)
{
    if (!pp_out_json)
    {
        SPDLOG_LOGGER_ERROR(g_logger, "pp_out_json is null!");
        return DAS_E_INVALID_POINTER;
    }
    switch (profile_property)
    {
    case DAS_PROFILE_PROPERTY_PROFILE:
        Utils::SetResult(p_settings_, pp_out_json);
        break;
    case DAS_PROFILE_PROPERTY_SCHEDULER_STATE:
        Utils::SetResult(p_scheduler_state_, pp_out_json);
        break;
    default:
        const auto error_message = DAS_FMT_NS::format(
            "Unexpected DasProfileProperty. Value = {}",
            static_cast<int32_t>(profile_property));
        SPDLOG_LOGGER_ERROR(g_logger, error_message.c_str());
        return DAS_E_INVALID_ENUM;
    }
    return DAS_S_OK;
}

DasResult IDasProfileImpl::GetStringProperty(
    DasProfileProperty   profile_property,
    IDasReadOnlyString** pp_out_property)
{
    if (!pp_out_property)
    {
        SPDLOG_LOGGER_ERROR(g_logger, "pp_out_property is null!");
        return DAS_E_INVALID_POINTER;
    }
    switch (profile_property)
    {
    case DAS_PROFILE_PROPERTY_NAME:
        Utils::SetResult(p_name_, pp_out_property);
        break;
    case DAS_PROFILE_PROPERTY_ID:
        Utils::SetResult(p_id_, pp_out_property);
        break;
    default:
        const auto error_message = DAS_FMT_NS::format(
            "Unexpected DasProfileProperty. Value = {}",
            static_cast<int32_t>(profile_property));
        SPDLOG_LOGGER_ERROR(g_logger, error_message.c_str());
        return DAS_E_INVALID_ENUM;
    }
    return DAS_S_OK;
}

DasResult IDasProfileImpl::SetProperty(
    DasProfileProperty profile_property,
    IDasJsonSetting*   p_property)
{
    if (!p_property)
    {
        SPDLOG_LOGGER_ERROR(g_logger, "p_property is null!");
        return DAS_E_INVALID_POINTER;
    }
    switch (profile_property)
    {
    case DAS_PROFILE_PROPERTY_PROFILE:
        p_settings_ = p_property;
        break;
    case DAS_PROFILE_PROPERTY_SCHEDULER_STATE:
        p_scheduler_state_ = p_property;
        break;
    default:
        const auto error_message = DAS_FMT_NS::format(
            "Unknown IDasProfileProperty. Value = {}",
            static_cast<int32_t>(profile_property));
        SPDLOG_LOGGER_ERROR(g_logger, error_message.c_str());
        return DAS_E_INVALID_ENUM;
    }
    return DAS_S_OK;
}

void IDasProfileImpl::SetName(IDasReadOnlyString* p_name) { p_name_ = p_name; }

void IDasProfileImpl::SetId(IDasReadOnlyString* p_id) { p_id_ = p_id; }

ProfileManager::ProfileManager()
{
    try
    {
        const auto data_dir = GetDataDirectory();
        for (const auto& subDirectory :
             std::filesystem::directory_iterator(data_dir))
        {
            auto       profile = MakeDasPtr<IDasProfileImpl>();
            const auto u8_directory_name =
                subDirectory.path().filename().u8string();

            DasPtr<IDasReadOnlyString> p_id{};
            if (const auto create_result =
                    g_pfnCreateIDasReadOnlyStringFromUtf8(
                        reinterpret_cast<const char*>(
                            u8_directory_name.c_str()),
                        p_id.Put());
                DAS::IsFailed(create_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Failed to create IDasReadOnlyString. Error code = {}",
                    create_result);
                SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
                continue;
            }
            profile->SetId(p_id.Get());

            std::ifstream ifs;
            Utils::EnableStreamException(
                ifs,
                std::ios::badbit | std::ios::failbit,
                [json_path = subDirectory.path()
                             / DAS_GATEWAY_PROFILE_INFO_FILE](auto& stream)
                {
                    SPDLOG_LOGGER_ERROR(
                        g_logger,
                        "json_path = {}",
                        reinterpret_cast<const char*>(
                            json_path.u8string().c_str()));
                    stream.open(json_path);
                });
            const auto  info_json = nlohmann::json::parse(ifs);
            std::string name{};
            info_json.at("name").get_to(name);
            DasPtr<IDasReadOnlyString> p_name;
            DAS_GATEWAY_THROW_IF_FAILED(g_pfnCreateIDasReadOnlyStringFromUtf8(
                name.c_str(),
                p_name.Put()))
            profile->SetName(p_name.Get());

            const auto settings_path =
                (subDirectory.path() / DAS_GATEWAY_SETTINGS_FILE).u8string();
            DasPtr<IDasReadOnlyString> p_settings_path{};
            if (const auto create_result =
                    g_pfnCreateIDasReadOnlyStringFromUtf8(
                        reinterpret_cast<const char*>(settings_path.c_str()),
                        p_settings_path.Put());
                DAS::IsFailed(create_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Failed to create IDasReadOnlyString. Error code = {}",
                    create_result);
                SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
                continue;
            }
            auto p_settings = MakeDasPtr<DasSettings>();
            if (const auto load_result =
                    p_settings->LoadSettings(p_settings_path.Get());
                IsFailed(load_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Failed to call LoadSettings. Error code = {}",
                    load_result);
                SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
                continue;
            }
            profile->SetProperty(
                DAS_PROFILE_PROPERTY_PROFILE,
                *p_settings.Get());

            const auto scheduler_state_path =
                (subDirectory.path() / DAS_GATEWAY_SCHEDULER_STATE_FILE)
                    .u8string();
            DasPtr<IDasReadOnlyString> p_scheduler_state_path{};
            if (const auto create_result =
                    g_pfnCreateIDasReadOnlyStringFromUtf8(
                        reinterpret_cast<const char*>(
                            scheduler_state_path.c_str()),
                        p_scheduler_state_path.Put());
                DAS::IsFailed(create_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Failed to create IDasReadOnlyString. Error code = {}",
                    create_result);
                SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
                continue;
            }
            auto p_scheduler_state = MakeDasPtr<DasSettings>();
            if (const auto load_result = p_scheduler_state->LoadSettings(
                    p_scheduler_state_path.Get());
                IsFailed(load_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Failed to call LoadSettings. Error code = {}",
                    load_result);
                SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
                continue;
            }
            profile->SetProperty(
                DAS_PROFILE_PROPERTY_SCHEDULER_STATE,
                *p_scheduler_state.Get());

            profiles_[name] = std::move(profile);
        }
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        SPDLOG_LOGGER_ERROR(g_logger, ex.what());
    }
    catch (const DAS::Core::DasException& ex)
    {
        SPDLOG_LOGGER_ERROR(g_logger, ex.what());
    }
    catch (const std::ios_base::failure& ex)
    {
        SPDLOG_LOGGER_ERROR(g_logger, ex.what());
        const auto& error_code = ex.code();
        SPDLOG_LOGGER_INFO(
            g_logger,
            "Error happened when reading file. Error code = {}",
            error_code.value());
        SPDLOG_LOGGER_INFO(g_logger, error_code.message());
    }
}

DasResult ProfileManager::GetAllIDasProfile(
    size_t         buffer_size,
    IDasProfile*** ppp_out_profile)
{
    if (ppp_out_profile == nullptr)
    {
        return static_cast<DasResult>(profiles_.size());
    }

    if (buffer_size != profiles_.size())
    {
        const auto message = DAS_FMT_NS::format(
            "Profile buffer size not equal to profile size. Expected = {}. Got = {}.",
            profiles_.size(),
            buffer_size);
        SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
        return DAS_E_MAYBE_OVERFLOW;
    }

    for (const auto& profile : profiles_)
    {
        Utils::SetResult(profile.second.Get(), *ppp_out_profile);
        ++ppp_out_profile;
    }

    return DAS_S_OK;
}

DasResult ProfileManager::CreateIDasProfile(
    IDasReadOnlyString* p_profile_id,
    IDasReadOnlyString* p_profile_name,
    IDasReadOnlyString* p_profile_json)
{
    if (!p_profile_id)
    {
        SPDLOG_LOGGER_ERROR(DAS::Gateway::g_logger, "p_profile_id is null!");
        return DAS_E_INVALID_POINTER;
    }

    if (!p_profile_name)
    {
        SPDLOG_LOGGER_ERROR(DAS::Gateway::g_logger, "p_profile_name is null!");
        return DAS_E_INVALID_POINTER;
    }

    if (!p_profile_json)
    {
        SPDLOG_LOGGER_ERROR(DAS::Gateway::g_logger, "p_profile_json is null!");
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto        data_directory = DAS::Gateway::GetDataDirectory();
        const char* profile_id;
        DAS_GATEWAY_THROW_IF_FAILED(p_profile_id->GetUtf8(&profile_id))
        data_directory /= reinterpret_cast<const char8_t*>(profile_id);
        if (std::filesystem::exists(data_directory))
        {
            SPDLOG_LOGGER_ERROR(
                DAS::Gateway::g_logger,
                "Path already exist. Value = {}.",
                reinterpret_cast<const char*>(
                    data_directory.u8string().c_str()));
            return DAS_E_DUPLICATE_ELEMENT;
        }
        if (std::error_code error_code;
            !Utils::CreateDirectoryRecursive(data_directory, error_code))
        {
            SPDLOG_LOGGER_ERROR(
                DAS::Gateway::g_logger,
                "Can not create path {}. Error code = {}.",
                reinterpret_cast<const char*>(
                    data_directory.u8string().c_str()),
                error_code.value());
            SPDLOG_LOGGER_ERROR(
                DAS::Gateway::g_logger,
                "Message = \"{}\".",
                error_code.message());
            return DAS_E_INVALID_FILE;
        }

        const auto u8_path =
            (data_directory / DAS_GATEWAY_SETTINGS_FILE).u8string();
        DasPtr<IDasReadOnlyString> p_path;
        DAS_GATEWAY_THROW_IF_FAILED(g_pfnCreateIDasReadOnlyStringFromUtf8(
            reinterpret_cast<const char*>(u8_path.c_str()),
            p_path.Put()))
        auto p_settings = MakeDasPtr<DasSettings>();
        if (const auto error_code =
                p_settings->InitSettings(p_path.Get(), p_profile_json);
            DAS::IsFailed(error_code))
        {
            return error_code;
        }

        nlohmann::json info;
        const char*    profile_name;
        DAS_GATEWAY_THROW_IF_FAILED(p_profile_id->GetUtf8(&profile_name))
        info["name"] = profile_name;

        const auto info_path = data_directory / DAS_GATEWAY_PROFILE_INFO_FILE;
        std::ofstream ofs{};
        Utils::EnableStreamException(
            ofs,
            std::ios::badbit | std::ios::failbit,
            [&info_path](auto& stream) { stream.open(info_path); });
        ofs << info;
        ofs.flush();

        auto profile = MakeDasPtr<IDasProfileImpl>();
        profile->SetId(p_profile_id);
        profile->SetName(p_profile_name);
        profile->SetProperty(DAS_PROFILE_PROPERTY_PROFILE, *p_settings.Get());

        profiles_[profile_id] = std::move(profile);

        return DAS_S_OK;
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        SPDLOG_LOGGER_ERROR(DAS::Gateway::g_logger, ex.what());
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
    catch (const DAS::Core::DasException& ex)
    {
        SPDLOG_LOGGER_ERROR(DAS::Gateway::g_logger, ex.what());
        return ex.GetErrorCode();
    }
    catch (const std::bad_alloc& ex)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult ProfileManager::FindIDasProfile(
    IDasReadOnlyString* p_name,
    IDasProfile**       pp_out_profile)
{
    if (!p_name)
    {
        SPDLOG_LOGGER_ERROR(g_logger, "p_name is null!");
        return DAS_E_INVALID_POINTER;
    }

    if (!pp_out_profile)
    {
        SPDLOG_LOGGER_ERROR(g_logger, "pp_out_profile is null!");
        return DAS_E_INVALID_POINTER;
    }

    const char* u8_name{};
    if (const auto get_result = p_name->GetUtf8(&u8_name);
        DAS::IsFailed(get_result))
    {
        const auto message = DAS_FMT_NS::format(
            "Failed to get utf8 string. Error code = {}",
            get_result);
        SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
        return get_result;
    }

    if (const auto it = profiles_.find(u8_name); it != profiles_.end())
    {
        Utils::SetResult(it->second.Get(), pp_out_profile);
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DAS_GATEWAY_NS_END

DasResult GetAllIDasProfile(size_t buffer_size, IDasProfile*** ppp_out_profile)
{
    using namespace DAS::Gateway;
    return g_profileManager.GetAllIDasProfile(buffer_size, ppp_out_profile);
}

DasResult CreateIDasProfile(
    IDasReadOnlyString* p_profile_id,
    IDasReadOnlyString* p_profile_name,
    IDasReadOnlyString* p_profile_json)
{
    return DAS::Gateway::g_profileManager.CreateIDasProfile(
        p_profile_id,
        p_profile_name,
        p_profile_json);
}

DasResult DeleteIDasProfile(IDasReadOnlyString* p_profile_id) { return 0; }

DasResult FindIDasProfile(
    IDasReadOnlyString* p_name,
    IDasProfile**       pp_out_profile)
{
    using namespace DAS::Gateway;

    return g_profileManager.FindIDasProfile(p_name, pp_out_profile);
}
