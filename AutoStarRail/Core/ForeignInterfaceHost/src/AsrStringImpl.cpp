#include <AutoStarRail/AsrConfig.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <cstring>
#include <magic_enum_format.hpp>
#include <new>
#include <nlohmann/json.hpp>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

bool operator==(AsrReadOnlyString lhs, AsrReadOnlyString rhs)
{
    const char* p_lhs_str = lhs.GetUtf8();
    const char* p_rhs_str = rhs.GetUtf8();

    return std::strcmp(p_lhs_str, p_rhs_str) == 0;
}

std::size_t std::hash<AsrReadOnlyString>::operator()(
    const AsrReadOnlyString& string) const noexcept
{
    auto* const p_string = string.Get();
    if (p_string != nullptr) [[likely]]
    {
        const char16_t* p_u16_string{nullptr};
        size_t          length{0};
        p_string->GetUtf16(&p_u16_string, &length);
        if (p_u16_string != nullptr) [[likely]]
        {
            return boost::hash_unordered_range(
                p_u16_string,
                p_u16_string + length);
        }
        return std::hash<void*>{}(p_string);
    }
    return std::hash<void*>{}(p_string);
}

auto(ASR_FMT_NS::formatter<ASR::AsrPtr<IAsrReadOnlyString>, char>::format)(
    const ASR::AsrPtr<IAsrReadOnlyString>& p_string,
    format_context&                        ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    if (!p_string) [[unlikely]]
    {
        ASR_CORE_LOG_ERROR("Null ASR::AsrPtr<IAsrReadOnlyString> found!");
        return ctx.out();
    }

    const char* p_string_data{nullptr};
    const auto  result = p_string->GetUtf8(&p_string_data);
    if (ASR::IsOk(result))
    {
        return ASR_FMT_NS::format_to(ctx.out(), "{}", p_string_data);
    }
    return ASR_FMT_NS::format_to(
        ctx.out(),
        "(An error occurred when getting string, with error code = {})",
        result);
}

auto(ASR_FMT_NS::formatter<AsrReadOnlyString, char>::format)(
    const AsrReadOnlyString& asr_string,
    format_context&          ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    if (!asr_string.Get()) [[unlikely]]
    {
        ASR_CORE_LOG_ERROR("Null AsrString found!");
        return ctx.out();
    }
    return ASR_FMT_NS::format_to(ctx.out(), "{}", asr_string.GetUtf8());
}

ASR_UTILS_NS_BEGIN

AsrResult ToPath(
    IAsrReadOnlyString*    p_string,
    std::filesystem::path& ref_out_path)
{
    ASR_UTILS_CHECK_POINTER(p_string);
#ifdef ASR_WINDOWS
    const wchar_t* w_path;
    const auto     get_result = p_string->GetW(&w_path);
    if (ASR::IsFailed(get_result))
    {
        return get_result;
    }
    ref_out_path = std::filesystem::path{w_path};
#else
    const char* u8_path;
    const auto  get_result = p_string->GetUtf8(&u8_path);
    if (ASR::IsFailed(get_result))
    {
        return get_result;
    }
    ref_out_path = std::filesystem::path{u8_path};
#endif // ASR_WINDOWS
    return get_result;
}
auto ToU8StringWithoutOwnership(IAsrReadOnlyString* p_string)
    -> Expected<const char*>
{
    const char* result{nullptr};
    if (const auto get_u8_string_result = p_string->GetUtf8(&result);
        IsFailed(get_u8_string_result))
    {
        ASR_CORE_LOG_ERROR(
            "GetUtf8 failed with error code = {}.",
            get_u8_string_result);
        return tl::make_unexpected(get_u8_string_result);
    }

    return result;
}

auto ToU8String(IAsrReadOnlyString* p_string) -> Expected<std::string>
{
    const char* p_u8_string{nullptr};
    if (const auto get_u8_string_result = p_string->GetUtf8(&p_u8_string);
        IsFailed(get_u8_string_result))
    {
        ASR_CORE_LOG_ERROR(
            "GetUtf8 failed with error code = {}.",
            get_u8_string_result);
        return tl::make_unexpected(get_u8_string_result);
    }

    return std::string{p_u8_string};
}

ASR_UTILS_NS_END

