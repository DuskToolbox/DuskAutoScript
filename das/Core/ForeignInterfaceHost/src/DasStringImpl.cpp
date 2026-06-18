#include <boost/container_hash/hash.hpp>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasConfig.h>
#include <das/DasException.hpp>
#include <limits>
#include <magic_enum_format.hpp>
#include <new>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

bool operator==(DasReadOnlyString lhs, DasReadOnlyString rhs)
{
    const char* p_lhs_str = lhs.GetUtf8();
    const char* p_rhs_str = rhs.GetUtf8();

    return std::strcmp(p_lhs_str, p_rhs_str) == 0;
}

std::size_t std::hash<DasReadOnlyString>::operator()(
    const DasReadOnlyString& string) const noexcept
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

auto(DAS_FMT_NS::formatter<DAS::DasPtr<IDasReadOnlyString>, char>::format)(
    const DAS::DasPtr<IDasReadOnlyString>& p_string,
    format_context&                        ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    if (!p_string) [[unlikely]]
    {
        DAS_CORE_LOG_ERROR("Null DAS::DasPtr<IDasReadOnlyString> found!");
        return ctx.out();
    }

    const char* p_string_data{nullptr};
    const auto  result = p_string->GetUtf8(&p_string_data);
    if (DAS::IsOk(result)) [[likely]]
    {
        return DAS_FMT_NS::format_to(ctx.out(), "{}", p_string_data);
    }
    return DAS_FMT_NS::format_to(
        ctx.out(),
        "(An error occurred when getting string, with error code = {})",
        result);
}

auto(DAS_FMT_NS::formatter<DasReadOnlyString, char>::format)(
    const DasReadOnlyString& das_string,
    format_context&          ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    if (!das_string.Get()) [[unlikely]]
    {
        DAS_CORE_LOG_ERROR("Null DasString found!");
        return ctx.out();
    }
    return DAS_FMT_NS::format_to(ctx.out(), "{}", das_string.GetUtf8());
}

DAS_UTILS_NS_BEGIN

auto ToU8StringWithoutOwnership(IDasReadOnlyString* p_string)
    -> Expected<const char*>
{
    const char* result{nullptr};
    if (const auto get_u8_string_result = p_string->GetUtf8(&result);
        IsFailed(get_u8_string_result))
    {
        DAS_CORE_LOG_ERROR(
            "GetUtf8 failed with error code = {}.",
            get_u8_string_result);
        return tl::make_unexpected(get_u8_string_result);
    }

    return result;
}

auto ToU8String(IDasReadOnlyString* p_string) -> Expected<std::string>
{
    const char* p_u8_string{nullptr};
    if (const auto get_u8_string_result = p_string->GetUtf8(&p_u8_string);
        IsFailed(get_u8_string_result))
    {
        DAS_CORE_LOG_ERROR(
            "GetUtf8 failed with error code = {}.",
            get_u8_string_result);
        return tl::make_unexpected(get_u8_string_result);
    }

    return std::string{p_u8_string};
}

DAS_UTILS_NS_END

bool DAS::DasStringLess::operator()(
    const DAS::DasPtr<IDasReadOnlyString>& lhs,
    const DAS::DasPtr<IDasReadOnlyString>& rhs) const
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

void DasStringCppImpl::InvalidateCache() { is_cache_expired_ = {true, true}; }

void DasStringCppImpl::UpdateUtf32Cache()
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
            DAS_CORE_LOG_ERROR(
                "Error happened when calling UnicodeString::toUTF32. Error code: {}",
                error_code);
        }
        ValidateCache<Encode::U32>();
    }
}

DasStringCppImpl::DasStringCppImpl() = default;

DasStringCppImpl::DasStringCppImpl(const std::filesystem::path& path)
{
    const auto u8_path = path.u8string();
    SetUtf8(reinterpret_cast<const char*>(u8_path.c_str()));
}

DasStringCppImpl::DasStringCppImpl(
    const U_NAMESPACE_QUALIFIER UnicodeString& impl)
    : impl_{impl}
{
}

