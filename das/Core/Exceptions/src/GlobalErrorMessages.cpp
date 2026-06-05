#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/i18n/GlobalLocale.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/DasTypes.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/_autogen/idl/abi/IDasTypeInfo.h>
#include <unordered_map>

DAS_NS_BEGIN

namespace Details
{
    const std::unordered_map<DasResult, const char*>
        g_predefined_error_messages = {
            {DAS_E_INVALID_POINTER, "Invalid pointer"},
            {DAS_E_INVALID_ARGUMENT, "Invalid argument"},
            {DAS_E_OUT_OF_RANGE, "Out of range"},
            {DAS_E_FAIL, "Operation failed"},
            {DAS_E_NO_INTERFACE, "No interface"},
            {DAS_E_NO_IMPLEMENTATION, "No implementation"},
            {DAS_E_OUT_OF_MEMORY, "Out of memory"},
            {DAS_E_INVALID_STRING, "Invalid string"},
            {DAS_E_INVALID_FILE, "Invalid file"},
            {DAS_E_FILE_NOT_FOUND, "File not found"},
            {DAS_E_INVALID_PATH, "Invalid path"},
            {DAS_E_ACCESS_DENIED, "Access denied"},
            {DAS_E_TIMEOUT, "Operation timeout"},
            {DAS_E_INVALID_JSON, "Invalid JSON"},
            {DAS_E_TYPE_ERROR, "Type error"},
            {DAS_E_UNDEFINED_RETURN_VALUE, "Undefined return value"},
            {DAS_E_PYTHON_ERROR, "Python error"},
            {DAS_E_JAVA_ERROR, "Java error"},
            {DAS_E_CSHARP_ERROR, "C# error"},
            {DAS_E_JAVASCRIPT_ERROR, "JavaScript error"},
            {DAS_E_JAVASCRIPT_NO_IMPLEMENTATION,
             "JavaScript callback not implemented"},
            {DAS_E_CSHARP_MISSING_RUNTIMECONFIG, "C# runtimeconfig missing"},
            {DAS_E_CSHARP_UNSUPPORTED_TFM, "C# target framework unsupported"},
            {DAS_E_CSHARP_HOSTFXR_INIT_FAILED,
             "C# hostfxr initialization failed"},
            {DAS_E_CSHARP_COM_CLR_INIT_FAILED,
             "C# COM CLR initialization failed"},
            {DAS_E_CSHARP_ENTRYPOINT_MISSING, "C# entrypoint missing"},
            {DAS_E_CSHARP_PLUGIN_INIT_FAILED,
             "C# plugin initialization failed"},
            {DAS_E_CSHARP_MANIFEST_INVALID, "C# manifest invalid"},
            {DAS_E_CSHARP_ENTRYPOINT_INVALID, "C# entrypoint invalid"},
            {DAS_E_CSHARP_BOOTSTRAP_INVALID, "C# bootstrap invalid"},
            {DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED,
             "C# director factory failed"},
            {DAS_E_CSHARP_NETFX_UNSUPPORTED_PLATFORM,
             "C# .NET Framework runtime unsupported on this platform"},
            {DAS_E_OPENCV_ERROR, "OpenCV error"},
            {DAS_E_ONNX_RUNTIME_ERROR, "ONNX runtime error"},
            {DAS_E_INTERNAL_FATAL_ERROR, "Internal fatal error"},
            {DAS_E_NOT_FOUND, "Not found"},
            {DAS_E_CAPTURE_FAILED, "Capture failed"},
            {DAS_E_ACCESS_DENIED, "Access denied"},
    };
}

DAS_C_API DasResult DasGetPredefinedErrorMessage(
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message)
{
    DAS_UTILS_CHECK_POINTER(pp_out_error_message)

    const auto it = Details::g_predefined_error_messages.find(error_code);
    if (it != Details::g_predefined_error_messages.end())
    {
        return ::CreateIDasReadOnlyStringFromUtf8(
            it->second,
            pp_out_error_message);
    }

    return ::CreateIDasReadOnlyStringFromUtf8(
        "Unknown error",
        pp_out_error_message);
}

DAS_C_API DasResult DasGetErrorMessage(
    IDasTypeInfo*        p_type_info,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message)
{
    DAS_UTILS_CHECK_POINTER(p_type_info)
    DAS_UTILS_CHECK_POINTER(pp_out_error_message)

    if (!DAS::Core::IPC::IsCurrentBusinessThread())
    {
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }

    DasGuid    type_guid{};
    const auto get_guid_result = p_type_info->GetGuid(&type_guid);
    if (Das::IsFailed(get_guid_result))
    {
        return ::DasGetPredefinedErrorMessage(error_code, pp_out_error_message);
    }

    auto* active_manager =
        DAS::Core::ForeignInterfaceHost::GetActiveErrorLensManager();
    if (active_manager != nullptr)
    {
        DasPtr<IDasReadOnlyString> locale_name;
        const auto get_locale_result = ::DasGetDefaultLocale(locale_name.Put());
        if (Das::IsOk(get_locale_result) && locale_name)
        {
            auto plugin_message = active_manager->GetErrorMessage(
                type_guid,
                locale_name.Get(),
                error_code);
            if (plugin_message && plugin_message.value())
            {
                *pp_out_error_message = plugin_message.value().Get();
                (*pp_out_error_message)->AddRef();
                return DAS_S_OK;
            }
        }
    }

    return ::DasGetPredefinedErrorMessage(error_code, pp_out_error_message);
}

DAS_NS_END
