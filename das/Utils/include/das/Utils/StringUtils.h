#ifndef DAS_UTILS_STRINGUTILS_HPP
#define DAS_UTILS_STRINGUTILS_HPP

#include <das/Utils/Config.h>
#include <das/Utils/Expected.h>
#include <das/Utils/fmt.h>
#include <filesystem>
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

/**
 * @brief IDasReadOnlyString 内容相等谓词，配合 DasReadOnlyStringHash 用作
 * unordered_map 的 KeyEqual。同指针（含两者皆 nullptr）为 true；仅一方为
 * nullptr 为 false；否则调用 IDasReadOnlyString::Equals 比较内容。
 */
struct DasReadOnlyStringEqual
{
    bool operator()(
        const DasPtr<IDasReadOnlyString>& lhs,
        const DasPtr<IDasReadOnlyString>& rhs) const noexcept;
};

/**
 * @brief Extract std::string from IDasReadOnlyString*
 * Returns empty string on null pointer or failure.
 */
inline std::string ToString(IDasReadOnlyString* p_str)
{
    if (!p_str)
    {
        return {};
    }
    const char* c_str = nullptr;
    auto        result = p_str->GetUtf8(&c_str);
    if (DAS::IsFailed(result) || !c_str)
    {
        return {};
    }
    return std::string(c_str);
}

/**
 * @brief Zero-overhead reinterpret of std::u8string_view as std::string_view.
 *
 * Usage (read-only, no ownership):
 *   auto u8 = path.u8string();
 *   SomeFunc(U8AsString(u8));
 *
 * The returned string_view borrows from the u8string argument.
 * The u8string must outlive the returned string_view.
 */
[[nodiscard]]
inline std::string_view U8AsString(std::u8string_view sv) noexcept
{
    return {reinterpret_cast<const char*>(sv.data()), sv.size()};
}

/**
 * @brief Owning wrapper that stores a std::u8string and exposes char-based
 * accessors.
 *
 * Only accepts rvalue std::u8string (zero-copy move).
 * Use when you need ownership of the UTF-8 path string:
 *
 *   U8String u8{path.u8string()};   // OK: rvalue, zero-copy move
 *   auto s = path.u8string();
 *   U8String u8{s};                  // Error: lvalue, deleted
 *   U8String u8{std::move(s)};       // OK: explicit move
 *
 * Private constructors (const std::u8string&, const std::string&) are
 * accessible only to the free function ToString(std::string_view).
 */
class U8String
{
public:
    U8String() = default;

    // Accept rvalue only — zero-copy move
    explicit U8String(std::u8string&& str) noexcept : value_(std::move(str)) {}

    // Reject lvalue — forces explicit std::move() at call site
    explicit U8String(const std::u8string&) = delete;

    U8String(const U8String&) noexcept = default;
    U8String(U8String&&) noexcept = default;
    U8String& operator=(const U8String&) noexcept = default;
    U8String& operator=(U8String&&) noexcept = default;
    ~U8String() = default;

    [[nodiscard]]
    const char* c_str() const noexcept
    {
        return reinterpret_cast<const char*>(value_.c_str());
    }

    [[nodiscard]]
    const char* data() const noexcept
    {
        return reinterpret_cast<const char*>(value_.data());
    }

    [[nodiscard]]
    std::size_t size() const noexcept
    {
        return value_.size();
    }

    [[nodiscard]]
    bool empty() const noexcept
    {
        return value_.empty();
    }

    [[nodiscard]]
    std::string_view string_view() const noexcept
    {
        return {reinterpret_cast<const char*>(value_.data()), value_.size()};
    }

    [[nodiscard]]
    operator std::string_view() const noexcept
    {
        return string_view();
    }

private:
    // Private constructor from native-encoded std::string (assumes UTF-8 bytes)
    explicit U8String(const std::string& str) noexcept
        : value_(reinterpret_cast<const char8_t*>(str.data()), str.size())
    {
    }

    // Grant access to ToString(std::string_view)
    friend U8String ToString(std::string_view str);

    std::u8string value_;
};

DAS_UTILS_NS_END

constexpr char operator""_das_as_char(char8_t c) { return c; }

template <DAS::Utils::char8_t_string_literal L>
constexpr auto& operator""_das_as_char()
{
    return DAS::Utils::make_as_char_buffer<L>(
        std::make_index_sequence<decltype(L)::size>());
}

// Convert native-encoded string (e.g., Windows GBK) to UTF-8 encoded U8String
// Uses std::filesystem::path for the encoding conversion
inline DAS::Utils::U8String ToString(std::string_view str)
{
    return DAS::Utils::U8String{std::filesystem::path{str}.u8string()};
}

template <>
struct DAS_FMT_NS::formatter<DAS::Utils::U8String, char>
    : public formatter<std::string_view, char>
{
    auto format(const DAS::Utils::U8String& str, format_context& ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator
    {
        return formatter<std::string_view, char>::format(
            str.string_view(),
            ctx);
    }
};

#endif // DAS_UTILS_STRINGUTILS_HPP