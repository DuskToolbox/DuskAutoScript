#ifndef DAS_UTILS_STRINGUTILS_HPP
#define DAS_UTILS_STRINGUTILS_HPP

#include <das/Utils/Config.h>
#include <das/Utils/Expected.h>
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

namespace Details
{
    // U8String: A wrapper class for UTF-8 encoded strings
    // Provides safe handling and conversion of UTF-8 string data
    // with proper move semantics and explicit conversions
    class U8String
    {
    public:
        // Default constructor
        U8String() = default;

        // Constructor from std::u8string (copy)
        explicit U8String(const std::u8string& str) noexcept : value(str) {}

        // Constructor from std::u8string (move)
        explicit U8String(std::u8string&& str) noexcept : value(std::move(str))
        {
        }

        // Constructor from std::string (assumes UTF-8 encoding)
        // Note: Caller must ensure input is valid UTF-8
        explicit U8String(const std::string& str) noexcept
            : value(reinterpret_cast<const char8_t*>(str.data()), str.size())
        {
        }

        // Copy constructor (defaulted)
        U8String(const U8String&) noexcept = default;

        // Move constructor (defaulted)
        U8String(U8String&&) noexcept = default;

        // Copy assignment operator (defaulted)
        U8String& operator=(const U8String&) noexcept = default;

        // Move assignment operator (defaulted)
        U8String& operator=(U8String&&) noexcept = default;

        // Destructor (defaulted)
        ~U8String() = default;

        // Get the underlying std::u8string (const reference)
        [[nodiscard]]
        const std::u8string& Get() const noexcept
        {
            return value;
        }

        // Get C-style string pointer (null-terminated)
        [[nodiscard]]
        const char* CStr() const noexcept
        {
            return reinterpret_cast<const char*>(value.c_str());
        }

        // Get string view for efficient read-only access
        [[nodiscard]]
        std::string_view StringView() const noexcept
        {
            return {reinterpret_cast<const char*>(value.data()), value.size()};
        }

        // Convert to std::string (reinterprets bytes)
        [[nodiscard]]
        std::string ToString() const noexcept
        {
            return std::string(
                reinterpret_cast<const char*>(value.data()),
                value.size());
        }

        // Check if string is empty
        [[nodiscard]]
        bool Empty() const noexcept
        {
            return value.empty();
        }

        // Get string size
        [[nodiscard]]
        std::size_t Size() const noexcept
        {
            return value.size();
        }

        // Clear the string
        void Clear() noexcept { value.clear(); }

        // Swap with another U8String
        void Swap(U8String& other) noexcept { value.swap(other.value); }

        // Equality comparison
        [[nodiscard]]
        bool operator==(const U8String& other) const noexcept
        {
            return value == other.value;
        }

        [[nodiscard]]
        bool operator!=(const U8String& other) const noexcept
        {
            return value != other.value;
        }

        // Comparison with std::u8string
        [[nodiscard]]
        bool operator==(const std::u8string& other) const noexcept
        {
            return value == other;
        }

        [[nodiscard]]
        bool operator!=(const std::u8string& other) const noexcept
        {
            return value != other;
        }

        // Comparison with char8_t pointer
        [[nodiscard]]
        bool operator==(const char8_t* other) const noexcept
        {
            return value == (other ? other : u8"");
        }

        [[nodiscard]]
        bool operator!=(const char8_t* other) const noexcept
        {
            return value != (other ? other : u8"");
        }

    private:
        std::u8string value;
    };

    // Non-member swap function for ADL support
    inline void swap(U8String& lhs, U8String& rhs) noexcept { lhs.Swap(rhs); }
}

// Convert string to UTF-8 encoded U8String
// Handles local encoding (e.g., Windows GBK) to UTF-8 conversion
inline Details::U8String ToString(std::string_view str)
{
    return Details::U8String{std::filesystem::path{str}.u8string()};
}

#endif // DAS_UTILS_STRINGUTILS_HPP
