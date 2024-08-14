#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/PluginInterface/IAsrTask.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <cstring>

AsrRetGuid AsrMakeAsrGuid(const char* p_guid_string)
{
    ASR_CORE_TRACE_SCOPE;

    AsrRetGuid result;
    try
    {
        result.value =
            ASR::Core::ForeignInterfaceHost::MakeAsrGuid(p_guid_string);
        result.error_code = ASR_S_OK;
    }
    catch (const ASR::Core::InvalidGuidStringException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        result = {ASR_E_INVALID_STRING, ASR_IID_BASE};
    }
    catch (const ASR::Core::InvalidGuidStringSizeException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        result = {ASR_E_INVALID_STRING_SIZE, ASR_IID_BASE};
    }
    return result;
}

AsrResult AsrMakeAsrGuid(const char* p_guid_string, AsrGuid* p_out_guid)
{
    ASR_UTILS_CHECK_POINTER(p_out_guid)

    const auto result = AsrMakeAsrGuid(p_guid_string);
    if (result.error_code == ASR_S_OK)
    {
        *p_out_guid = result.value;
        return ASR_S_OK;
    }
    return result.error_code;
}

void* AsrRetSwigBase::GetVoidNoAddRef() const noexcept { return value; }

void AsrRetSwigBase::SetValueAddRef(void* value)
{
    this->value = value;
    InternalAddRef();
}

AsrRetSwigBase::AsrRetSwigBase(AsrResult error_code, void* value)
    : error_code{error_code}, value{value}
{
}

AsrRetSwigBase::AsrRetSwigBase(AsrResult error_code)
    : error_code{error_code}, value{}
{
}

AsrRetSwigBase::AsrRetSwigBase(const AsrRetSwigBase& rhs)
    : error_code{rhs.error_code}, value{rhs.value}
{
    InternalAddRef();
}
AsrRetSwigBase& AsrRetSwigBase::operator=(const AsrRetSwigBase& rhs)
{
    error_code = rhs.error_code;
    value = rhs.value;
    InternalAddRef();
    return *this;
}

AsrRetSwigBase::AsrRetSwigBase(AsrRetSwigBase&& other) noexcept
{
    error_code = std::exchange(other.error_code, ASR_E_UNDEFINED_RETURN_VALUE);
    value = std::exchange(other.value, nullptr);
}

AsrRetSwigBase& AsrRetSwigBase::operator=(AsrRetSwigBase&& other) noexcept
{
    this->error_code =
        std::exchange(other.error_code, ASR_E_UNDEFINED_RETURN_VALUE);
    this->value = std::exchange(other.value, nullptr);
    return *this;
}
