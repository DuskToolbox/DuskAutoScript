#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/Config.h>
#include <das/Core/Utils/DasJsonSettingImpl.h>
#include <das/Utils/CommonUtils.hpp>
#include <filesystem>
#include <fstream>

// Forward declarations — defined in DasJsonImpl.cpp
DasResult ParseDasJsonFromString(
    const char*                      p_u8_string,
    Das::ExportInterface::IDasJson** pp_out_json);

DasResult CloneDasJsonFromCopy(
    const nlohmann::json&            src,
    Das::ExportInterface::IDasJson** pp_out_json);

DasResult ExtractNlohmannJson(
    Das::ExportInterface::IDasJson* p_json,
    nlohmann::json&                 out);

// {56E5529D-C4EB-498D-BFAA-EFFEA20EB02A}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Utils,
    DasJsonSettingImpl,
    0x56e5529d,
    0xc4eb,
    0x498d,
    0xbf,
    0xaa,
    0xef,
    0xfe,
    0xa2,
    0x0e,
    0xb0,
    0x2a);

DAS_CORE_UTILS_NS_BEGIN

DasJsonSettingImpl::DasJsonSettingImpl(const std::filesystem::path& file_path)
    : file_path_{file_path}
{
    if (!std::filesystem::exists(file_path_))
    {
        return;
    }

    try
    {
        std::ifstream ifs{file_path_};
        if (!ifs.is_open())
        {
            return;
        }
        json_ = nlohmann::json::parse(ifs);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
    }
}

DasJsonSettingImpl::~DasJsonSettingImpl()
{
    if (on_deleted_handler_)
    {
        on_deleted_handler_->OnDeleted();
    }
}

DasResult DasJsonSettingImpl::ToString(IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string)

    try
    {
        std::shared_lock<std::shared_mutex> lock{mutex_};
        const auto                          str = json_.dump();
        return ::CreateIDasReadOnlyStringFromUtf8(str.c_str(), pp_out_string);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasJsonSettingImpl::FromString(IDasReadOnlyString* p_in_settings)
{
    DAS_UTILS_CHECK_POINTER(p_in_settings)

    const auto expected_str = ToU8StringWithoutOwnership(p_in_settings);
    if (!expected_str)
    {
        return expected_str.error();
    }

    try
    {
        std::unique_lock<std::shared_mutex> lock{mutex_};
        json_ = nlohmann::json::parse(expected_str.value());
        return DAS_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasJsonSettingImpl::SaveLocked()
{
    if (file_path_.empty())
    {
        return DAS_E_INVALID_PATH;
    }

    try
    {
        if (file_path_.has_parent_path())
        {
            std::filesystem::create_directories(file_path_.parent_path());
        }
        std::ofstream ofs{file_path_};
        if (!ofs.is_open())
        {
            return DAS_E_INVALID_FILE;
        }
        ofs << json_.dump(4);
        return DAS_S_OK;
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_FILE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasJsonSettingImpl::Save()
{
    std::unique_lock<std::shared_mutex> lock{mutex_};
    return SaveLocked();
}

DasResult DasJsonSettingImpl::SaveToWorkingDirectory(
    IDasReadOnlyString* p_relative_path)
{
    DAS_UTILS_CHECK_POINTER(p_relative_path)

    const auto expected_str = ToU8StringWithoutOwnership(p_relative_path);
    if (!expected_str)
    {
        return expected_str.error();
    }

    try
    {
        std::unique_lock<std::shared_mutex> lock{mutex_};
        file_path_ = std::filesystem::current_path() / expected_str.value();
        return SaveLocked();
    }
    catch (const std::filesystem::filesystem_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_PATH;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasJsonSettingImpl::SetOnDeletedHandler(
    Das::ExportInterface::IDasJsonSettingOnDeletedHandler* p_handler)
{
    on_deleted_handler_ =
        DasPtr<Das::ExportInterface::IDasJsonSettingOnDeletedHandler>{
            p_handler};
    if (p_handler)
    {
        p_handler->AddRef();
    }
    return DAS_S_OK;
}

DasResult DasJsonSettingImpl::ExecuteAtomically(
    Das::ExportInterface::IDasJsonSettingOperator* p_operator)
{
    DAS_UTILS_CHECK_POINTER(p_operator)

    try
    {
        std::unique_lock<std::shared_mutex> lock{mutex_};

        Das::DasPtr<Das::ExportInterface::IDasJson> p_json;
        const auto clone_result = CloneDasJsonFromCopy(json_, p_json.Put());
        if (DAS::IsFailed(clone_result))
        {
            return clone_result;
        }

        const auto apply_result = p_operator->Apply(p_json.Get());
        if (DAS::IsFailed(apply_result))
        {
            return apply_result;
        }

        nlohmann::json modified;
        const auto extract_result = ExtractNlohmannJson(p_json.Get(), modified);
        if (DAS::IsFailed(extract_result))
        {
            return extract_result;
        }

        json_ = std::move(modified);
        return DAS_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_CORE_UTILS_NS_END
