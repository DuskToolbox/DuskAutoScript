#ifndef DAS_UTILS_EXPECTED_H
#define DAS_UTILS_EXPECTED_H

#include <das/DasString.hpp>
#include <das/Utils/Config.h>
#include <das/Utils/fmt.h>
#include <tl/expected.hpp>
#include <type_traits>
#include <variant>

#define ASTR_UTILS_LOG_ON_ERROR                                                \
    [](const auto& unexpected) { DAS_CORE_LOG_ERROR(unexpected.explanation); }

DAS_UTILS_NS_BEGIN

using VariantString =
    std::variant<DasReadOnlyString, std::string, const char*, std::string_view>;

namespace Details
{
    extern const char* const NULL_STRING;
}

struct ErrorAndExplanation
{
    explicit ErrorAndExplanation(const DasResult error_code) noexcept;
    template <class T>
    explicit ErrorAndExplanation(const DasResult error_code, T&& explanation)
        : error_code{error_code}, explanation{std::forward<T>(explanation)}
    {
    }

    DasResult     error_code;
    VariantString explanation{Details::NULL_STRING};
};

template <class... Args>
auto MakeUnexpected(Args&&... args)
{
    return tl::make_unexpected(std::forward<Args>(args)...);
}

template <class T>
using ExpectedWithExplanation = tl::expected<T, ErrorAndExplanation>;

template <class T>
using Expected = tl::expected<T, DasResult>;

template <class T>
auto Map(T&& object) -> Expected<T>
{
    return std::forward<T>(object);
}

template <class T>
DasResult GetResult(const Expected<T>& expected_result)
{
    if (expected_result.has_value())
    {
        return DAS_S_OK;
    }
    return expected_result.error();
}

DAS_UTILS_NS_END

template <>
struct DAS_FMT_NS::formatter<DAS::Utils::VariantString, char>
    : public formatter<const char*, char>
{
    auto format(const DAS::Utils::VariantString& string, format_context& ctx)
        const -> typename std::remove_reference_t<decltype(ctx)>::iterator;
};

using DASE = DAS::Utils::ErrorAndExplanation;

#endif // DAS_UTILS_EXPECTED_H
