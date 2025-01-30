#ifndef DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGIMPL_H

#include <array>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Config.h>
#include <das/Utils/Expected.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <unicode/unistr.h>

bool operator==(DasReadOnlyString lhs, DasReadOnlyString rhs);

template <>
struct std::hash<DasReadOnlyString>
{
    std::size_t operator()(const DasReadOnlyString& string) const noexcept;
};

template <>
struct DAS_FMT_NS::formatter<DAS::DasPtr<IDasReadOnlyString>, char>
    : public formatter<std::string, char>
{
    auto format(
        const DAS::DasPtr<IDasReadOnlyString>& p_string,
        format_context&                        ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

template <>
struct DAS_FMT_NS::formatter<DasReadOnlyString, char>
    : public formatter<std::string, char>
{
    auto format(const DasReadOnlyString& das_string, format_context& ctx) const
        -> typename std::remove_reference_t<decltype(ctx)>::iterator;
};

DAS_NS_BEGIN

struct DasStringLess
{
    bool operator()(
        const DasPtr<IDasReadOnlyString>& lhs,
        const DasPtr<IDasReadOnlyString>& rhs) const;
};

namespace Details
{
    template <class T>
    class DynamicBuffer
    {
        std::size_t          size_{0};
        std::unique_ptr<T[]> up_data_{nullptr};

    public:
        DynamicBuffer() = default;
        DynamicBuffer(DynamicBuffer&& other) noexcept
            : size_{other.size_}, up_data_{std::move(other.up_data_)}
        {
        }

        static DynamicBuffer Attach(T* p_data, std::size_t size_used)
        {

            auto result = DynamicBuffer();
            result.up_data_ = std::unique_ptr<T[]>(p_data);
            result.size_ = size_used;
        }

        T* DiscardAndGetNullTerminateBufferPointer(std::size_t new_size)
        {
            if (new_size > size_)
            {
                const auto size_with_null_character = new_size + 1;
                up_data_ = std::make_unique<T[]>(size_with_null_character);
                size_ = size_with_null_character;
            }
            else
            {
                up_data_.get()[new_size] = 0;
            }
            return up_data_.get();
        }

        T* begin() noexcept { return up_data_.get(); }

        T* end() noexcept { return up_data_.get() + size_; }

        [[nodiscard]]
        T* cbegin() const noexcept
        {
            return up_data_.get();
        }

        [[nodiscard]]
        T* cend() const noexcept
        {
            return up_data_.get() + size_;
        }

        [[nodiscard]]
        size_t GetSize() const noexcept
        {
            return size_;
        }
    };
} // namespace Details

DAS_NS_END

DAS_UTILS_NS_BEGIN

auto ToU8StringWithoutOwnership(IDasReadOnlyString* p_string)
    -> Expected<const char*>;

auto ToU8String(IDasReadOnlyString* p_string) -> Utils::Expected<std::string>;

DAS_UTILS_NS_END

class DasReadOnlyStringWrapper
{
    mutable DAS::DasPtr<IDasReadOnlyString> p_impl_{
        []
        {
            IDasReadOnlyString* result = nullptr;
            CreateNullDasString(&result);
            return result;
        }()};

    template <class T>
    T To() const;

public:
    DasReadOnlyStringWrapper();
    DasReadOnlyStringWrapper(const char* p_u8_string);
    DasReadOnlyStringWrapper(const char8_t* u8_string);
    DasReadOnlyStringWrapper(const std::string& std_u8_string);
    DasReadOnlyStringWrapper(IDasReadOnlyString* p_string);
    DasReadOnlyStringWrapper(DAS::DasPtr<IDasReadOnlyString> p_string);
    DasReadOnlyStringWrapper(const DasReadOnlyString& ref_das_string);
    ~DasReadOnlyStringWrapper();

    void GetTo(const char*& p_u8_string) const;
    void GetTo(const char8_t*& p_u8_string) const;
    void GetTo(std::string& std_u8_string) const;
    void GetTo(DAS::DasPtr<IDasReadOnlyString>& p_string) const;
    void GetTo(IDasReadOnlyString**& pp_string) const;
    void GetTo(IDasReadOnlyString*& p_string) const;

    operator DasReadOnlyString() const;
    void                 GetImpl(IDasReadOnlyString** pp_impl) const;
    IDasReadOnlyString*  Get() const noexcept;
    IDasReadOnlyString** Put();
};

void from_json(nlohmann::json input, DasReadOnlyStringWrapper& output);

void from_json(nlohmann::json input, DasReadOnlyString& output);

// {85648BDC-B73A-41F9-AF7A-71C83085C4B0}
DAS_DEFINE_CLASS_GUID(
    DasStringCppImpl,
    0x85648bdc,
    0xb73a,
    0x41f9,
    0xaf,
    0x7a,
    0x71,
    0xc8,
    0x30,
    0x85,
    0xc4,
    0xb0);
/**
 * @brief 内部使用icu存储来自各个语言字符串的实现接口
 * ! 除了GetImpl()函数和后续声明的方法外，C++侧不应该调用任何其它方法
 *
 */
class DasStringCppImpl final : public IDasString
{
public:
    using ICUString = U_NAMESPACE_QUALIFIER UnicodeString;

    enum class Encode
    {
        U8 = 0,
        U32 = 1,
        WideChar = 2,
    };

private:
    DAS::Utils::RefCounter<DasStringCppImpl> ref_counter_{};
    ICUString                                impl_{};
    DAS::Details::DynamicBuffer<char16_t>    u16_buffer_{};
    /**
     * @brief Notice: Boost regex assume std::string contains
     * utf-8 encoding string.
     * @see
     * https://www.boost.org/doc/libs/1_82_0/libs/regex/doc/html/boost_regex/ref/non_std_strings/icu/unicode_algo.html
     */
    std::string                          cached_utf8_string_{};
    DAS::Details::DynamicBuffer<UChar32> cached_utf32_string_{};
    DAS::Details::DynamicBuffer<wchar_t> cached_wchar_string_{};

    std::array<bool, 3> is_cache_expired_{true, true, true};

    template <Encode E>
    bool IsCacheExpired() const noexcept
    {
        return is_cache_expired_[static_cast<std::size_t>(E)];
    }

    template <Encode E>
    void ValidateCache()
    {
        is_cache_expired_[static_cast<std::size_t>(E)] = false;
    }

    void InvalidateCache();
    void UpdateUtf32Cache();

public:
    DasStringCppImpl();
    explicit DasStringCppImpl(const std::filesystem::path& path);
    explicit DasStringCppImpl(const U_NAMESPACE_QUALIFIER UnicodeString& impl);
    explicit DasStringCppImpl(
        U_NAMESPACE_QUALIFIER UnicodeString&& impl) noexcept;
    ~DasStringCppImpl();
    // * IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    // * IDasReadOnlyString
    const UChar32* CBegin() override;
    const UChar32* CEnd() override;
    // * IDasString
    DasResult SetUtf8(const char* p_string) override;
    DasResult GetUtf8(const char** out_string) override;
    DasResult SetUtf16(const char16_t* p_string, size_t length) override;
    DasResult GetUtf16(
        const char16_t** out_string,
        size_t*          out_string_size) noexcept override;
    /**
     * @brief 接受一串外部为wchar_t的UTF-16编码的字符串
     *
     * @param p_string
     * @return DAS_METHOD
     */
    DasResult SetSwigW(const wchar_t* p_string) override;
    DasResult SetW(const wchar_t* p_string, size_t length) override;
    /**
     * @brief 在Windows下返回UTF-16 ，在Linux下返回UTF-32的字符串
     * * C# is using this function.
     *
     * @param p_string
     * @return DAS_METHOD
     */
    DasResult GetW(const wchar_t** out_wstring) override;
    // * DasStringCppImpl
    DasResult GetImpl(ICUString** out_icu_string) noexcept;
    DasResult GetImpl(const ICUString** out_icu_string) const noexcept;
};

namespace nlohmann
{
    template <>
    struct adl_serializer<DasReadOnlyStringWrapper>
    {
        static void to_json(
            json&                           j,
            const DasReadOnlyStringWrapper& das_string);

        static void from_json(
            const json&               j,
            DasReadOnlyStringWrapper& das_string);
    };

    template <>
    struct adl_serializer<DasReadOnlyString>
    {
        static void to_json(json& j, const DasReadOnlyString& das_string);

        static void from_json(const json& j, DasReadOnlyString& das_string);
    };

    template <>
    struct adl_serializer<DAS::DasPtr<IDasReadOnlyString>>
    {
        static void to_json(
            json&                                  j,
            const DAS::DasPtr<IDasReadOnlyString>& p_das_string);

        static void from_json(
            const json&                      j,
            DAS::DasPtr<IDasReadOnlyString>& p_das_string);
    };
}

#endif // DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGIMPL_H
