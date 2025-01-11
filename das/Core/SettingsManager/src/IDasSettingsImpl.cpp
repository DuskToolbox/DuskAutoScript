#include "das/IDasBase.h"
#include <boost/filesystem/operations.hpp>
#include <das/Core/Exceptions/DasException.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/IDasSettingsImpl.h>
#include <das/Core/Utils/InternalUtils.h>
#include <das/ExportInterface/IDasSettings.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/FileUtils.hpp>
#include <das/Utils/QueryInterface.hpp>
#include <das/Utils/StreamUtils.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

// TODO: support plugin set configuration. See
// https://code.visualstudio.com/api/references/contribution-points#contributes.configuration

DAS_CORE_SETTINGSMANAGER_NS_BEGIN

IDasSettingsForUiImpl::IDasSettingsForUiImpl(DasSettings& impl) : impl_{impl} {}

int64_t IDasSettingsForUiImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSettingsForUiImpl::Release() { return impl_.Release(); }

DAS_IMPL IDasSettingsForUiImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return Utils::QueryInterface<IDasSettingsForUi>(this, iid, pp_object);
}

DAS_IMPL IDasSettingsForUiImpl::ToString(IDasReadOnlyString** pp_out_string)
{
    return impl_.ToString(pp_out_string);
}

DAS_IMPL IDasSettingsForUiImpl::FromString(IDasReadOnlyString* p_in_settings)
{
    return impl_.FromString(p_in_settings);
}

DAS_IMPL IDasSettingsForUiImpl::SaveToWorkingDirectory(
    IDasReadOnlyString* p_relative_path)
{
    return impl_.SaveToWorkingDirectory(p_relative_path);
}

DasResult IDasSettingsForUiImpl::Save() { return impl_.Save(); }

auto DasSettings::GetKey(const char* p_type_name, const char* key)
    -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>
{
    if (const auto global_setting_it = settings_.find(p_type_name);
        global_setting_it != settings_.end())
    {
        if (const auto setting_it = global_setting_it->find(key);
            setting_it != global_setting_it->end())
        {
            return std::cref(*setting_it);
        }
    }
    return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
}

auto DasSettings::FindTypeSettings(const char* p_type_name)
    -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>
{
    if (const auto global_setting_it = settings_.find(p_type_name);
        global_setting_it != settings_.end())
    {
        return std::cref(*global_setting_it);
    }
    return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
}

auto DasSettings::SaveImpl(const std::filesystem::path& full_path) -> DasResult
{
    std::ofstream ofs{};

    try
    {
        Utils::EnableStreamException(
            ofs,
            std::ios::badbit | std::ios::failbit,
            [&full_path](auto& stream) { stream.open(full_path); });
        std::lock_guard guard{mutex_};
        ofs << settings_;
        ofs.flush();
        return DAS_S_OK;
    }
    catch (const std::ios_base::failure& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_INFO(
            "Error happened when saving settings. Error code = " DAS_STR(
                DAS_E_INVALID_FILE) ".");
        DAS_CORE_LOG_INFO(
            "NOTE: Path = {}.",
            reinterpret_cast<const char*>(full_path.u8string().c_str()));
        return DAS_E_INVALID_FILE;
    }
}

int64_t DasSettings::AddRef() { return 1; }

int64_t DasSettings::Release() { return 1; }

DasResult DasSettings::ToString(IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string)

    std::lock_guard lock{mutex_};

    try
    {
        auto       json_string = settings_.dump();
        const auto p_result = MakeDasPtr<DasStringCppImpl>();
        const auto set_utf_8_result = p_result->SetUtf8(json_string.data());
        if (IsFailed(set_utf_8_result))
        {
            return set_utf_8_result;
        }
        *pp_out_string = p_result.Get();
        p_result->AddRef();
        return set_utf_8_result;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasSettings::FromString(IDasReadOnlyString* p_in_settings)
{
    DAS_UTILS_CHECK_POINTER(p_in_settings)

    std::lock_guard lock{mutex_};

    try
    {
        const char* p_u8_string{};
        if (const auto get_u8_result = p_in_settings->GetUtf8(&p_u8_string);
            IsFailed(get_u8_result))
        {
            DAS_CORE_LOG_ERROR(
                "Can not get utf8 string from pointer {}.",
                Utils::VoidP(p_in_settings));
            return get_u8_result;
        }
        auto tmp_result = nlohmann::json::parse(p_u8_string);
        settings_ = std::move(tmp_result);
        return DAS_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}

DasResult DasSettings::SaveToWorkingDirectory(
    IDasReadOnlyString* p_relative_path)
{
    DAS_UTILS_CHECK_POINTER(p_relative_path)

    std::filesystem::path path{};
    if (const auto to_path_result = Utils::ToPath(p_relative_path, path);
        IsFailed(to_path_result))
    {
        return to_path_result;
    }
    const auto full_path = std::filesystem::absolute(path);

    return SaveImpl(full_path);
}

DasResult DasSettings::Save() { return SaveImpl(path_); }

DasResult DasSettings::LoadSettings(IDasReadOnlyString* p_path)
{
    try
    {
        if (p_path == nullptr) [[unlikely]]
        {
            DAS_CORE_LOG_ERROR("Null pointer found! Variable name is p_path."
                               " Please check your code.");
            DAS_THROW_EC(DAS_E_INVALID_POINTER);
        }

        std::filesystem::path path;
        if (const auto to_path_result = Utils::ToPath(p_path, path);
            IsFailed(to_path_result))
        {
            DAS_THROW_EC(to_path_result);
        }

        std::error_code error_code;
        Utils::CreateDirectoryRecursive(path.parent_path(), error_code);
        if (!error_code)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to create directory {}. Error code = {}.",
                reinterpret_cast<const char*>(path.u8string().c_str()),
                error_code.value());
            return DAS_E_INTERNAL_FATAL_ERROR;
        }

        path_ = path;

        if (!exists(path))
        {
            return DAS_S_OK;
        }

        std::ifstream ifs;
        Utils::EnableStreamException(
            ifs,
            std::ios::badbit | std::ios::failbit,
            [&path](auto& stream) { stream.open(path); });
        settings_ = nlohmann::json::parse(ifs);

        return DAS_S_OK;
    }
    catch (const DasException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return ex.GetErrorCode();
    }
    catch (const std::ios_base::failure& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_INFO(
            "Error happened when reading settings file. Error code = " DAS_STR(
                DAS_E_INVALID_FILE) ".");
        return DAS_E_INVALID_FILE;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_INFO(
            "Error happened when reading settings json. Error code = " DAS_STR(
                DAS_E_INVALID_JSON) ".");
        return DAS_E_INVALID_JSON;
    }
}

