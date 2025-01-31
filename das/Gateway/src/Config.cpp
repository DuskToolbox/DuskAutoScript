#include <das/Gateway/Config.h>

#include <boost/dll.hpp>
#include <das/Gateway/Logger.h>
#include <das/Utils/fmt.h>

DAS_GATEWAY_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetDasCore() -> boost::dll::shared_library
{
    static boost::dll::shared_library result{DAS_CORE_DLL};
    return result;
}

DAS_NS_ANONYMOUS_DETAILS_END

DAS_DEFINE_VARIABLE(g_pfnCreateIDasReadOnlyStringFromUtf8){
    []() -> decltype(&::CreateIDasReadOnlyStringFromUtf8)
    {
        try
        {
            auto       das_core = Details::GetDasCore();
            const auto result =
                das_core.get<decltype(::CreateIDasReadOnlyStringFromUtf8)>(
                    "CreateIDasReadOnlyStringFromUtf8");
            return result;
        }
        catch (const boost::dll::fs::system_error& ex)
        {
            const auto code = ex.code();
            const auto message = DAS_FMT_NS::format(
                "Can not load library " DAS_CORE_DLL
                " .Error code = {}. Message = {}",
                code.value(),
                code.message());
            SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
            SPDLOG_LOGGER_ERROR(g_logger, ex.what());
        }
        return nullptr;
    }()};

DAS_DEFINE_VARIABLE(g_pfnThrowDasExceptionEc){
    []() -> decltype(g_pfnThrowDasExceptionEc)
    {
        try
        {
            auto       das_core = Details::GetDasCore();
            const auto result = das_core.get_alias<
                std::remove_pointer_t<decltype(g_pfnThrowDasExceptionEc)>>(
                "ThrowDasExceptionEc");
            return result;
        }
        catch (const boost::dll::fs::system_error& ex)
        {
            const auto code = ex.code();
            const auto message = DAS_FMT_NS::format(
                "Can not load library " DAS_CORE_DLL
                " .Error code = {}. Message = {}",
                code.value(),
                code.message());
            SPDLOG_LOGGER_ERROR(g_logger, message.c_str());
            SPDLOG_LOGGER_ERROR(g_logger, ex.what());
        }
        return nullptr;
    }()};

DAS_GATEWAY_NS_END