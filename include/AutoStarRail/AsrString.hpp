#ifndef ASR_STRING_HPP
#define ASR_STRING_HPP

#include <AutoStarRail/AsrPtr.hpp>
#include <AutoStarRail/IAsrBase.h>

// {C09E276A-B824-4667-A504-7609B4B7DD28}
ASR_DEFINE_GUID(
    ASR_IID_READ_ONLY_STRING,
    IAsrReadOnlyString,
    0xc09e276a,
    0xb824,
    0x4667,
    0xa5,
    0x4,
    0x76,
    0x9,
    0xb4,
    0xb7,
    0xdd,
    0x28)
SWIG_IGNORE(IAsrReadOnlyString)
ASR_INTERFACE IAsrReadOnlyString : public IAsrBase
{
    // * Python
    ASR_METHOD GetUtf8(const char** out_string) = 0;
    // * Java
    ASR_METHOD GetUtf16(
        const char16_t** out_string,
        size_t*          out_string_size) noexcept = 0;
    // * C#
    /**
     * @brief 在Windows下返回UTF-16字符串，在Linux下返回UTF-32字符串
     *
     * @param p_string
     * @return ASR_METHOD
     */
    ASR_METHOD GetW(const wchar_t**) = 0;
    // * C++
    virtual const int32_t* CBegin() = 0;
    virtual const int32_t* CEnd() = 0;
};

// {B1F93FD0-B818-448D-A58C-493DCBDFB781}
ASR_DEFINE_GUID(
    ASR_IID_STRING,
    IAsrString,
    0xb1f93fd0,
    0xb818,
    0x448d,
    0xa5,
    0x8c,
    0x49,
    0x3d,
    0xcb,
    0xdf,
    0xb7,
    0x81);
SWIG_IGNORE(IAsrString)
ASR_INTERFACE IAsrString : public IAsrReadOnlyString
{
    // * Python
    ASR_METHOD SetUtf8(const char* p_string) = 0;
    // * Java
    ASR_METHOD SetUtf16(const char16_t* p_string, size_t length) = 0;
    // * C#
    /**
     * @brief 接受一串字符类型为wchar_t的UTF-16编码的字符串
     *
     * @param p_string
     * @return ASR_METHOD
     */
    ASR_METHOD SetSwigW(const wchar_t* p_string) = 0;
    // * C++
    /**
     * @brief 在Windows下接收UTF-16字符串，在Linux下接收UTF-32字符串。
     *
     * @param p_string
     * @param length
     * Unicode码元的数量。例如：“侮”（U+2F805）是2个码元。建议使用wcslen获取。
     * @return ASR_METHOD
     */
    ASR_METHOD SetW(const wchar_t* p_string, size_t length) = 0;
};

#ifndef SWIG

ASR_C_API void CreateNullAsrString(IAsrReadOnlyString** pp_out_null_string);

ASR_C_API void CreateAsrString(IAsrString** pp_out_string);

#endif // SWIG

#ifndef SWIG
#define ASR_STRING_ENABLE_WHEN_CPP
#endif // SWIG

/**
 * @brief
 * ! 此类通过宏定义控制对SWIG公开的函数，
 *
 */
class AsrReadOnlyString
{
    ASR::AsrPtr<IAsrReadOnlyString> p_impl_{
        []
        {
            ASR::AsrPtr<IAsrReadOnlyString> result{};
            ::CreateNullAsrString(result.Put());
            return result;
        }()};
    using Impl = decltype(p_impl_);

public:
    AsrReadOnlyString() = default;
#ifndef SWIG
    explicit AsrReadOnlyString(ASR::AsrPtr<IAsrString> p_impl)
        : p_impl_{std::move(p_impl)}
    {
    }

    explicit(false) AsrReadOnlyString(IAsrReadOnlyString* p_impl)
        : p_impl_{p_impl}
    {
    }

    explicit AsrReadOnlyString(ASR::AsrPtr<IAsrReadOnlyString> p_impl)
        : p_impl_{std::move(p_impl)}
    {
    }

    AsrReadOnlyString& operator=(ASR::AsrPtr<IAsrReadOnlyString> p_impl)
    {
        p_impl_ = std::move(p_impl);
        return *this;
    }

    AsrReadOnlyString& operator=(IAsrReadOnlyString* p_impl)
    {
        p_impl_ = {p_impl};
        return *this;
    }

