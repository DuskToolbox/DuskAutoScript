#include "das/DasPtr.hpp"
#include "das/DasString.hpp"
#include "spdlog/common.h"
#include <das/ExportInterface/DasLogger.h>
#include <das/Core/Logger/Logger.h>
#include <optional>

namespace AC = DAS::Core;

DAS_NS_ANONYMOUS_DETAILS_BEGIN

std::optional<spdlog::source_loc> ToSpdlogSourceLocation(
    const DasSourceLocation* p_location)
{
    if (p_location == nullptr) [[unlikely]]
    {
        DAS_CORE_LOG_ERROR(
            "Received a null pointer of type DasSourceLocation.");

        return std::nullopt;
    }

    spdlog::source_loc result;
    result.filename = p_location->file_name;
    result.line = p_location->line;
    result.funcname = p_location->function_name;

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

void DasLogErrorU8(const char* p_string) { AC::g_logger->info(p_string); }

void DasLogErrorU8WithSourceLocation(
    const char*              p_string,
    const DasSourceLocation* p_location)
{
    const auto opt_location = Details::ToSpdlogSourceLocation(p_location);
    if (opt_location)
    {
        AC::g_logger->log(opt_location.value(), spdlog::level::err, p_string);
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

void DasLogWarningU8(const char* p_string) { AC::g_logger->warn(p_string); }

void DasLogWarningU8WithSourceLocation(
    const char*              p_string,
    const DasSourceLocation* p_location)
{
    const auto opt_location = Details::ToSpdlogSourceLocation(p_location);
    if (opt_location)
    {
        AC::g_logger->log(opt_location.value(), spdlog::level::warn, p_string);
    }
    else
    {
        DasLogErrorU8(p_string);
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

void DasLogInfoU8(const char* p_string) { AC::g_logger->info(p_string); }

void DasLogInfoU8WithSourceLocation(
    const char*              p_string,
    const DasSourceLocation* p_location)
{
    const auto opt_location = Details::ToSpdlogSourceLocation(p_location);
    if (opt_location)
    {
        AC::g_logger->log(opt_location.value(), spdlog::level::info, p_string);
    }
    else
    {
        DasLogErrorU8(p_string);
    }
}

// ----------------------------------------------------------------

void DasLogError(DasReadOnlyString asr_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string{};
    asr_string.GetImpl(p_string.Put());
    DasLogError(p_string.Get());
}

void DasLogWarning(DasReadOnlyString asr_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string{};
    asr_string.GetImpl(p_string.Put());
    DasLogWarning(p_string.Get());
}

void DasLogInfo(DasReadOnlyString asr_string)
{
    Das::DasPtr<IDasReadOnlyString> p_string{};
    asr_string.GetImpl(p_string.Put());
    DasLogInfo(p_string.Get());
}