bool ASR::AsrStringLess::operator()(
    const ASR::AsrPtr<IAsrReadOnlyString>& lhs,
    const ASR::AsrPtr<IAsrReadOnlyString>& rhs) const
{
    const char16_t* p_lhs{};
    const char16_t* p_rhs{};
    size_t          lhs_size{};
    size_t          rhs_size{};
    lhs->GetUtf16(&p_lhs, &lhs_size);
    rhs->GetUtf16(&p_rhs, &rhs_size);

    return ::u_strCompare(
               p_lhs,
               static_cast<int32_t>(lhs_size),
               p_rhs,
               static_cast<int32_t>(rhs_size),
               false)
           < 0;
}

void AsrStringCppImpl::InvalidateCache()
{
    is_cache_expired_ = {true, true, true};
}

void AsrStringCppImpl::UpdateUtf32Cache()
{
    if (IsCacheExpired<Encode::U32>())
    {
        const auto  u32_char_count = impl_.countChar32();
        auto* const p_cached_utf32_string =
            cached_utf32_string_.DiscardAndGetNullTerminateBufferPointer(
                u32_char_count);
        UErrorCode error_code = U_ZERO_ERROR;
        impl_.toUTF32(p_cached_utf32_string, u32_char_count, error_code);
        if (error_code != U_ZERO_ERROR)
        {
            ASR_CORE_LOG_ERROR(
                "Error happened when calling UnicodeString::toUTF32. Error code: {}",
                error_code);
        }
        ValidateCache<Encode::U32>();
    }
}

AsrStringCppImpl::AsrStringCppImpl() = default;

AsrStringCppImpl::AsrStringCppImpl(const std::filesystem::path& path)
{
    const auto p_string = path.c_str();
#ifdef ASR_WINDOWS
    static_assert(
        std::is_same_v<
            std::remove_reference_t<decltype(path)>::value_type,
            wchar_t>,
        "Not wchar_t in Windows?");
    const auto string_size = ::wcslen(p_string);
    SetW(p_string, string_size);
#else
    static_assert(
        std::is_same_v<
            std::remove_reference_t<decltype(path)>::value_type,
            char>,
        "Not char in Linux?");
    // assume utf-8 in linux!
    SetUtf8(p_string);
#endif // ASR_WINDOWS
}

AsrStringCppImpl::AsrStringCppImpl(
    const U_NAMESPACE_QUALIFIER UnicodeString& impl)
    : impl_{impl}
{
}

AsrStringCppImpl::AsrStringCppImpl(
    U_NAMESPACE_QUALIFIER UnicodeString&& impl) noexcept
    : impl_{std::move(impl)}
{
}

AsrStringCppImpl::~AsrStringCppImpl() = default;

int64_t AsrStringCppImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t AsrStringCppImpl::Release() { return ref_counter_.Release(this); }

AsrResult AsrStringCppImpl::QueryInterface(const AsrGuid& iid, void** pp_object)
{
    return ASR::Utils::QueryInterface<IAsrReadOnlyString>(this, iid, pp_object);
}

const UChar32* AsrStringCppImpl::CBegin()
{
    UpdateUtf32Cache();
    return cached_utf32_string_.cbegin();
}

const UChar32* AsrStringCppImpl::CEnd()
{
    UpdateUtf32Cache();
    return cached_utf32_string_.cend();
}

AsrResult AsrStringCppImpl::SetUtf8(const char* p_string)
{
    InvalidateCache();
    impl_ = U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8(p_string);

    cached_utf8_string_ = p_string;
    ValidateCache<Encode::U8>();

    return ASR_S_OK;
}

AsrResult AsrStringCppImpl::GetUtf8(const char** out_string)
{
    if (IsCacheExpired<Encode::U8>())
    {
        impl_.toUTF8String(cached_utf8_string_);
        ValidateCache<Encode::U8>();
    }
    *out_string = cached_utf8_string_.c_str();
    return ASR_S_OK;
}

AsrResult AsrStringCppImpl::SetUtf16(const char16_t* p_string, size_t length)
{
    InvalidateCache();
    const auto int_length = static_cast<int>(length);
    /**
     *  @brief char16_t* constructor.
     *
     *  @param text The characters to place in the UnicodeString.
     *  @param textLength The number of Unicode characters in text to copy.
     */
    impl_ = {p_string, int_length};
    return ASR_S_OK;
}

