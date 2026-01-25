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

decltype(&::CreateIDasReadOnlyStringFromUtf8)
GetCreateIDasReadOnlyStringFromUtf8Function()
{
    static decltype(&::CreateIDasReadOnlyStringFromUtf8) result{
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
                SPDLOG_LOGGER_ERROR(GetLogger(), message.c_str());
                SPDLOG_LOGGER_ERROR(GetLogger(), ex.what());
            }
            return nullptr;
        }()};
    return result;
}

decltype(&::ParseDasJsonFromString) GetParseDasJsonFromStringFunction()
{
    static decltype(&::ParseDasJsonFromString) result{
        []() -> decltype(&::ParseDasJsonFromString)
        {
            try
            {
                auto       das_core = Details::GetDasCore();
                const auto result =
                    das_core.get<decltype(::ParseDasJsonFromString)>(
                        "ParseDasJsonFromString");
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
                SPDLOG_LOGGER_ERROR(GetLogger(), message.c_str());
                SPDLOG_LOGGER_ERROR(GetLogger(), ex.what());
            }
            return nullptr;
        }()};
    return result;
}

DAS_GATEWAY_NS_END