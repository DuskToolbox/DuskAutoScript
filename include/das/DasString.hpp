#ifndef DAS_STRING_HPP
#define DAS_STRING_HPP

#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <string>


// {C09E276A-B824-4667-A504-7609B4B7DD28}
DAS_DEFINE_GUID(
    DAS_IID_READ_ONLY_STRING,
    IDasReadOnlyString,
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
SWIG_IGNORE(IDasReadOnlyString)
DAS_INTERFACE IDasReadOnlyString : public IDasBase
{
    // * Python
    DAS_METHOD GetUtf8(const char** out_string) = 0;
    // * Java
    DAS_METHOD GetUtf16(
        const char16_t** out_string,
        size_t*          out_string_size) noexcept = 0;
    // * C#
    /**
     * @brief 在Windows下返回UTF-16字符串，在Linux下返回UTF-32字符串
     *
     * @param p_string
     * @return DAS_METHOD
     */
    DAS_METHOD GetW(const wchar_t**) = 0;
    // * C++
    virtual const int32_t* CBegin() = 0;
    virtual const int32_t* CEnd() = 0;
};

// {B1F93FD0-B818-448D-A58C-493DCBDFB781}
DAS_DEFINE_GUID(
    DAS_IID_STRING,
    IDasString,
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
SWIG_IGNORE(IDasString)
DAS_INTERFACE IDasString : public IDasReadOnlyString
{
    // * Python
    DAS_METHOD SetUtf8(const char* p_string) = 0;
    // * Java
    DAS_METHOD SetUtf16(const char16_t* p_string, size_t length) = 0;
    // * C#
    /**
     * @brief 接受一串字符类型为wchar_t的UTF-16编码的字符串
     *
     * @param p_string
     * @return DAS_METHOD
     */
    DAS_METHOD SetSwigW(const wchar_t* p_string) = 0;
    // * C++
    /**
     * @brief 在Windows下接收UTF-16字符串，在Linux下接收UTF-32字符串。
     *
     * @param p_string
     * @param length
     * Unicode码元的数量。例如：“侮”（U+2F805）是2个码元。建议使用wcslen获取。
     * @return DAS_METHOD
     */
    DAS_METHOD SetW(const wchar_t* p_string, size_t length) = 0;
};

#ifndef SWIG

DAS_C_API void CreateNullDasString(IDasReadOnlyString** pp_out_null_string);

DAS_C_API void CreateDasString(IDasString** pp_out_string);

DAS_C_API DasResult CreateIDasReadOnlyStringFromChar(
    const char*          p_char_literal,
    IDasReadOnlyString** pp_out_readonly_string);

DAS_C_API DasResult
CreateIDasStringFromUtf8(const char* p_utf8_string, IDasString** pp_out_string);

DAS_C_API DasResult CreateIDasReadOnlyStringFromUtf8(
    const char*          p_utf8_string,
    IDasReadOnlyString** pp_out_readonly_string);

/**
 * @brief same as DAS_METHOD IDasString::SetW(const wchar_t* p_string,
 * size_t length) = 0
 *
 * @param p_wstring
 * @param size
 * @param pp_out_string
 * @return DAS_C_API
 */
DAS_C_API DasResult CreateIDasStringFromWChar(
    const wchar_t* p_wstring,
    size_t         length,
    IDasString**   pp_out_string);

/**
 * @See CreateIDasStringFromWChar
 */
DAS_C_API DasResult CreateIDasReadOnlyStringFromWChar(
    const wchar_t*       p_wstring,
    size_t               length,
    IDasReadOnlyString** pp_out_readonly_string);

#endif // SWIG

#ifndef SWIG
#define DAS_STRING_ENABLE_WHEN_CPP
#endif // SWIG

/**
 * @brief
 * ! 此类通过宏定义控制对SWIG公开的函数，
 *
 */
class DasReadOnlyString
{
    DAS::DasPtr<IDasReadOnlyString> p_impl_{
        []
        {
            DAS::DasPtr<IDasReadOnlyString> result{};
            ::CreateNullDasString(result.Put());
            return result;
        }()};
    using Impl = decltype(p_impl_);

public:
    DasReadOnlyString() = default;
#ifndef SWIG
    explicit DasReadOnlyString(DAS::DasPtr<IDasString> p_impl)
        : p_impl_{std::move(p_impl)}
    {
    }

    explicit(false) DasReadOnlyString(IDasReadOnlyString* p_impl)
        : p_impl_{p_impl}
    {
    }

    explicit DasReadOnlyString(DAS::DasPtr<IDasReadOnlyString> p_impl)
        : p_impl_{std::move(p_impl)}
    {
    }

    DasReadOnlyString& operator=(DAS::DasPtr<IDasReadOnlyString> p_impl)
    {
        p_impl_ = std::move(p_impl);
        return *this;
    }

    DasReadOnlyString& operator=(IDasReadOnlyString* p_impl)
    {
        p_impl_ = {p_impl};
        return *this;
    }

    explicit operator IDasReadOnlyString*() const noexcept
    {
        if (p_impl_)
        {
            p_impl_->AddRef();
        }
        return p_impl_.Get();
    }

    const int32_t* cbegin() const { return p_impl_->CBegin(); };

    const int32_t* cend() const { return p_impl_->CEnd(); };

    void GetImpl(IDasReadOnlyString** pp_out_readonly_string) const
    {
        const auto result = p_impl_.Get();
        *pp_out_readonly_string = result;
        result->AddRef();
    };

    IDasReadOnlyString* Get() const noexcept { return p_impl_.Get(); }

    static DasReadOnlyString FromUtf8(
        const char* p_u8_string,
        DasResult*  p_out_result)
    {
        DasReadOnlyString               result{};
        DAS::DasPtr<IDasReadOnlyString> p_result{};
        const auto                      create_result =
            ::CreateIDasReadOnlyStringFromUtf8(p_u8_string, p_result.Put());
        if (p_out_result)
        {
            *p_out_result = create_result;
        }
        result.p_impl_ = std::move(p_result);
        return result;
    }

    static DasReadOnlyString FromUtf8(
        const std::string& u8_string,
        DasResult*         p_out_result)
    {
        return FromUtf8(u8_string.c_str(), p_out_result);
    }
#endif // SWIG

/**
 * @brief
 * 从其它语言运行时获得UTF-8字符串
    Get时也使用UTF-8字符串
    当前使用这一策略的语言：Python
 *
 */
#if defined(DAS_STRING_ENABLE_WHEN_CPP) || defined(SWIGPYTHON)
    DasReadOnlyString(const char* p_utf8_string)
    {
        IDasString* p_string;
        CreateDasString(&p_string);
        p_string->SetUtf8(p_utf8_string);
        p_impl_ = Impl::Attach(p_string);
    }

    const char* GetUtf8() const
    {
        const char* result;
        p_impl_->GetUtf8(&result);
        return result;
    }
#endif // defined(DAS_STRING_ENABLE_WHEN_CPP) || defined(SWIGPYTHON)

/**
 * @brief
 * 从其它语言运行时获得UTF-16的字符串，但是外层使用wchar_t包装。
    Get时在Win32环境下返回UTF-16，在Linux环境下返回UTF-32。
    当前使用这一策略的语言：C#
 *
 */
#if defined(DAS_STRING_ENABLE_WHEN_CPP) || defined(SWIGCSHARP)
    DasReadOnlyString(const wchar_t* p_wstring)
    {
        IDasString* p_string;
        CreateDasString(&p_string);
        p_string->SetSwigW(p_wstring);
        p_impl_ = Impl::Attach(p_string);
    }

    const wchar_t* GetW() const
    {
        const wchar_t* p_wstring = nullptr;
        p_impl_->GetW(&p_wstring);
        return p_wstring;
    }

#endif // defined(DAS_STRING_ENABLE_WHEN_CPP) || defined(SWIGCSHARP)

/**
 * @brief 由于SWIG对于Java支持可能存在缺陷，这一功能由本项目实现
 *
 */
#if defined(DAS_STRING_ENABLE_WHEN_CPP) || defined(SWIGJAVA)
    DasReadOnlyString(const char16_t* p_u16string, size_t length)
    {
        IDasString* p_string;
        CreateDasString(&p_string);
        p_string->SetUtf16(p_u16string, length);
        p_impl_ = Impl::Attach(p_string);
    }

    void GetUtf16(const char16_t** out_string, size_t* out_string_size) const
    {
        p_impl_->GetUtf16(out_string, out_string_size);
    }
#endif // defined(DAS_STRING_ENABLE_WHEN_CPP) || defined(SWIGJAVA)
};

DAS_DEFINE_RET_TYPE(DasRetReadOnlyString, DasReadOnlyString);

DAS_API DasReadOnlyString DasGuidToString(const DasGuid& guid);

#endif // DAS_STRING_HPP