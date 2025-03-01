#include <boost/filesystem/operations.hpp>
#include <das/Core/Exceptions/DasException.h>
#include <das/ExportInterface/IDasSettings.h>
#include <das/Gateway/IDasSettingsImpl.h>
#include <das/Gateway/Logger.h>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/FileUtils.hpp>
#include <das/Utils/QueryInterface.hpp>
#include <das/Utils/StreamUtils.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

DAS_NS_ANONYMOUS_DETAILS_BEGIN

DasResult ToPath(
    IDasReadOnlyString*    p_string,
    std::filesystem::path& ref_out_path)
{
    if (p_string == nullptr)
    {
        SPDLOG_LOGGER_ERROR(DAS::Gateway::GetLogger(), "p_string is nullptr.");
        return DAS_E_INVALID_POINTER;
    }
#ifdef DAS_WINDOWS
    const wchar_t* w_path;
    const auto     get_result = p_string->GetW(&w_path);
    if (DAS::IsFailed(get_result))
    {
        return get_result;
    }
    ref_out_path = std::filesystem::path{w_path};
#else
    const char* u8_path;
    const auto  get_result = p_string->GetUtf8(&u8_path);
    if (DAS::IsFailed(get_result))
    {
        return get_result;
    }
    ref_out_path = std::filesystem::path{u8_path};
#endif // DAS_WINDOWS
    return get_result;
}

DAS::DasPtr<IDasReadOnlyString> g_p_ui_extra_settings_json_string{};

constexpr auto UI_EXTRA_SETTINGS_FILE_NAME = "UiExtraSettings.json";

DAS_NS_ANONYMOUS_DETAILS_END

// TODO: support plugin set configuration. See
// https://code.visualstudio.com/api/references/contribution-points#contributes.configuration

DAS_GATEWAY_NS_BEGIN

IDasJsonSettingImpl::IDasJsonSettingImpl(DasSettings& impl) : impl_{impl} {}

int64_t IDasJsonSettingImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasJsonSettingImpl::Release() { return impl_.Release(); }

DAS_IMPL IDasJsonSettingImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    if (Utils::QueryInterface<IDasJsonSetting>(this, iid, pp_object)
        == DAS_E_NO_INTERFACE)
    {
        if (iid == DasIidOf<DasSettings>())
        {
            impl_.AddRef();
            *pp_object = &impl_;
            return DAS_S_OK;
        }
    }
    return DAS_E_NO_INTERFACE;
}

DAS_IMPL IDasJsonSettingImpl::ToString(IDasReadOnlyString** pp_out_string)
{
    return impl_.ToString(pp_out_string);
}

DAS_IMPL IDasJsonSettingImpl::FromString(IDasReadOnlyString* p_in_settings)
{
    return impl_.FromString(p_in_settings);
}

DAS_IMPL IDasJsonSettingImpl::SaveToWorkingDirectory(
    IDasReadOnlyString* p_relative_path)
{
    return impl_.SaveToWorkingDirectory(p_relative_path);
}

DasResult IDasJsonSettingImpl::Save() { return impl_.Save(); }

DasResult IDasJsonSettingImpl::SetOnDeletedHandler(
    IDasJsonSettingOnDeletedHandler* p_handler)
{
    return impl_.SetOnDeletedHandler(p_handler);
}

DasResult IDasJsonSettingImpl::ExecuteAtomically(
    IDasJsonSettingOperator* p_operator)
{
    return impl_.ExecuteAtomically(p_operator);
}

auto DasSettings::SaveImpl(const std::filesystem::path& full_path) -> DasResult
{
    std::ofstream ofs{};

    try
    {
        Utils::EnableStreamException(
            ofs,
            std::ios::badbit | std::ios::failbit,
            [&full_path](auto& stream)
            {
                stream.open(full_path);
                stream.width(2);
            });
        DasPtr<IDasReadOnlyString> p_json_string{};
        {
            std::lock_guard guard{mutex_};
            DAS_GATEWAY_THROW_IF_FAILED(
                settings_->ToString(ofs.width(0), p_json_string.Put()))
        }
        const char* p_u8_json_string{nullptr};
        DAS_GATEWAY_THROW_IF_FAILED(p_json_string->GetUtf8(&p_u8_json_string))
        ofs << p_u8_json_string;
        ofs.flush();
        return DAS_S_OK;
    }
    catch (const std::ios_base::failure& ex)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), ex.what());
        SPDLOG_LOGGER_INFO(
            GetLogger(),
            "Error happened when saving settings. Error code = " DAS_STR(
                DAS_E_INVALID_FILE) ".");
        const auto message = DAS_FMT_NS::format(
            "NOTE: Path = {}.",
            reinterpret_cast<const char*>(full_path.u8string().c_str()));
        return DAS_E_INVALID_FILE;
    }
    catch (const DAS::Core::DasException& ex)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), ex.what());
        return ex.GetErrorCode();
    }
}

