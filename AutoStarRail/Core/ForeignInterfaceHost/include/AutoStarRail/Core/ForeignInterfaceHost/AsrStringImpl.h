#ifndef ASR_CORE_FOREIGNINTERFACEHOST_ASRSTRINGIMPL_H
#define ASR_CORE_FOREIGNINTERFACEHOST_ASRSTRINGIMPL_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/Config.h>
#include <AutoStarRail/Utils/Expected.h>
#include <AutoStarRail/Utils/fmt.h>
#include <array>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <unicode/unistr.h>

bool operator==(AsrReadOnlyString lhs, AsrReadOnlyString rhs);

template <>
struct std::hash<AsrReadOnlyString>
{
    std::size_t operator()(const AsrReadOnlyString& string) const noexcept;
};

template <>
struct ASR_FMT_NS::formatter<ASR::AsrPtr<IAsrReadOnlyString>, char>
    : public formatter<std::string, char>
{
    auto format(
        const ASR::AsrPtr<IAsrReadOnlyString>& p_string,
        format_context&                        ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

template <>
struct ASR_FMT_NS::formatter<AsrReadOnlyString, char>
    : public formatter<std::string, char>
{
    auto format(const AsrReadOnlyString& asr_string, format_context& ctx) const
        -> typename std::remove_reference_t<decltype(ctx)>::iterator;
};

ASR_NS_BEGIN

struct AsrStringLess
{
    bool operator()(
        const AsrPtr<IAsrReadOnlyString>& lhs,
        const AsrPtr<IAsrReadOnlyString>& rhs) const;
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

ASR_NS_END

ASR_UTILS_NS_BEGIN

AsrResult ToPath(
    IAsrReadOnlyString*    p_string,
    std::filesystem::path& ref_out_path);

auto ToU8StringWithoutOwnership(IAsrReadOnlyString* p_string)
    -> Expected<const char*>;

auto ToU8String(IAsrReadOnlyString* p_string) -> Utils::Expected<std::string>;

ASR_UTILS_NS_END

class AsrReadOnlyStringWrapper
{
    mutable ASR::AsrPtr<IAsrReadOnlyString> p_impl_;

public:
    AsrReadOnlyStringWrapper();
    AsrReadOnlyStringWrapper(const char* p_u8_string);
    AsrReadOnlyStringWrapper(const char8_t* u8_string);
    AsrReadOnlyStringWrapper(const std::string& std_u8_string);
    ~AsrReadOnlyStringWrapper();

    operator AsrReadOnlyString() const;
    void GetImpl(IAsrReadOnlyString** pp_impl) const;
};

void from_json(nlohmann::json input, AsrReadOnlyStringWrapper& output);

void from_json(nlohmann::json input, AsrReadOnlyString& output);

// {85648BDC-B73A-41F9-AF7A-71C83085C4B0}
ASR_DEFINE_CLASS_GUID(
    AsrStringCppImpl,
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
class AsrStringCppImpl final : public IAsrString
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
    ASR::Utils::RefCounter<AsrStringCppImpl> ref_counter_{};
    ICUString                                impl_{};
    ASR::Details::DynamicBuffer<char16_t>    u16_buffer_{};
    /**
     * @brief Notice: Boost regex assume std::string contains
     * utf-8 encoding string.
     * @see
     * https://www.boost.org/doc/libs/1_82_0/libs/regex/doc/html/boost_regex/ref/non_std_strings/icu/unicode_algo.html
     */
    std::string                          cached_utf8_string_{};
    ASR::Details::DynamicBuffer<UChar32> cached_utf32_string_{};
    ASR::Details::DynamicBuffer<wchar_t> cached_wchar_string_{};

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
    AsrStringCppImpl();
    explicit AsrStringCppImpl(const std::filesystem::path& path);
    explicit AsrStringCppImpl(const U_NAMESPACE_QUALIFIER UnicodeString& impl);
    explicit AsrStringCppImpl(
        U_NAMESPACE_QUALIFIER UnicodeString&& impl) noexcept;
    ~AsrStringCppImpl();
    // * IAsrBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    AsrResult QueryInterface(const AsrGuid& iid, void** pp_object) override;
    // * IAsrReadOnlyString
    const UChar32* CBegin() override;
    const UChar32* CEnd() override;
    // * IAsrString
    AsrResult SetUtf8(const char* p_string) override;
    AsrResult GetUtf8(const char** out_string) override;
    AsrResult SetUtf16(const char16_t* p_string, size_t length) override;
    AsrResult GetUtf16(
        const char16_t** out_string,
        size_t*          out_string_size) noexcept override;
    /**
     * @brief 接受一串外部为wchar_t的UTF-16编码的字符串
     *
     * @param p_string
     * @return ASR_METHOD
     */
    AsrResult SetSwigW(const wchar_t* p_string) override;
    AsrResult SetW(const wchar_t* p_string, size_t length) override;
    /**
     * @brief 在Windows下返回UTF-16 ，在Linux下返回UTF-32的字符串
     * * C# is using this function.
     *
     * @param p_string
     * @return ASR_METHOD
     */
    AsrResult GetW(const wchar_t** out_wstring) override;
    // * AsrStringCppImpl
    AsrResult GetImpl(ICUString** out_icu_string) noexcept;
    AsrResult GetImpl(const ICUString** out_icu_string) const noexcept;
};

#endif // ASR_CORE_FOREIGNINTERFACEHOST_ASRSTRINGIMPL_H