AsrResult AsrStringCppImpl::GetUtf16(
    const char16_t** out_string,
    size_t*          out_string_size) noexcept
{
    const auto capacity = impl_.getCapacity();
    *out_string = impl_.getBuffer();
    *out_string_size = impl_.length();
    impl_.releaseBuffer(capacity);
    return ASR_S_OK;
}

ASR_NS_ANONYMOUS_DETAILS_BEGIN

#define ANONYMOUS_DETAILS_MAX_SIZE 4096

/**
 * @brief 返回不带L'\0'字符的空终止字符串长度
 * @param p_wstring
 * @return
 */
template <class = std::enable_if<sizeof(wchar_t) == 4, size_t>>
auto GetStringSize(const wchar_t* p_wstring) -> size_t
{
    for (size_t i = 0; i < ANONYMOUS_DETAILS_MAX_SIZE; ++i)
    {
        if (p_wstring[i] == L'\0')
        {
            return i;
        }
    }

    ASR_CORE_LOG_ERROR(
        "Input string size is larger than expected. Expected max size is " ASR_STR(
            ANONYMOUS_DETAILS_MAX_SIZE) ".");

    const wchar_t char_at_i = p_wstring[ANONYMOUS_DETAILS_MAX_SIZE - 1];
    const auto    char_at_i_value =
        static_cast<uint16_t>(static_cast<uint32_t>(char_at_i));

    // 前导代理
    if (0xD800 <= char_at_i_value && char_at_i_value <= 0xDBFF)
    {
        return ANONYMOUS_DETAILS_MAX_SIZE - 2;
    }
    // 后尾代理
    if (0xDC00 <= char_at_i_value && char_at_i_value <= 0xDFFF)
    {
        return ANONYMOUS_DETAILS_MAX_SIZE - 3;
    }
    return ANONYMOUS_DETAILS_MAX_SIZE - 1;
}

template <class C, class T>
auto SetSwigW(const C* p_wstring, T& u16_buffer)
    -> U_NAMESPACE_QUALIFIER UnicodeString
{
    if constexpr (sizeof(C) == sizeof(char16_t))
    {
        return {p_wstring};
    }
    else if constexpr (sizeof(C) == sizeof(char32_t))
    {
        const auto string_size = GetStringSize(p_wstring);
        const auto p_shadow_string =
            u16_buffer.DiscardAndGetNullTerminateBufferPointer(string_size);
        std::transform(
            p_wstring,
            p_wstring + string_size,
            p_shadow_string,
            [](const wchar_t c)
            {
                char16_t u16_char{};
                // Can be replaced to std::bit_cast
                std::memcpy(&u16_char, &c, sizeof(u16_char));
                return u16_char;
            });
        const auto size = u16_buffer.GetSize();
        const auto int_size = static_cast<int32_t>(size);
        const auto int_length = u_strlen(p_shadow_string);
        return {p_shadow_string, int_length, int_size};
    }
}

ASR_NS_ANONYMOUS_DETAILS_END

AsrResult AsrStringCppImpl::SetSwigW(const wchar_t* p_string)
{
    InvalidateCache();

    impl_ = Details::SetSwigW(p_string, u16_buffer_);

    return ASR_S_OK;
}

AsrResult AsrStringCppImpl::SetW(const wchar_t* p_string, size_t length)
{
    InvalidateCache();

    auto* const p_cached_wchar_string =
        cached_wchar_string_.DiscardAndGetNullTerminateBufferPointer(length);
    const size_t size = length * sizeof(wchar_t);
    std::memcpy(p_cached_wchar_string, p_string, size);
    ValidateCache<Encode::WideChar>();

    const auto i32_length = static_cast<int32_t>(length);
    UErrorCode str_from_wcs_result{};
    int32_t    expected_capacity{0};

    str_from_wcs_result = U_ZERO_ERROR;
    ::u_strFromWCS(
        nullptr,
        0,
        &expected_capacity,
        p_string,
        i32_length,
        &str_from_wcs_result);
    auto* const p_buffer =
        u16_buffer_.DiscardAndGetNullTerminateBufferPointer(expected_capacity);
    const auto i32_buffer_size = static_cast<int32_t>(u16_buffer_.GetSize());
    str_from_wcs_result = U_ZERO_ERROR;
    ::u_strFromWCS(
        p_buffer,
        i32_buffer_size,
        &expected_capacity,
        p_string,
        i32_length,
        &str_from_wcs_result);
    if (str_from_wcs_result != U_ZERO_ERROR)
    {
        ASR_CORE_LOG_ERROR(
            "Error happened when calling u_strFromWCS. Error code = {}.",
            str_from_wcs_result);
        return ASR_E_INVALID_STRING;
    }

    impl_ = {p_buffer, i32_buffer_size, i32_buffer_size};

    return ASR_S_OK;
}