int64_t DasSettings::AddRef() { return ref_counter_.AddRef(); }

int64_t DasSettings::Release() { return ref_counter_.Release(this); }

DasResult DasSettings::ToString(IDasReadOnlyString** pp_out_string)
{
    if (!pp_out_string)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), "pp_out_string is nullptr.");
        return DAS_E_INVALID_POINTER;
    }

    std::lock_guard lock{mutex_};

    return settings_->ToString(0, pp_out_string);
}

DasResult DasSettings::FromString(IDasReadOnlyString* p_in_settings)
{
    if (!p_in_settings)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), "p_in_settings is nullptr.");
        return DAS_E_INVALID_POINTER;
    }

    const char* p_u8_string{};
    if (const auto get_u8_result = p_in_settings->GetUtf8(&p_u8_string);
        IsFailed(get_u8_result))
    {
        auto message = DAS_FMT_NS::format(
            "Can not get utf8 string. Error code = {}",
            get_u8_result);
        SPDLOG_LOGGER_ERROR(GetLogger(), message.c_str());
        message = DAS_FMT_NS::format("Note: text = {}", p_u8_string);
        SPDLOG_LOGGER_INFO(GetLogger(), message.c_str());
        return get_u8_result;
    }

    std::lock_guard lock{mutex_};
    return GetParseDasJsonFromStringFunction()(p_u8_string, settings_.Put());
}

DasResult DasSettings::SaveToWorkingDirectory(
    IDasReadOnlyString* p_relative_path)
{
    if (p_relative_path == nullptr)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), "p_relative_path is nullptr.");
        return DAS_E_INVALID_POINTER;
    }

    std::filesystem::path path{};
    if (const auto to_path_result = Details::ToPath(p_relative_path, path);
        IsFailed(to_path_result))
    {
        return to_path_result;
    }
    const auto full_path = std::filesystem::absolute(path);

    return SaveImpl(full_path);
}

DasResult DasSettings::Save() { return SaveImpl(path_); }

DasResult DasSettings::SetOnDeletedHandler(
    IDasJsonSettingOnDeletedHandler* p_handler)
{
    p_handler_ = p_handler;
    return DAS_S_OK;
}

DasResult DasSettings::ExecuteAtomically(IDasJsonSettingOperator* p_operator)
{
    if (p_operator == nullptr) [[unlikely]]
    {
        SPDLOG_LOGGER_ERROR(
            GetLogger(),
            "Null pointer found! Variable name is p_operator."
            " Please check your code.");
        return DAS_E_INVALID_POINTER;
    }

    DasPtr          p_holder{p_operator};
    std::lock_guard _{mutex_};
    return p_operator->Apply(settings_.Get());
}

DasResult DasSettings::LoadSettings(IDasReadOnlyString* p_path)
{
    try
    {
        if (p_path == nullptr) [[unlikely]]
        {
            SPDLOG_LOGGER_ERROR(
                GetLogger(),
                "Null pointer found! Variable name is p_path."
                " Please check your code.");
            return DAS_E_INVALID_POINTER;
        }

        std::filesystem::path path;
        if (const auto to_path_result = Details::ToPath(p_path, path);
            IsFailed(to_path_result))
        {
            const auto message = DAS_FMT_NS::format(
                "Call ToPath failed. Error code = {}.",
                to_path_result);
            SPDLOG_LOGGER_ERROR(GetLogger(), message.c_str());
        }

        std::error_code error_code;
        if (!Utils::CreateDirectoryRecursive(path.parent_path(), error_code))
        {
            const auto message = DAS_FMT_NS::format(
                "Failed to create directory {}. Error code = {}.",
                reinterpret_cast<const char*>(path.u8string().c_str()),
                error_code.value());
            SPDLOG_LOGGER_ERROR(GetLogger(), message.c_str());
            SPDLOG_LOGGER_ERROR(
                GetLogger(),
                "Message = \"{}\".",
                error_code.message());
            return DAS_E_INTERNAL_FATAL_ERROR;
        }

        path_ = path;

        if (!exists(path))
        {
            const auto message = DAS_FMT_NS::format(
                "Path not exists. File will be create. Path = {}.",
                reinterpret_cast<const char*>(path.u8string().c_str()));
            SPDLOG_LOGGER_WARN(GetLogger(), message.c_str());
            settings_ = {};
            return DAS_S_FALSE;
        }

        std::ifstream ifs;
        Utils::EnableStreamException(
            ifs,
            std::ios::badbit | std::ios::failbit,
            [&path](auto& stream) { stream.open(path); });
        std::string ifs_string(
            std::istreambuf_iterator<char>{ifs},
            std::istreambuf_iterator<char>{});
        return GetParseDasJsonFromStringFunction()(
            ifs_string.c_str(),
            settings_.Put());
    }
    catch (const std::ios_base::failure& ex)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), ex.what());
        SPDLOG_LOGGER_ERROR(
            GetLogger(),
            "Error happened when reading settings file. Error code = " DAS_STR(
                DAS_E_INVALID_FILE) ".");
        return DAS_E_INVALID_FILE;
    }
}

