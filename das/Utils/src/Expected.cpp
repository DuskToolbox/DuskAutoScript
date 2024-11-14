#include <das/Utils/Expected.h>
#include <das/Utils/CommonUtils.hpp>

DAS_UTILS_NS_BEGIN

DAS_DEFINE_VARIABLE(Details::NULL_STRING){""};

ErrorAndExplanation::ErrorAndExplanation(const DasResult error_code) noexcept
    : error_code{error_code}
{
}

DAS_UTILS_NS_END

auto DAS_FMT_NS::formatter<DAS::Utils::VariantString, char>::format(
    const DAS::Utils::VariantString& string,
    format_context&                  ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    const char* p_string_data = std::visit(
        DAS::Utils::overload_set{
            [](const char* string_in_variant) { return string_in_variant; },
            // match std::string_view and std::string
            [](const std::string_view string_in_variant)
            { return string_in_variant.data(); },
            [](const DasReadOnlyString string_in_variant)
            { return string_in_variant.GetUtf8(); }},
        string);
    return DAS_FMT_NS::format_to(ctx.out(), "{}", p_string_data);
}
