#ifndef DAS_DASEXCEPTION_HPP
#define DAS_DASEXCEPTION_HPP

#include <das/DasExport.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <stdexcept>


// Forward declaration
struct IDasTypeInfo;

SWIG_IGNORE(DasExceptionSourceInfo)
struct DasExceptionSourceInfo
{
    const char* file;
    int         line;
    const char* function;
};

struct DasExceptionSourceInfoSwig
{
    const char* file;
    int         line;
    const char* function;

    ~DasExceptionSourceInfoSwig()
    {
        delete file;
        delete function;
    }
};

DAS_SWIG_EXPORT_ATTRIBUTE(IDasExceptionString)
// {6073A186-16C9-41E5-9A02-BE76CCB94951}
DAS_DEFINE_GUID(
    DAS_IID_EXCEPTION_STRING,
    IDasExceptionString,
    0x6073a186,
    0x16c9,
    0x41e5,
    0x9a,
    0x2,
    0xbe,
    0x76,
    0xcc,
    0xb9,
    0x49,
    0x51);
struct IDasExceptionString : IDasBase
{
    SWIG_PRIVATE
    DAS_METHOD GetU8(const char** pp_out_string) = 0;
};

#define DAS_THROW_IF_FAILED_EC(...)                                            \
    {                                                                          \
        if (const auto result = __VA_ARGS__; ::Das::IsFailed(result))          \
        {                                                                      \
            DAS_THROW_EC(result);                                              \
        }                                                                      \
    }

#define DAS_THROW_EC(error_code)                                               \
    {                                                                          \
        ::DasExceptionSourceInfo __das_internal_source_location{               \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::IDasExceptionString* __das_internal_p_string{};                      \
        ::CreateDasExceptionString(                                            \
            error_code,                                                        \
            &__das_internal_source_location,                                   \
            &__das_internal_p_string);                                         \
        throw DasException{error_code, __das_internal_p_string};               \
    }

#define DAS_THROW_EC_EX(error_code, p_type_info)                               \
    {                                                                          \
        ::DasExceptionSourceInfo __das_internal_source_location{               \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::IDasExceptionString* __das_internal_p_string{};                      \
        ::CreateDasExceptionStringWithTypeInfo(                                \
            error_code,                                                        \
            &__das_internal_source_location,                                   \
            p_type_info,                                                       \
            &__das_internal_p_string);                                         \
        throw DasException{error_code, __das_internal_p_string};               \
    }

#define DAS_THROW_MSG(error_code, error_message)                               \
    {                                                                          \
        ::DasExceptionSourceInfo __das_internal_source_location{               \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::IDasExceptionString* __das_internal_p_string{};                      \
        ::CreateDasExceptionStringWithMessage(                                 \
            error_code,                                                        \
            &__das_internal_source_location,                                   \
            error_message,                                                     \
            &__das_internal_p_string);                                         \
        throw DasException{error_code, __das_internal_p_string};               \
    }

// C API 函数声明（实现在 DasExceptionSupport.cpp）
SWIG_IGNORE(CreateDasExceptionString)
DAS_C_API void CreateDasExceptionString(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info,
    IDasExceptionString**   pp_out_handle);

SWIG_IGNORE(CreateDasExceptionStringWithMessage)
DAS_C_API void CreateDasExceptionStringWithMessage(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info,
    const char*             message,
    IDasExceptionString**   pp_out_handle);

SWIG_IGNORE(CreateDasExceptionStringWithTypeInfo)
DAS_C_API void CreateDasExceptionStringWithTypeInfo(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info,
    IDasTypeInfo*           p_type_info,
    IDasExceptionString**   pp_out_handle);

DAS_API IDasExceptionString* CreateDasExceptionStringSwig(
    DasResult                   error_code,
    DasExceptionSourceInfoSwig* p_source_info);

DAS_API IDasExceptionString* CreateDasExceptionStringWithTypeInfoSwig(
    DasResult                   error_code,
    DasExceptionSourceInfoSwig* p_source_info,
    IDasTypeInfo*               p_type_info);

SWIG_IGNORE(std::runtime_error)
class DasException : public std::runtime_error
{
    DasResult error_code_;

    using Base = std::runtime_error;

public:
    DasException(DasResult error_code, IDasExceptionString* p_string)
        : Base{[p_string]
               {
                   const char* result;
                   p_string->GetU8(&result);
                   return result;
               }()},
          error_code_{error_code}
    {
    }
#ifndef SWIG
    DasException(DasResult error_code, std::string&& string)
        : Base{string.c_str()}, error_code_{error_code}
    {
    }

    DasException(DasResult error_code, const char* p_string)
        : Base{p_string}, error_code_{error_code}
    {
    }

    DasException(DasResult error_code, const std::string& message)
        : DasException{error_code, std::string{message}}
    {
    }
#endif // SWIG

    [[nodiscard]]
    auto GetErrorCode() const noexcept -> DasResult
    {
        return error_code_;
    }
};
#endif // DAS_DASEXCEPTION_HPP