DasSettings::operator IDasJsonSettingImpl*() noexcept
{
    return &cpp_projection_for_ui_;
}

void DasSettings::Delete()
{
    if (settings_ != nullptr)
    {
        p_handler_->OnDeleted();
    }
}

DasResult DasSettings::InitSettings(
    IDasReadOnlyString* p_path,
    IDasReadOnlyString* p_json_string)
{
    if (!p_path)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), "p_path is nullptr.");
        return DAS_E_INVALID_POINTER;
    }

    std::filesystem::path path{};
    if (const auto to_path_result = Details::ToPath(p_path, path);
        IsFailed(to_path_result))
    {
        return to_path_result;
    }

    path_ = std::move(path);

    try
    {

        const char* json_string{};
        DAS_GATEWAY_THROW_IF_FAILED(p_json_string->GetUtf8(&json_string))
        DAS_GATEWAY_THROW_IF_FAILED(
            GetParseDasJsonFromStringFunction()(json_string, settings_.Put()))

        return Save();
    }
    catch (const nlohmann::json::exception& ex)
    {
        SPDLOG_LOGGER_ERROR(
            GetLogger(),
            "Parse json failed. Id = {}. What = {}",
            ex.id,
            ex.what());
        const char* json_string{};
        p_json_string->GetUtf8(&json_string);
        SPDLOG_LOGGER_ERROR(
            GetLogger(),
            "json = {}",
            json_string ? json_string : "nullptr");
        return DAS_E_INVALID_JSON;
    }
    catch (const DAS::Core::DasException& ex)
    {
        SPDLOG_LOGGER_ERROR(GetLogger(), ex.what());
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}

DasResult DasSettings::OnDeleted()
{
    if (!p_handler_)
    {
        // 回调不存在，无需执行，认为成功
        return DAS_S_OK;
    }
    return p_handler_->OnDeleted();
}

DAS_DEFINE_VARIABLE(g_settings);

DAS_GATEWAY_NS_END

DasResult DasLoadExtraStringForUi(
    IDasReadOnlyString** pp_out_ui_extra_settings_json_string)
{
    if (pp_out_ui_extra_settings_json_string == nullptr)
    {
        SPDLOG_LOGGER_ERROR(
            DAS::Gateway::GetLogger(),
            "pp_out_ui_extra_settings_json_string is nullptr.");
        return DAS_E_INVALID_POINTER;
    }

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
        return DAS::Gateway::GetCreateIDasReadOnlyStringFromUtf8Function()(
            buffer.c_str(),
            Details::g_p_ui_extra_settings_json_string.Put());
    }
    catch (const std::exception& ex)
    {
        SPDLOG_LOGGER_ERROR(DAS::Gateway::GetLogger(), ex.what());
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}

DasResult DasSaveExtraStringForUi(
    IDasReadOnlyString* p_in_ui_extra_settings_json_string)
{
    if (p_in_ui_extra_settings_json_string == nullptr)
    {
        SPDLOG_LOGGER_ERROR(
            DAS::Gateway::GetLogger(),
            "p_in_ui_extra_settings_json_string is nullptr.");
        return DAS_E_INVALID_POINTER;
    }

    Details::g_p_ui_extra_settings_json_string =
        p_in_ui_extra_settings_json_string;
    const char* p_u8_ui_extra_settings_json_string{};
    if (const auto get_u8_string_result =
            p_in_ui_extra_settings_json_string->GetUtf8(
                &p_u8_ui_extra_settings_json_string);
        DAS::IsFailed(get_u8_string_result))
    {
        const auto message = DAS_FMT_NS::format(
            "GetUtf8 failed. Error code = {}",
            get_u8_string_result);
        SPDLOG_LOGGER_ERROR(DAS::Gateway::GetLogger(), message.c_str());
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
        SPDLOG_LOGGER_ERROR(DAS::Gateway::GetLogger(), ex.what());
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}
