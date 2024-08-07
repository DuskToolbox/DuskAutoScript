#include "AutoStarRail/IAsrBase.h"
#include <AutoStarRail/Core/Exceptions/AsrException.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/SettingsManager/IAsrSettingsImpl.h>
#include <AutoStarRail/Core/Utils/InternalUtils.h>
#include <AutoStarRail/ExportInterface/IAsrSettings.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <AutoStarRail/Utils/StreamUtils.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

// TODO: support plugin set configuration. See
// https://code.visualstudio.com/api/references/contribution-points#contributes.configuration

ASR_CORE_SETTINGSMANAGER_NS_BEGIN

IAsrSettingsForUiImpl::IAsrSettingsForUiImpl(AsrSettings& impl) : impl_{impl} {}

int64_t IAsrSettingsForUiImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrSettingsForUiImpl::Release() { return impl_.Release(); }

ASR_IMPL IAsrSettingsForUiImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_object)
{
    return Utils::QueryInterface<IAsrSettingsForUi>(this, iid, pp_object);
}

ASR_IMPL IAsrSettingsForUiImpl::ToString(IAsrReadOnlyString** pp_out_string)
{
    return impl_.ToString(pp_out_string);
}

ASR_IMPL IAsrSettingsForUiImpl::FromString(IAsrReadOnlyString* p_in_settings)
{
    return impl_.FromString(p_in_settings);
}

ASR_IMPL IAsrSettingsForUiImpl::SaveToWorkingDirectory(
    IAsrReadOnlyString* p_relative_path)
{
    return impl_.SaveToWorkingDirectory(p_relative_path);
}

AsrResult IAsrSettingsForUiImpl::Save() { return impl_.Save(); }

auto AsrSettings::GetKey(const char* p_type_name, const char* key)
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
    if (const auto global_setting_it = default_values_.find(p_type_name);
        global_setting_it != default_values_.end())
    {
        if (const auto setting_it = global_setting_it->find(key);
            setting_it != global_setting_it->end())
        {
            return std::cref(*setting_it);
        }
    }
    return tl::make_unexpected(ASR_E_OUT_OF_RANGE);
}

auto AsrSettings::FindTypeSettings(const char* p_type_name)
    -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>
{
    if (const auto global_setting_it = settings_.find(p_type_name);
        global_setting_it != settings_.end())
    {
        return std::cref(*global_setting_it);
    }
    return tl::make_unexpected(ASR_E_OUT_OF_RANGE);
}

auto AsrSettings::SaveImpl(const std::filesystem::path& full_path) -> AsrResult
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
        return ASR_S_OK;
    }
    catch (const std::ios_base::failure& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        ASR_CORE_LOG_INFO(
            "Error happened when saving settings. Error code = " ASR_STR(
                ASR_E_INVALID_FILE) ".");
        ASR_CORE_LOG_INFO(
            "NOTE: Path = {}.",
            reinterpret_cast<const char*>(full_path.u8string().c_str()));
        return ASR_E_INVALID_FILE;
    }
}

int64_t AsrSettings::AddRef() { return 1; }

int64_t AsrSettings::Release() { return 1; }

