#include "das/DasPtr.hpp"
#include "das/DasString.hpp"
#include "spdlog/common.h"
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/_autogen/idl/abi/DasLogger.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.DasLogger.hpp>
#include <optional>

namespace DC = DAS::Core;

DAS_NS_ANONYMOUS_DETAILS_BEGIN

std::optional<spdlog::source_loc> ToSpdlogSourceLocation(
    const DAS::ExportInterface::DasSourceLocation& location)
{
    spdlog::source_loc result;
    result.filename = location.FileName().GetUtf8();
    result.line = location.Line();
    result.funcname = location.FunctionName().GetUtf8();

    return result;
}

DAS_NS_ANONYMOUS_DETAILS_END

void DasLogError(IDasReadOnlyString* p_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string_holder{p_string};
    const char*                     p_u8_string{};

    p_string_holder->GetUtf8(&p_u8_string);
    DasLogErrorU8(p_u8_string);
}

void DasLogErrorU8(const char* p_string) { DC::g_logger->info(p_string); }

void DasLogErrorU8WithSourceLocation(
    const char*                               p_string,
    DAS::ExportInterface::IDasSourceLocation* p_location)
{
    if (const auto opt_location = Details::ToSpdlogSourceLocation(p_location))
    {
        DC::g_logger->log(opt_location.value(), spdlog::level::err, p_string);
    }
    else
    {
        DasLogErrorU8(p_string);
    }
}

// ----------------------------------------------------------------

void DasLogWarning(IDasReadOnlyString* p_string)
{
    DAS::DasPtr<IDasReadOnlyString> p_string_holder{p_string};
    const char*                     p_u8_string{};

    p_string_holder->GetUtf8(&p_u8_string);
    DasLogWarningU8(p_u8_string);
}

void DasLogWarningU8(const char* p_string) { DC::g_logger->warn(p_string); }

void DasLogWarningU8WithSourceLocation(
    const char*                               p_string,
    DAS::ExportInterface::IDasSourceLocation* p_location)
{
    if (const auto opt_location = Details::ToSpdlogSourceLocation(p_location))
    {
        if (opt_location)
        {
            DC::g_logger->log(
                opt_location.value(),
                spdlog::level::warn,
                p_string);
        }
        else
        {
            DasLogErrorU8(p_string);
        }
    }
}

// ----------------------------------------------------------------

void DasLogInfo(IDasReadOnlyString* p_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string_holder{p_string};
    const char*                     p_u8_string{};

    p_string_holder->GetUtf8(&p_u8_string);
    DasLogInfoU8(p_u8_string);
}

void DasLogInfoU8(const char* p_string) { DC::g_logger->info(p_string); }

void DasLogInfoU8WithSourceLocation(
    const char*                               p_string,
    DAS::ExportInterface::IDasSourceLocation* p_location)
{
    if (const auto opt_location = Details::ToSpdlogSourceLocation(p_location))
    {
        DC::g_logger->log(opt_location.value(), spdlog::level::info, p_string);
    }
    else
    {
        DasLogErrorU8(p_string);
    }
}

// ----------------------------------------------------------------

void DasLogError(DasReadOnlyString das_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string{};
    das_string.GetImpl(p_string.Put());
    DasLogError(p_string.Get());
}

void DasLogWarning(DasReadOnlyString das_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string{};
    das_string.GetImpl(p_string.Put());
    DasLogWarning(p_string.Get());
}

void DasLogInfo(DasReadOnlyString das_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string{};
    das_string.GetImpl(p_string.Put());
    DasLogInfo(p_string.Get());
}