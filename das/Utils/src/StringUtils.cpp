#include <das/Utils/StringUtils.h>
#include <algorithm>

DAS_UTILS_NS_BEGIN

void ToLowerInPlace(std::string& in_out_str)
{
    std::transform(
        in_out_str.begin(),
        in_out_str.end(),
        in_out_str.begin(),
        [](unsigned char c) { return std::tolower(c); });
}

void ToLowerInPlace(std::vector<std::string>& in_out_str_vector)
{
    for (auto& str : in_out_str_vector)
    {
        ToLowerInPlace(str);
    }
}

auto ToUpper(const std::string_view in_string) -> std::string
{
    std::string result{in_string};
    std::transform(
        DAS_FULL_RANGE_OF(result),
        result.begin(),
        [](const auto c) { return std::toupper(c); });
    return result;
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

[[nodiscard]]
std::size_t BkdrHash(const char16_t* p_begin, const char16_t* p_end)
{
    constexpr std::size_t seed = 31;
    std::size_t           hash = 0;
    while (p_begin != p_end)
    {
        hash = hash * seed + *p_begin;
        std::advance(p_begin, 1);
    }
    return hash;
}

DAS_NS_ANONYMOUS_DETAILS_END

std::size_t DasReadOnlyStringHash::operator()(
    IDasReadOnlyString* p_string) const noexcept
{
    if (p_string == nullptr) [[unlikely]]
    {
        return 0;
    }

    const char16_t* p_u16{nullptr};
    size_t          string_size{0};
    const auto      result = p_string->GetUtf16(&p_u16, &string_size);
    const auto      string_size_int64 = static_cast<int64_t>(string_size);
    if (DAS::IsOk(result)) [[likely]]
    {
        return Details::BkdrHash(p_u16, std::next(p_u16, string_size_int64));
    }
    return std::hash<decltype(nullptr)>{}(nullptr);
}

std::size_t DasReadOnlyStringHash::operator()(
    const DasPtr<IDasReadOnlyString>& asr_ro_string) const noexcept
{
    const auto p_impl = asr_ro_string.Get();
    return (*this)(p_impl);
}

DAS_UTILS_NS_END