DasSettings::operator IDasSettingsForUiImpl*() noexcept
{
    return &cpp_projection_for_ui_;
}

DAS_DEFINE_VARIABLE(g_settings);

DAS_CORE_SETTINGSMANAGER_NS_END

DAS_NS_ANONYMOUS_DETAILS_BEGIN

DAS::DasPtr<IDasReadOnlyString> g_p_ui_extra_settings_json_string{};

constexpr auto UI_EXTRA_SETTINGS_FILE_NAME = "UiExtraSettings.json";

DAS_NS_ANONYMOUS_DETAILS_END

DasResult GetIDasSettingsForUi(IDasSettingsForUi** pp_out_settings)
{
    DAS_UTILS_CHECK_POINTER(pp_out_settings);

    using namespace DAS::Core::SettingsManager;

    if (!g_settings)
    {
        g_settings = DAS::MakeDasPtr<DasSettings>();
        std::error_code error_code;
        auto current_path = std::filesystem::current_path(error_code);
        current_path /= u8"Settings";
        current_path /= u8"CoreSettings.json";
        const DasReadOnlyStringWrapper path{current_path.u8string().c_str()};
        g_settings->LoadSettings(path.Get());
    }
    if (!g_settings)
    {
        DAS_CORE_LOG_ERROR("Nullptr!");
        return DAS_E_INVALID_POINTER;
    }
    DAS::Utils::SetResult(*g_settings, pp_out_settings);
    return DAS_S_OK;
}

DasResult DasLoadExtraStringForUi(
    IDasReadOnlyString** pp_out_ui_extra_settings_json_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_ui_extra_settings_json_string);

    if (Details::g_p_ui_extra_settings_json_string) [[likely]]
    {
        *pp_out_ui_extra_settings_json_string =
            Details::g_p_ui_extra_settings_json_string.Get();
        return DAS_S_OK;
    }
    try
    {
        std::ifstream extra_string_file{};
        std::string   buffer;
        DAS::Utils::EnableStreamException(
            extra_string_file,
            std::ios::badbit | std::ios::failbit,
            [&buffer](auto& stream)
            {
                stream.open(
                    Details::UI_EXTRA_SETTINGS_FILE_NAME,
                    std::ifstream::in);
                buffer = {
                    (std::istreambuf_iterator<char>(stream)),
                    std::istreambuf_iterator<char>()};
            });
        return ::CreateIDasReadOnlyStringFromUtf8(
            buffer.c_str(),
            Details::g_p_ui_extra_settings_json_string.Put());
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}

DasResult DasSaveExtraStringForUi(
    IDasReadOnlyString* p_in_ui_extra_settings_json_string)
{
    DAS_UTILS_CHECK_POINTER(p_in_ui_extra_settings_json_string);

    Details::g_p_ui_extra_settings_json_string =
        p_in_ui_extra_settings_json_string;
    const char* p_u8_ui_extra_settings_json_string{};
    if (const auto get_u8_string_result =
            p_in_ui_extra_settings_json_string->GetUtf8(
                &p_u8_ui_extra_settings_json_string);
        DAS::IsFailed(get_u8_string_result))
    {
        DAS_CORE_LOG_ERROR(
            "GetUtf8 failed. Error code = {}",
            get_u8_string_result);
        return get_u8_string_result;
    }

    try
    {
        std::ofstream extra_string_file{};
        DAS::Utils::EnableStreamException(
            extra_string_file,
            std::ios::badbit | std::ios::failbit,
            [p_u8_ui_extra_settings_json_string](auto& stream)
            {
                stream.open(
                    Details::UI_EXTRA_SETTINGS_FILE_NAME,
                    std::ios::ate | std::ios::out);
                stream << p_u8_ui_extra_settings_json_string;
                stream.close();
            });
        return DAS_S_OK;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}
