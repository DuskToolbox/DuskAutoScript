#ifndef DAS_UTILS_STRINGUTILS_HPP
#define DAS_UTILS_STRINGUTILS_HPP

#include <das/Utils/Config.h>
#include <das/Utils/Expected.h>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#define DAS_UTILS_STRINGUTILS_COMPARE_STRING(var, string_literals)             \
    [&var]() -> bool                                                           \
    {                                                                          \
        constexpr auto rhs = string_literals;                                  \
        constexpr auto w_rhs = DAS_WSTR(string_literals);                      \
        return DAS::Utils::Compare(var, std::make_tuple(rhs, w_rhs));          \
    }()

// reference from
// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2513r4.html
#if defined(__cpp_char8_t)
#define DAS_UTILS_STRINGUTILS_DEFINE_U8STR_IMPL(x) u8##x##_das_as_char
#define DAS_UTILS_STRINGUTILS_DEFINE_U8STR(x)                                  \
    DAS_UTILS_STRINGUTILS_DEFINE_U8STR_IMPL(x)
#else
#define DAS_UTILS_STRINGUTILS_DEFINE_U8STR_IMPL(x) u8##x
#define DAS_UTILS_STRINGUTILS_DEFINE_U8STR(x)                                  \
    DAS_UTILS_STRINGUTILS_DEFINE_U8STR_IMPL(x)
#define
#endif // defined(__cpp_char8_t)

DAS_UTILS_NS_BEGIN

template <std::size_t N>
struct char8_t_string_literal
{
    static constexpr inline std::size_t size = N;

    template <std::size_t... I>
    constexpr char8_t_string_literal(
        const char8_t (&r)[N],
        std::index_sequence<I...>)
        : s{r[I]...}
    {
    }

    constexpr char8_t_string_literal(const char8_t (&r)[N])
        : char8_t_string_literal(r, std::make_index_sequence<N>())
    {
    }

    auto operator<=>(const char8_t_string_literal&) const noexcept = default;

    char8_t s[N];
};

template <char8_t_string_literal L, std::size_t... I>
constexpr inline const char as_char_buffer[sizeof...(I)] = {
    static_cast<char>(L.s[I])...};

template <char8_t_string_literal L, std::size_t... I>
constexpr auto& make_as_char_buffer(std::index_sequence<I...>)
{
    return as_char_buffer<L, I...>;
}

template <class T>
concept is_method_c_str_existence = requires(const T& t) { t.c_str(); };

template <is_method_c_str_existence T>
bool Compare(const T& lhs, std::tuple<const char*, const wchar_t*> rhs)
{
    using CharT = std::remove_cv_t<
        std::remove_reference_t<std::remove_pointer_t<decltype(lhs.c_str())>>>;
    const auto lhs_view = std::basic_string_view<CharT>{lhs.c_str()};
    const auto rhs_value = std::get<const CharT*>(rhs);
    const auto rhs_view = decltype(lhs_view){rhs_value};
    return lhs_view == rhs_view;
}

void ToLowerInPlace(std::string& in_out_str);

auto ToUpper(const std::string_view in_string) -> std::string;

void ToLowerInPlace(std::vector<std::string>& in_out_str_vector);

struct DasReadOnlyStringHash
{
    std::size_t operator()(IDasReadOnlyString* p_string) const noexcept;
    std::size_t operator()(
        const DasPtr<IDasReadOnlyString>& das_ro_string) const noexcept;
};

DAS_UTILS_NS_END

constexpr char operator""_das_as_char(char8_t c) { return c; }

template <DAS::Utils::char8_t_string_literal L>
constexpr auto& operator""_das_as_char()
{
    return DAS::Utils::make_as_char_buffer<L>(
        std::make_index_sequence<decltype(L)::size>());
}

#endif // DAS_UTILS_STRINGUTILS_HPP
