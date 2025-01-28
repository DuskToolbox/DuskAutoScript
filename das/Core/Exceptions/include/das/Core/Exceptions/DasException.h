#ifndef DAS_CORE_EXCEPTIONS_DASEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_DASEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <das/IDasTypeInfo.h>
#include <exception>
#include <variant>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class borrow_t
{
};

struct DasExceptionSourceInfo
{
    const char* file;
    int         line;
    const char* function;
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
        ::Das::Core::DasExceptionSourceInfo __das_internal_source_location{    \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::Das::Core::DasException::DasException::Throw(                        \
            error_code,                                                        \
            &__das_internal_source_location);                                  \
    }

#define DAS_THROW_EC_EX(error_code, p_type_info)                               \
    {                                                                          \
        ::Das::Core::DasExceptionSourceInfo __das_internal_source_location{    \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::Das::Core::DasException::DasException::Throw(                        \
            error_code,                                                        \
            p_type_info,                                                       \
            &__das_internal_source_location);                                  \
    }

#define DAS_THROW_MSG(error_code, error_message)                               \
    {                                                                          \
        ::Das::Core::DasExceptionSourceInfo __das_internal_source_location{    \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::Das::Core::DasException::DasException::Throw(                        \
            error_code,                                                        \
            error_message,                                                     \
            &__das_internal_source_location);                                  \
    }

class DAS_API DasException final : public std::exception
{
    DasResult                              error_code_;
    std::variant<const char*, std::string> common_string_;

    using Base = std::runtime_error;

    static void ThrowDefault(DasResult error_code);

    DasException(DasResult error_code, std::string&& string);
    DasException(DasResult error_code, const char* p_string, borrow_t);

public:
    static void Throw(
        DasResult               error_code,
        DasExceptionSourceInfo* p_source_info);
    static void Throw(
        DasResult               error_code,
        IDasTypeInfo*           p_type_info,
        DasExceptionSourceInfo* p_source_info);
    static void Throw(
        DasResult               error_code,
        IDasSwigTypeInfo*       p_type_info,
        DasExceptionSourceInfo* p_source_info);
    static void Throw(
        DasResult               error_code,
        const std::string&      ex_message,
        DasExceptionSourceInfo* p_source_info);
    // ex message 支持p_type_info

    [[nodiscard]]
    const char* what() const noexcept override;
    auto        GetErrorCode() const noexcept -> DasResult;
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_DASEXCEPTION_H