    explicit operator IAsrReadOnlyString*() const noexcept
    {
        if (p_impl_)
        {
            p_impl_->AddRef();
        }
        return p_impl_.Get();
    }

    const int32_t* cbegin() const { return p_impl_->CBegin(); };

    const int32_t* cend() const { return p_impl_->CEnd(); };

    void GetImpl(IAsrReadOnlyString** pp_out_readonly_string) const
    {
        const auto result = p_impl_.Get();
        *pp_out_readonly_string = result;
        result->AddRef();
    };

    IAsrReadOnlyString* Get() const noexcept { return p_impl_.Get(); }
#endif // SWIG

/**
 * @brief
 * 从其它语言运行时获得UTF-8字符串
    Get时也使用UTF-8字符串
    当前使用这一策略的语言：Python
 *
 */
#if defined(ASR_STRING_ENABLE_WHEN_CPP) || defined(SWIGPYTHON)
    AsrReadOnlyString(const char* p_utf8_string)
    {
        IAsrString* p_string;
        CreateAsrString(&p_string);
        p_string->SetUtf8(p_utf8_string);
        p_impl_ = Impl::Attach(p_string);
    }

    const char* GetUtf8() const
    {
        const char* result;
        p_impl_->GetUtf8(&result);
        return result;
    }
#endif // defined(ASR_STRING_ENABLE_WHEN_CPP) || defined(SWIGPYTHON)

/**
 * @brief
 * 从其它语言运行时获得UTF-16的字符串，但是外层使用wchar_t包装。
    Get时在Win32环境下返回UTF-16，在Linux环境下返回UTF-32。
    当前使用这一策略的语言：C#
 *
 */
#if defined(ASR_STRING_ENABLE_WHEN_CPP) || defined(SWIGCSHARP)
    AsrReadOnlyString(const wchar_t* p_wstring)
    {
        IAsrString* p_string;
        CreateAsrString(&p_string);
        p_string->SetSwigW(p_wstring);
        p_impl_ = Impl::Attach(p_string);
    }

    const wchar_t* GetW() const
    {
        const wchar_t* p_wstring = nullptr;
        p_impl_->GetW(&p_wstring);
        return p_wstring;
    }

#endif // defined(ASR_STRING_ENABLE_WHEN_CPP) || defined(SWIGCSHARP)

/**
 * @brief 由于SWIG对于Java支持可能存在缺陷，这一功能由本项目实现
 *
 */
#if defined(ASR_STRING_ENABLE_WHEN_CPP) || defined(SWIGJAVA)
    AsrReadOnlyString(const char16_t* p_u16string, size_t length)
    {
        IAsrString* p_string;
        CreateAsrString(&p_string);
        p_string->SetUtf16(p_u16string, length);
        p_impl_ = Impl::Attach(p_string);
    }

    void GetUtf16(const char16_t** out_string, size_t* out_string_size) const
    {
        p_impl_->GetUtf16(out_string, out_string_size);
    }
#endif // defined(ASR_STRING_ENABLE_WHEN_CPP) || defined(SWIGJAVA)
};

#ifndef SWIG

ASR_C_API AsrResult CreateIAsrReadOnlyStringFromChar(
    const char*          p_char_literal,
    IAsrReadOnlyString** pp_out_readonly_string);

ASR_C_API AsrResult
CreateIAsrStringFromUtf8(const char* p_utf8_string, IAsrString** pp_out_string);

ASR_C_API AsrResult CreateIAsrReadOnlyStringFromUtf8(
    const char*          p_utf8_string,
    IAsrReadOnlyString** pp_out_readonly_string);

/**
 * @brief same as ASR_METHOD IAsrString::SetW(const wchar_t* p_string,
 * size_t length) = 0
 *
 * @param p_wstring
 * @param size
 * @param pp_out_string
 * @return ASR_C_API
 */
ASR_C_API AsrResult CreateIAsrStringFromWChar(
    const wchar_t* p_wstring,
    size_t         length,
    IAsrString**   pp_out_string);

/**
 * @See CreateIAsrStringFromWChar
 */
ASR_C_API AsrResult CreateIAsrReadOnlyStringFromWChar(
    const wchar_t*       p_wstring,
    size_t               length,
    IAsrReadOnlyString** pp_out_readonly_string);

#endif // SWIG

ASR_DEFINE_RET_TYPE(AsrRetReadOnlyString, AsrReadOnlyString);

ASR_API AsrReadOnlyString AsrGuidToString(const AsrGuid& guid);

#endif // ASR_STRING_HPP