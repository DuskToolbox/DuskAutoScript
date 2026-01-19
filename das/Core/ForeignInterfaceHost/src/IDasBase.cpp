#include <DAS/_autogen/idl/abi/IDasTask.h>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasException.hpp>
#include <das/IDasBase.h>
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
    catch (const InvalidGuidStringException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        result = {DAS_E_INVALID_STRING, DAS_IID_BASE};
    }
    catch (const InvalidGuidStringSizeException& ex)
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

DasResult InitializeDasCore()
{
    try
    {
        // DAS::Gateway::InitializeProfileManager();
        // return DAS::Core::InitializeGlobalTaskScheduler();
    }
    catch (const DasException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return ex.GetErrorCode();
    }
    return DAS_S_OK;
}