AsrResult AsrStringCppImpl::GetW(const wchar_t** out_wstring)
{
    if (out_wstring == nullptr)
    {
        return ASR_E_INVALID_POINTER;
    }
    if (!IsCacheExpired<Encode::WideChar>())
    {
        *out_wstring = cached_wchar_string_.begin();
        return ASR_S_OK;
    }
    UErrorCode        error_code = U_ZERO_ERROR;
    int32_t           expected_size{};
    const auto        impl_capacity = impl_.getCapacity();
    const auto* const p_impl_buffer = impl_.getBuffer();
    ::u_strToWCS(
        nullptr,
        0,
        &expected_size,
        p_impl_buffer,
        impl_.length(),
        &error_code);
    error_code = U_ZERO_ERROR;
    auto* const p_buffer =
        cached_wchar_string_.DiscardAndGetNullTerminateBufferPointer(
            expected_size);
    const auto int_size = static_cast<int32_t>(cached_wchar_string_.GetSize());
    ::u_strToWCS(
        p_buffer,
        int_size,
        nullptr,
        p_impl_buffer,
        impl_.length(),
        &error_code);
    impl_.releaseBuffer(impl_capacity);
    if (error_code != U_ZERO_ERROR)
    {
        ASR_CORE_LOG_ERROR(
            "Error happened when calling u_strToWCS. Error code = {}.",
            error_code);
        return ASR_E_INVALID_STRING;
    }
    ValidateCache<Encode::WideChar>();
    *out_wstring = p_buffer;
    return ASR_S_OK;
}

/**
 * @brief C++侧使用此接口读取字符串，其它接口均供外部语言调用
 *
 * @return AsrResult
 */
AsrResult AsrStringCppImpl::GetImpl(ICUString** out_icu_string) noexcept
{
    InvalidateCache();
    *out_icu_string = &impl_;
    return ASR_S_OK;
}

AsrResult AsrStringCppImpl::GetImpl(
    const ICUString** out_icu_string) const noexcept
{
    *out_icu_string = &impl_;
    return ASR_S_OK;
}

AsrReadOnlyStringWrapper::AsrReadOnlyStringWrapper(const char* p_u8_string)
{
    ::CreateIAsrReadOnlyStringFromUtf8(p_u8_string, p_impl_.Put());
}

AsrReadOnlyStringWrapper::AsrReadOnlyStringWrapper(const char8_t* u8_string)
{
    ::CreateIAsrReadOnlyStringFromUtf8(
        reinterpret_cast<const char*>(u8_string),
        p_impl_.Put());
}

AsrReadOnlyStringWrapper::AsrReadOnlyStringWrapper(
    const std::string& std_u8_string)
{
    ::CreateIAsrReadOnlyStringFromUtf8(std_u8_string.c_str(), p_impl_.Put());
}

void AsrReadOnlyStringWrapper::GetImpl(IAsrReadOnlyString** pp_impl) const
{
    p_impl_->AddRef();
    *pp_impl = p_impl_.Get();
}

void from_json(nlohmann::json input, AsrReadOnlyStringWrapper& output)
{
    AsrReadOnlyStringWrapper result{
        input.get_ref<const std::string&>().c_str()};
    output = result;
}

void from_json(nlohmann::json input, AsrReadOnlyString& output)
{
    AsrReadOnlyStringWrapper result{
        input.get_ref<const std::string&>().c_str()};
    output = result;
}

AsrReadOnlyStringWrapper::AsrReadOnlyStringWrapper() = default;

AsrReadOnlyStringWrapper::~AsrReadOnlyStringWrapper() = default;

