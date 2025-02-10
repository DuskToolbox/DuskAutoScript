#include <cstring>
#include <das/Core/Exceptions/DasException.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/TaskScheduler.h>
#include <das/Gateway/ProfileManager.h>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasTask.h>
#include <das/Utils/CommonUtils.hpp>

DasRetGuid DasMakeDasGuid(const char* p_guid_string)
{
    DAS_CORE_TRACE_SCOPE;

    DasRetGuid result;
    try
    {
        result.value =
            DAS::Core::ForeignInterfaceHost::MakeDasGuid(p_guid_string);
        result.error_code = DAS_S_OK;
    }
    catch (const DAS::Core::InvalidGuidStringException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        result = {DAS_E_INVALID_STRING, DAS_IID_BASE};
    }
    catch (const DAS::Core::InvalidGuidStringSizeException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        result = {DAS_E_INVALID_STRING_SIZE, DAS_IID_BASE};
    }
    return result;
}

DasResult DasMakeDasGuid(const char* p_guid_string, DasGuid* p_out_guid)
{
    DAS_UTILS_CHECK_POINTER(p_out_guid)

    const auto result = DasMakeDasGuid(p_guid_string);
    if (result.error_code == DAS_S_OK)
    {
        *p_out_guid = result.value;
        return DAS_S_OK;
    }
    return result.error_code;
}

void* DasRetSwigBase::GetVoidNoAddRef() const noexcept { return value; }

void DasRetSwigBase::SetValueAddRef(void* value_need_add_ref)
{
    this->value = value_need_add_ref;
    InternalAddRef();
}

DasRetSwigBase::DasRetSwigBase(DasResult error_code, void* value)
    : error_code{error_code}, value{value}
{
}

DasRetSwigBase::DasRetSwigBase(DasResult error_code)
    : error_code{error_code}, value{}
{
}

DasRetSwigBase::DasRetSwigBase(const DasRetSwigBase& rhs)
    : error_code{rhs.error_code}, value{rhs.value}
{
    InternalAddRef();
}
DasRetSwigBase& DasRetSwigBase::operator=(const DasRetSwigBase& rhs)
{
    error_code = rhs.error_code;
    value = rhs.value;
    InternalAddRef();
    return *this;
}

DasRetSwigBase::DasRetSwigBase(DasRetSwigBase&& other) noexcept
{
    error_code = std::exchange(other.error_code, DAS_E_UNDEFINED_RETURN_VALUE);
    value = std::exchange(other.value, nullptr);
}

DasRetSwigBase& DasRetSwigBase::operator=(DasRetSwigBase&& other) noexcept
{
    this->error_code =
        std::exchange(other.error_code, DAS_E_UNDEFINED_RETURN_VALUE);
    this->value = std::exchange(other.value, nullptr);
    return *this;
}

DasResult InitializeDasCore()
{
    try
    {
        DAS::Gateway::InitializeProfileManager();
        return DAS::Core::InitializeGlobalTaskScheduler();
    }
    catch (const DAS::Core::DasException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return ex.GetErrorCode();
    }
}