AsrResult AsrSettings::ToString(IAsrReadOnlyString** pp_out_string)
{
    ASR_UTILS_CHECK_POINTER(pp_out_string)

    std::lock_guard lock{mutex_};

    try
    {
        auto       json_string = settings_.dump();
        const auto p_result = MakeAsrPtr<AsrStringCppImpl>();
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
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrSettings::FromString(IAsrReadOnlyString* p_in_settings)
{
    ASR_UTILS_CHECK_POINTER(p_in_settings)

    std::lock_guard lock{mutex_};

    try
    {
        const char* p_u8_string{};
        if (const auto get_u8_result = p_in_settings->GetUtf8(&p_u8_string);
            IsFailed(get_u8_result))
        {
            ASR_CORE_LOG_ERROR(
                "Can not get utf8 string from pointer {}.",
                Utils::VoidP(p_in_settings));
            return get_u8_result;
        }
        auto tmp_result = nlohmann::json::parse(p_u8_string);
        settings_ = std::move(tmp_result);
        return ASR_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INTERNAL_FATAL_ERROR;
    }
}

AsrResult AsrSettings::SaveToWorkingDirectory(
    IAsrReadOnlyString* p_relative_path)
{
    ASR_UTILS_CHECK_POINTER(p_relative_path)

    std::filesystem::path path{};
    if (const auto to_path_result = Utils::ToPath(p_relative_path, path);
        IsFailed(to_path_result))
    {
        return to_path_result;
    }
    const auto full_path = std::filesystem::absolute(path);

    return SaveImpl(full_path);
}

AsrResult AsrSettings::Save() { return SaveImpl(path_); }

AsrResult AsrSettings::SetDefaultValues(nlohmann::json&& rv_json)
{
    std::lock_guard lock{mutex_};

    default_values_ = std::move(rv_json);

    return ASR_S_OK;
}

AsrResult AsrSettings::LoadSettings(IAsrReadOnlyString* p_path)
{
    try
    {
        if (p_path == nullptr) [[unlikely]]
        {
            ASR_CORE_LOG_ERROR("Null pointer found! Variable name is p_path."
                               " Please check your code.");
            AsrException::Throw(ASR_E_INVALID_POINTER);
        }

        std::filesystem::path path;
        if (const auto to_path_result = Utils::ToPath(p_path, path);
            IsFailed(to_path_result))
        {
            AsrException::Throw(to_path_result);
        }

        std::ifstream ifs;
        Utils::EnableStreamException(
            ifs,
            std::ios::badbit | std::ios::failbit,
            [&path](auto& stream) { stream.open(path); });
        settings_ = nlohmann::json::parse(ifs);

        return ASR_S_OK;
    }
    catch (const AsrException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ex.GetErrorCode();
    }
    catch (const std::ios_base::failure& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        ASR_CORE_LOG_INFO(
            "Error happened when reading settings file. Error code = " ASR_STR(
                ASR_E_INVALID_FILE) ".");
        return ASR_E_INVALID_FILE;
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        ASR_CORE_LOG_INFO(
            "Error happened when reading settings json. Error code = " ASR_STR(
                ASR_E_INVALID_JSON) ".");
        return ASR_E_INVALID_JSON;
    }
}

AsrSettings::operator IAsrSettingsForUiImpl*() noexcept
{
    return &cpp_projection_for_ui_;
}

ASR_DEFINE_VARIABLE(g_settings);

ASR_CORE_SETTINGSMANAGER_NS_END

ASR_NS_ANONYMOUS_DETAILS_BEGIN

ASR::AsrPtr<IAsrReadOnlyString> g_p_ui_extra_settings_json_string{};

constexpr auto UI_EXTRA_SETTINGS_FILE_NAME = "UiExtraSettings.json";

ASR_NS_ANONYMOUS_DETAILS_END

AsrResult AsrGetGlobalSettings(IAsrSettingsForUi** pp_out_settings)
{
    ASR_UTILS_CHECK_POINTER(pp_out_settings);

    *pp_out_settings = *ASR::Core::SettingsManager::g_settings.Get();
    (*pp_out_settings)->AddRef();
    return ASR_S_OK;
}

AsrResult AsrLoadExtraStringForUi(
    IAsrReadOnlyString** pp_out_ui_extra_settings_json_string)
{
    ASR_UTILS_CHECK_POINTER(pp_out_ui_extra_settings_json_string);

    if (Details::g_p_ui_extra_settings_json_string) [[likely]]
    {
        *pp_out_ui_extra_settings_json_string =
            Details::g_p_ui_extra_settings_json_string.Get();
        return ASR_S_OK;
    }
    try
    {
        std::ifstream extra_string_file{};
        std::string   buffer;
        ASR::Utils::EnableStreamException(
            extra_string_file,
            std::ios::badbit | std::ios::failbit,
            [&buffer](auto& stream)
            {
                stream.open(Details::UI_EXTRA_SETTINGS_FILE_NAME, std::ifstream::in);
                buffer = {
                    (std::istreambuf_iterator<char>(stream)),
                    std::istreambuf_iterator<char>()};
            });
        return ::CreateIAsrReadOnlyStringFromUtf8(
            buffer.c_str(),
            Details::g_p_ui_extra_settings_json_string.Put());
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INTERNAL_FATAL_ERROR;
    }
}

AsrResult AsrSaveExtraStringForUi(
    IAsrReadOnlyString* p_out_ui_extra_settings_json_string)
{
    ASR_UTILS_CHECK_POINTER(p_out_ui_extra_settings_json_string);

    Details::g_p_ui_extra_settings_json_string =
        p_out_ui_extra_settings_json_string;
    const char* p_u8_ui_extra_settings_json_string{};
    if (const auto get_u8_string_result =
            p_out_ui_extra_settings_json_string->GetUtf8(
                &p_u8_ui_extra_settings_json_string);
        ASR::IsFailed(get_u8_string_result))
    {
        ASR_CORE_LOG_ERROR(
            "GetUtf8 failed. Error code = {}",
            get_u8_string_result);
        return get_u8_string_result;
    }

    try
    {
        std::ofstream extra_string_file{};
        ASR::Utils::EnableStreamException(
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
        return ASR_S_OK;
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INTERNAL_FATAL_ERROR;
    }
}

// AsrRetGlobalSettings GetPluginSettings(IAsrSwigTypeInfo* p_plugin)
// {
//     if (p_plugin == nullptr)
//     {
//         ASR_CORE_LOG_ERROR("Nullptr found!");
//         return {ASR_E_INVALID_POINTER, nullptr};
//     }

//     AsrRetGlobalSettings result{};

//     try
//     {
//         const auto type_name =
//             ASR::Core::Utils::GetRuntimeClassNameFrom(p_plugin);

//         const auto expected_result =
//             ASR::Utils::ToU8StringWithoutOwnership(type_name.Get())
//                 .map(
//                     [&](const char* p_u8_name)
//                     {
//                         const auto p_result = ASR::MakeAsrPtr<
//                             IAsrSwigSettings,
//                             ASR::Core::SettingsManager::IAsrSwigSettingsImpl>(
//                             ASR::Core::SettingsManager::g_settings,
//                             p_u8_name);
//                         result.value = p_result.Get();
//                     });
//         result.error_code = ASR::Utils::GetResult(expected_result);
//         return result;
//     }
//     catch (const Asr::Core::AsrException& ex)
//     {
//         ASR_CORE_LOG_EXCEPTION(ex);
//         return {ex.GetErrorCode(), nullptr};
//     }
//     catch (const std::bad_alloc&)
//     {
//         return {ASR_E_OUT_OF_MEMORY, nullptr};
//     }
// }