AsrReadOnlyStringWrapper::operator AsrReadOnlyString() const
{
    return AsrReadOnlyString{p_impl_};
}

ASR_NS_BEGIN

namespace Details
{
    template <class T>
    using NullString = std::array<T, 2>;

    class NullStringImpl final : public IAsrReadOnlyString
    {
        static std::string          null_u8string_;
        static NullString<wchar_t>  null_wstring_;
        static NullString<char16_t> null_u16string_;
        static NullString<UChar32>  null_u32string_;

    public:
        int64_t   AddRef() override { return 1; }
        int64_t   Release() override { return 1; }
        AsrResult QueryInterface(const AsrGuid& iid, void** pp_object) override
        {
            return ASR::Utils::QueryInterface<IAsrReadOnlyString>(
                this,
                iid,
                pp_object);
        }

        AsrResult GetUtf8(const char** out_string) override
        {
            *out_string = null_u8string_.c_str();
            return ASR_S_OK;
        }

        AsrResult GetUtf16(
            const char16_t** out_string,
            size_t*          out_string_size) noexcept override
        {
            *out_string = null_u16string_.data();
            *out_string_size = 0;
            return ASR_S_OK;
        };

        AsrResult GetW(const wchar_t** out_wstring) override
        {
            *out_wstring = null_wstring_.data();
            return ASR_S_OK;
        }

        const UChar32* CBegin() override { return null_u32string_.data(); }
        const UChar32* CEnd() override { return null_u32string_.data(); }
    };

    ASR_DEFINE_VARIABLE(NullStringImpl::null_u8string_){};
    ASR_DEFINE_VARIABLE(NullStringImpl::null_wstring_){};
    ASR_DEFINE_VARIABLE(NullStringImpl::null_u16string_){};
    ASR_DEFINE_VARIABLE(NullStringImpl::null_u32string_){};

    NullStringImpl null_asr_string_impl_{};
}

ASR_NS_END

void CreateNullAsrString(IAsrReadOnlyString** pp_out_null_string)
{
    *pp_out_null_string = &ASR::Details::null_asr_string_impl_;
}

ASR_C_API void CreateAsrString(IAsrString** pp_out_string)
{
    try
    {
        *pp_out_string = new AsrStringCppImpl();
        (*pp_out_string)->AddRef();
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        *pp_out_string = nullptr;
    }
}

AsrResult CreateIAsrReadOnlyStringFromChar(
    const char*          p_char_literal,
    IAsrReadOnlyString** pp_out_readonly_string)
{
    try
    {
        auto                      icu_string =
            U_NAMESPACE_QUALIFIER UnicodeString(p_char_literal, "");
        auto                      p_string =
            std::make_unique<AsrStringCppImpl>(std::move(icu_string));
        p_string->AddRef();
        *pp_out_readonly_string = p_string.release();
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult CreateIAsrStringFromUtf8(
    const char*  p_utf8_string,
    IAsrString** pp_out_string)
{
    try
    {
        auto p_string = std::make_unique<AsrStringCppImpl>();
        p_string->SetUtf8(p_utf8_string);
        p_string->AddRef();
        *pp_out_string = p_string.release();
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult CreateIAsrReadOnlyStringFromUtf8(
    const char*          p_utf8_string,
    IAsrReadOnlyString** pp_out_readonly_string)
{
    IAsrString* p_string = nullptr;
    auto        result = CreateIAsrStringFromUtf8(p_utf8_string, &p_string);
    *pp_out_readonly_string = p_string;
    return result;
}

AsrResult CreateIAsrStringFromWChar(
    const wchar_t* p_wstring,
    size_t         length,
    IAsrString**   pp_out_string)
{
    try
    {
        auto p_string = std::make_unique<AsrStringCppImpl>();
        p_string->SetW(p_wstring, length);
        p_string->AddRef();
        *pp_out_string = p_string.release();
        return ASR_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult CreateIAsrReadOnlyStringFromWChar(
    const wchar_t*       p_wstring,
    size_t               length,
    IAsrReadOnlyString** pp_out_readonly_string)
{
    IAsrString* p_string = nullptr;
    auto result = CreateIAsrStringFromWChar(p_wstring, length, &p_string);
    *pp_out_readonly_string = p_string;
    return result;
}