DasStringCppImpl::DasStringCppImpl(
    U_NAMESPACE_QUALIFIER UnicodeString&& impl) noexcept
    : impl_{std::move(impl)}
{
}

DasStringCppImpl::~DasStringCppImpl() = default;

uint32_t DasStringCppImpl::AddRef() { return ref_counter_.AddRef(); }

uint32_t DasStringCppImpl::Release() { return ref_counter_.Release(this); }

DasResult DasStringCppImpl::QueryInterface(const DasGuid& iid, void** pp_object)
{
    if (pp_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 检查IID_IDasString
    if (iid == DAS_IID_STRING)
    {
        *pp_object = static_cast<IDasString*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasReadOnlyString
    if (iid == DAS_IID_READ_ONLY_STRING)
    {
        *pp_object = static_cast<IDasReadOnlyString*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasBase
    if (iid == DAS_IID_BASE)
    {
        *pp_object = static_cast<IDasBase*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // DasStringCppImpl class guid —— 供 Equals 走 QI 快速路径：直接拿到具体
    // 实现类比较 native ICU UnicodeString，避开 GetUtf16 的 getTerminatedBuffer
    // 开销与编码转换。仅同类实现命中，proxy/mock/null 不响应该 guid。
    if (iid == DasIidOf<DasStringCppImpl>())
    {
        *pp_object = static_cast<DasStringCppImpl*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    *pp_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

const UChar32* DasStringCppImpl::CBegin()
{
    UpdateUtf32Cache();
    return cached_utf32_string_.cbegin();
}

const UChar32* DasStringCppImpl::CEnd()
{
    UpdateUtf32Cache();
    return cached_utf32_string_.cend();
}

DasResult DasStringCppImpl::SetUtf8(const char* p_string)
{
    InvalidateCache();
    impl_ = U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8(p_string);

    cached_utf8_string_ = p_string;
    ValidateCache<Encode::U8>();

    return DAS_S_OK;
}

DasResult DasStringCppImpl::SetUtf8WithLength(
    const char* p_utf8_string,
    size_t      length)
{
    InvalidateCache();
    // ICU StringPiece constructor accepts (ptr, len) without null-termination
    impl_.setTo(
        U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8(
            U_NAMESPACE_QUALIFIER StringPiece{
                p_utf8_string,
                static_cast<int32_t>(length)}));
    // Note: cached_utf8_string_ is not updated here to avoid another allocation
    // The cache will be regenerated on demand via GetUtf8()
    is_cache_expired_[static_cast<std::size_t>(Encode::U8)] = true;
    return DAS_S_OK;
}

DasResult DasStringCppImpl::GetUtf8(const char** out_string)
{
    if (IsCacheExpired<Encode::U8>())
    {
        impl_.toUTF8String(cached_utf8_string_);
        ValidateCache<Encode::U8>();
    }
    *out_string = cached_utf8_string_.c_str();
    return DAS_S_OK;
}

DasResult DasStringCppImpl::SetUtf16(const char16_t* p_string, size_t length)
{
    if (p_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    InvalidateCache();
    const auto int_length = static_cast<int32_t>(length);
    /**
     *  @brief char16_t* constructor.
     *
     *  @param text The characters to place in the UnicodeString.
     *  @param textLength The number of Unicode characters in text to copy.
     */
    impl_ = U_NAMESPACE_QUALIFIER UnicodeString(p_string, int_length);
    return DAS_S_OK;
}

DasResult DasStringCppImpl::GetUtf16(
    const char16_t** out_string,
    size_t*          out_string_size) noexcept
{
    if (out_string == nullptr || out_string_size == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    const auto* terminated =
        reinterpret_cast<const char16_t*>(impl_.getTerminatedBuffer());
    if (terminated == nullptr)
    {
        return DAS_E_OUT_OF_MEMORY;
    }

    *out_string = terminated;
    *out_string_size = static_cast<size_t>(impl_.length());
    return DAS_S_OK;
}

DasBool DasStringCppImpl::Equals(IDasReadOnlyString* other) noexcept
{
    if (other == nullptr)
    {
        return false;
    }
    // QI 快速路径：入参也是 DasStringCppImpl 时，直接比较 native ICU
    // UnicodeString（UTF-16），避开 GetUtf16 的 getTerminatedBuffer 开销与
    // 任何编码转换。
    DasStringCppImpl* cpp_other = nullptr;
    if (other->QueryInterface(
            DasIidOf<DasStringCppImpl>(),
            reinterpret_cast<void**>(&cpp_other))
        == DAS_S_OK)
    {
        const auto equals = impl_ == cpp_other->impl_;
        cpp_other->Release();
        return equals;
    }
    // 退化路径：双方 GetUtf16 比较。GetUtf16 对本地/proxy 都返回各自已有的
    // UTF-16 buffer（零拷贝、零转换），u_strCompare 逐 code unit 比较也不拷贝。
    const char16_t* lhs_utf16 = nullptr;
    const char16_t* rhs_utf16 = nullptr;
    size_t          lhs_size = 0;
    size_t          rhs_size = 0;
    if (GetUtf16(&lhs_utf16, &lhs_size) != DAS_S_OK
        || other->GetUtf16(&rhs_utf16, &rhs_size) != DAS_S_OK)
    {
        return false;
    }
    if (lhs_size != rhs_size)
    {
        return false;
    }
    return u_strCompare(lhs_utf16, lhs_size, rhs_utf16, rhs_size, false) == 0;
}

/**
 * @brief C++侧使用此接口读取字符串，其它接口均供外部语言调用
 *
 * @return DasResult
 */
DasResult DasStringCppImpl::GetImpl(ICUString** out_icu_string) noexcept
{
    InvalidateCache();
    *out_icu_string = &impl_;
    return DAS_S_OK;
}

DasResult DasStringCppImpl::GetImpl(
    const ICUString** out_icu_string) const noexcept
{
    *out_icu_string = &impl_;
    return DAS_S_OK;
}

DasReadOnlyStringWrapper::DasReadOnlyStringWrapper(const char* p_u8_string)
{
    const auto result =
        ::CreateIDasReadOnlyStringFromUtf8(p_u8_string, p_impl_.Put());
    if (DAS::IsFailed(result))
    {
        DAS_THROW_EC(result);
    }
}

DasReadOnlyStringWrapper::DasReadOnlyStringWrapper(const char8_t* u8_string)
{
    const auto result = ::CreateIDasReadOnlyStringFromUtf8(
        reinterpret_cast<const char*>(u8_string),
        p_impl_.Put());
    if (DAS::IsFailed(result))
    {
        DAS_THROW_EC(result);
    }
}

DasReadOnlyStringWrapper::DasReadOnlyStringWrapper(
    const std::string& std_u8_string)
{
    const auto result = ::CreateIDasReadOnlyStringFromUtf8(
        std_u8_string.c_str(),
        p_impl_.Put());
    if (DAS::IsFailed(result))
    {
        DAS_THROW_EC(result);
    }
}

void DasReadOnlyStringWrapper::GetImpl(IDasReadOnlyString** pp_impl) const
{
    p_impl_->AddRef();
    *pp_impl = p_impl_.Get();
}

IDasReadOnlyString* DasReadOnlyStringWrapper::Get() const noexcept
{
    return p_impl_.Get();
}

IDasReadOnlyString** DasReadOnlyStringWrapper::Put() { return p_impl_.Put(); }

template <>
inline std::string DasReadOnlyStringWrapper::To() const
{
    if (!p_impl_)
    {
        return {};
    }
    const char* p_string;
    const auto  result = p_impl_->GetUtf8(&p_string);
    if (DAS::IsFailed(result))
    {
        DAS_THROW_EC(result);
    }
    return {p_string};
}

template <>
inline const char* DasReadOnlyStringWrapper::To() const
{
    if (!p_impl_)
    {
        return nullptr;
    }
    const char* p_string;
    const auto  result = p_impl_->GetUtf8(&p_string);
    if (DAS::IsFailed(result))
    {
        DAS_THROW_EC(result);
    }
    return p_string;
}

template <>
inline const char8_t* DasReadOnlyStringWrapper::To() const
{
    const auto p_string = To<const char*>();
    return reinterpret_cast<const char8_t*>(p_string);
}

template <>
inline IDasReadOnlyString* DasReadOnlyStringWrapper::To() const
{
    return p_impl_.Get();
}

template <>
inline DAS::DasPtr<IDasReadOnlyString> DasReadOnlyStringWrapper::To() const
{
    return p_impl_;
}

template <>
inline DasReadOnlyString DasReadOnlyStringWrapper::To() const
{
    return DasReadOnlyString{p_impl_};
}

DasReadOnlyStringWrapper::DasReadOnlyStringWrapper() = default;

DasReadOnlyStringWrapper::~DasReadOnlyStringWrapper() = default;

void DasReadOnlyStringWrapper::GetTo(const char*& p_u8_string) const
{
    p_u8_string = To<const char*>();
}

void DasReadOnlyStringWrapper::GetTo(const char8_t*& p_u8_string) const
{
    p_u8_string = To<const char8_t*>();
}

void DasReadOnlyStringWrapper::GetTo(std::string& std_u8_string) const
{
    std_u8_string = To<std::string>();
}

void DasReadOnlyStringWrapper::GetTo(
    DAS::DasPtr<IDasReadOnlyString>& p_string) const
{
    p_string = p_impl_;
}

void DasReadOnlyStringWrapper::GetTo(IDasReadOnlyString**& pp_string) const
{
    if (p_impl_)
    {
        DAS::Utils::SetResult(p_impl_, pp_string);
    }
    else
    {
        DAS_CORE_LOG_ERROR("Empty string!");
        CreateNullDasString(pp_string);
    }
}

void DasReadOnlyStringWrapper::GetTo(IDasReadOnlyString*& p_string) const
{
    p_string = p_impl_.Get();
}

DasReadOnlyStringWrapper::operator DasReadOnlyString() const
{
    return DasReadOnlyString{p_impl_};
}

DasReadOnlyStringWrapper::DasReadOnlyStringWrapper(IDasReadOnlyString* p_string)
    : p_impl_{p_string}
{
}

DasReadOnlyStringWrapper::DasReadOnlyStringWrapper(
    const DasReadOnlyString& ref_das_string)
    : p_impl_{ref_das_string.Get()}
{
}

DasReadOnlyStringWrapper::DasReadOnlyStringWrapper(
    DAS::DasPtr<IDasReadOnlyString> p_string)
    : p_impl_{p_string.Get()}
{
}

DAS_NS_BEGIN

namespace Details
{
    template <class T>
    using NullString = std::array<T, 2>;

    class NullStringImpl final : public IDasReadOnlyString
    {
        static std::string          null_u8string_;
        static NullString<char16_t> null_u16string_;
        static NullString<UChar32>  null_u32string_;

    public:
        uint32_t  AddRef() override { return 1; }
        uint32_t  Release() override { return 1; }
        DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (pp_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            // 检查IID_IDasReadOnlyString
            if (iid == DAS_IID_READ_ONLY_STRING)
            {
                *pp_object = static_cast<IDasReadOnlyString*>(this);
                this->AddRef();
                return DAS_S_OK;
            }

            // 检查IID_IDasBase
            if (iid == DAS_IID_BASE)
            {
                *pp_object = static_cast<IDasBase*>(this);
                this->AddRef();
                return DAS_S_OK;
            }

            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetUtf8(const char** out_string) override
        {
            *out_string = null_u8string_.c_str();
            return DAS_S_OK;
        }

        DasResult GetUtf16(
            const char16_t** out_string,
            size_t*          out_string_size) noexcept override
        {
            *out_string = null_u16string_.data();
            *out_string_size = 0;
            return DAS_S_OK;
        };

        const UChar32* CBegin() override { return null_u32string_.data(); }
        const UChar32* CEnd() override { return null_u32string_.data(); }

        // 空串仅与空串（长度 0）相等；other 为 nullptr 时不等。
        DasBool Equals(IDasReadOnlyString* other) noexcept override
        {
            if (other == nullptr)
            {
                return false;
            }
            const char16_t* other_utf16 = nullptr;
            size_t          other_size = 0;
            if (other->GetUtf16(&other_utf16, &other_size) != DAS_S_OK)
            {
                return false;
            }
            return other_size == 0;
        }
    };

    DAS_DEFINE_VARIABLE(NullStringImpl::null_u8string_){};
    DAS_DEFINE_VARIABLE(NullStringImpl::null_u16string_){};
    DAS_DEFINE_VARIABLE(NullStringImpl::null_u32string_){};

    NullStringImpl null_das_string_impl_{};
}

DAS_NS_END

void CreateNullDasString(IDasReadOnlyString** pp_out_null_string)
{
    *pp_out_null_string = &DAS::Details::null_das_string_impl_;
}

DAS_C_API DasResult CreateDasString(IDasString** pp_out_string)
{
    DAS::DasOutPtr<IDasString> result(pp_out_string);

    try
    {
        result.Set(new DasStringCppImpl());
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }

    result.Keep();
    return DAS_S_OK;
}

DasResult CreateIDasReadOnlyStringFromChar(
    const char*          p_char_literal,
    IDasReadOnlyString** pp_out_readonly_string)
{
    try
    {
        auto                      icu_string =
            U_NAMESPACE_QUALIFIER UnicodeString(p_char_literal, "");
        auto                      p_string =
            std::make_unique<DasStringCppImpl>(std::move(icu_string));
        p_string->AddRef();
        *pp_out_readonly_string = p_string.release();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CreateIDasStringFromUtf8(
    const char*  p_utf8_string,
    IDasString** pp_out_string)
{
    try
    {
        auto p_string = std::make_unique<DasStringCppImpl>();
        p_string->SetUtf8(p_utf8_string);
        p_string->AddRef();
        *pp_out_string = p_string.release();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CreateIDasReadOnlyStringFromUtf8(
    const char*          p_utf8_string,
    IDasReadOnlyString** pp_out_readonly_string)
{
    IDasString* p_string = nullptr;
    auto        result = CreateIDasStringFromUtf8(p_utf8_string, &p_string);
    *pp_out_readonly_string = p_string;
    return result;
}

DasResult CreateIDasReadOnlyStringFromUtf8WithLength(
    const char*          p_utf8_string,
    size_t               length,
    IDasReadOnlyString** pp_out_readonly_string)
{
    if (p_utf8_string == nullptr || pp_out_readonly_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto p_string = std::make_unique<DasStringCppImpl>();
        // Use SetUtf8WithLength which accepts non-null-terminated strings via
        // ICU StringPiece
        auto result = p_string->SetUtf8WithLength(p_utf8_string, length);
        if (result != DAS_S_OK)
        {
            return result;
        }
        p_string->AddRef();
        *pp_out_readonly_string = p_string.release();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CreateIDasStringFromUtf16WithLength(
    const DasUtf16CodeUnit* p_utf16_string,
    size_t                  length,
    IDasString**            pp_out_string)
{
    if (p_utf16_string == nullptr || pp_out_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (length > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    DAS::DasOutPtr<IDasString> result(pp_out_string);

    try
    {
        const auto int_length = static_cast<int32_t>(length);
        auto       p_string = std::make_unique<DasStringCppImpl>(
            U_NAMESPACE_QUALIFIER UnicodeString(
                reinterpret_cast<const char16_t*>(p_utf16_string),
                int_length));
        result.Set(p_string.get());
        p_string.release();
        result.Keep();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CreateIDasReadOnlyStringFromUtf16WithLength(
    const DasUtf16CodeUnit* p_utf16_string,
    size_t                  length,
    IDasReadOnlyString**    pp_out_readonly_string)
{
    if (pp_out_readonly_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_readonly_string = nullptr;
    IDasString* p_string = nullptr;
    auto        result =
        CreateIDasStringFromUtf16WithLength(p_utf16_string, length, &p_string);
    if (result != DAS_S_OK)
    {
        return result;
    }

    *pp_out_readonly_string = p_string;
    return DAS_S_OK;
}
